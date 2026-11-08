#!/usr/bin/env python3
"""The MSC service exposes bounded T733 disk operations to one local user.

The Terminal launcher starts the root process. Clients pass already-open regular
files over a Unix socket. The service enumerates userdata for each operation,
checks its geometry and installed system header, and accesses only the fixed FAT
or the final one GiB reserved for logs. Silicium owns transport and rebooting.
"""

import argparse
import array
import ctypes
import fcntl
import json
import os
import plistlib
import re
import signal
import socket
import stat
import struct
import subprocess
import sys
import threading
import time

MIB = 1 << 20
FAT_SIZE = 512 * MIB
FAT_OFFSET = 0x22F400000
PART_SIZE = 117878796288
EXPECTED = {
    "BusProtocol": "USB", "Internal": False, "WholeDisk": False,
    "Writable": True, "IORegistryEntryName": "userdata",
    "PartitionMapPartitionOffset": 9506390016, "Size": PART_SIZE,
    "DeviceBlockSize": 4096,
}


def directory(uid):
    return f"/var/run/t733-msc-{uid}"


def diskutil(*args):
    result = subprocess.run(["/usr/sbin/diskutil", *args], capture_output=True,
                            timeout=60, check=True, env={"PATH": "/usr/bin:/bin"})
    return result.stdout


def discover():
    listing = plistlib.loads(diskutil("list", "-plist", "external", "physical"))
    matches = []
    for disk in listing.get("AllDisksAndPartitions", []):
        for partition in disk.get("Partitions", []):
            if partition.get("Size") != PART_SIZE:
                continue

            identifier = partition.get("DeviceIdentifier", "")
            if not re.fullmatch(r"disk[0-9]+s[0-9]+", identifier):
                continue

            info = plistlib.loads(diskutil("info", "-plist", identifier))
            if not all(info.get(key) == value for key, value in EXPECTED.items()):
                continue

            parent = info.get("ParentWholeDisk", "")
            if re.fullmatch(r"disk[0-9]+", parent):
                matches.append({"partition": identifier, "whole": parent,
                                "mounted": bool(info.get("MountPoint"))})

    return matches


def check_header(fd):
    header = os.pread(fd, 4096, 0)
    if len(header) != 4096 or header[:8] != b"SHNGROOT":
        raise ValueError("Missing installed SHNGROOT header")

    version, count, image_size = struct.unpack_from("<IIQ", header, 8)
    if version != 1 or count != 2:
        raise ValueError("Unsupported SHNGROOT version or window count")

    windows = []
    for index in range(count):
        name, offset, size = struct.unpack_from("<32sQQ", header, 24 + index * 128)
        if offset < 4096 or size == 0 or offset + size > FAT_OFFSET:
            raise ValueError("Invalid installed system window bounds")

        windows.append((name.split(b"\0", 1)[0], offset, size))

    ordered = sorted(windows, key=lambda item: item[1])
    highest = max(offset + size for _, offset, size in windows)
    if ({name for name, _, _ in windows} != {b"OS", b"SystemOS"}
            or ordered[0][1] + ordered[0][2] > ordered[1][1]
            or (highest + MIB - 1) // MIB * MIB != FAT_OFFSET
            or image_size != FAT_OFFSET + FAT_SIZE):
        raise ValueError("Installed system layout does not match T733 FAT")


def check_image(fd):
    bpb = os.pread(fd, 512, 0)
    if (os.fstat(fd).st_size != FAT_SIZE or bpb[510:512] != b"\x55\xaa"
            or bpb[71:82] != b"T733ESP    " or bpb[82:90] != b"FAT32   "):
        raise ValueError("Input must be a complete 512 MiB T733ESP FAT32 image")


def send(connection, value):
    connection.sendall(json.dumps(value).encode() + b"\n")


class Service:
    """Service owns the socket, task history, and serialized disk access.

    Each connection is authenticated with kernel peer credentials. Status requests
    remain available during transfers. A transfer holds the operation lock through
    unmounting, disk access, and flushing; clients own their input/output files.
    """

    def __init__(self, uid):
        self.uid = uid
        self.lock = threading.Lock()
        self.state_lock = threading.Lock()
        self.state = {"operation": "idle", "bytes": 0, "total": 0}
        self.stopping = threading.Event()
        self.log_path = directory(uid) + "/tasks.jsonl"

    def record(self, **event):
        event["time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        with self.state_lock:
            self.state.update(event)
            with open(self.log_path, "a", encoding="utf-8") as stream:
                stream.write(json.dumps(event) + "\n")

    def handle(self, connection):
        descriptors = []
        try:
            uid = ctypes.c_uint()
            gid = ctypes.c_uint()
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.getpeereid(connection.fileno(), ctypes.byref(uid), ctypes.byref(gid)) != 0:
                raise PermissionError("Cannot read peer credentials")

            if uid.value != self.uid:
                raise PermissionError("Client UID is not authorized")

            connection.settimeout(30)
            data, ancillary, flags, _ = connection.recvmsg(8192, socket.CMSG_SPACE(16))
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    values = array.array("i")
                    values.frombytes(payload[:len(payload) // values.itemsize * values.itemsize])
                    descriptors.extend(values)

            if flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC):
                raise ValueError("Request exceeded protocol bounds")

            while not data.endswith(b"\n") and len(data) < 8192:
                chunk = connection.recv(8192 - len(data))
                if not chunk:
                    break

                data += chunk

            request = json.loads(data)
            operation = request["operation"]
            if operation == "status":
                with self.state_lock:
                    send(connection, {"ok": True, "uid": self.uid, **self.state})
                return

            if operation == "logs":
                with self.state_lock:
                    with open(self.log_path, "rb") as stream:
                        stream.seek(max(0, os.fstat(stream.fileno()).st_size - 65536))
                        history = stream.read().decode(errors="replace")
                send(connection, {"ok": True, "logs": history})
                return

            if operation not in {"list", "read-fat", "write-fat", "read-log", "eject", "shutdown"}:
                raise ValueError("Unknown operation")

            if not self.lock.acquire(blocking=False):
                raise RuntimeError("A disk operation is already running")

            try:
                self.execute(connection, request, descriptors)
            finally:
                self.lock.release()
        except Exception as error:
            try:
                send(connection, {"ok": False, "error": str(error)})
            except OSError:
                pass
        finally:
            for fd in descriptors:
                os.close(fd)
            connection.close()

    def execute(self, connection, request, descriptors):
        if self.stopping.is_set():
            raise RuntimeError("Service is stopping")

        operation = request["operation"]
        if operation == "shutdown":
            self.stopping.set()
            send(connection, {"ok": True, "message": "Service stopped"})
            return

        self.record(operation=operation, bytes=0, total=0, result="discovering",
                    device=None, command=None, error=None)
        try:
            matches = discover()
        except Exception as error:
            self.record(result="failed", error=str(error))
            raise

        if operation == "list":
            self.record(result="complete")
            send(connection, {"ok": True, "devices": matches})
            return

        if len(matches) != 1:
            raise ValueError(f"Expected one matching T733 userdata, found {len(matches)}")

        device = matches[0]
        self.record(operation=operation, bytes=0, total=0, device=device, result="running", error=None)
        try:
            if operation == "eject":
                diskutil("eject", device["whole"])
            else:
                self.transfer(connection, operation, request, descriptors, device)

            self.record(result="complete")
        except Exception as error:
            self.record(result="failed", error=str(error))
            raise

        send(connection, {"ok": True, "message": operation + " completed"})

    def transfer(self, connection, operation, request, descriptors, device):
        if len(descriptors) != 1:
            raise ValueError("Exactly one client file descriptor is required")

        client_fd = descriptors[0]
        metadata = os.fstat(client_fd)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_uid != self.uid:
            raise ValueError("Client file must be a regular file owned by the authorized user")

        mode = fcntl.fcntl(client_fd, fcntl.F_GETFL)
        writing = operation == "write-fat"
        if writing:
            if mode & os.O_ACCMODE == os.O_WRONLY:
                raise ValueError("Input file is not readable")
            check_image(client_fd)
        elif mode & os.O_ACCMODE == os.O_RDONLY or mode & os.O_APPEND:
            raise ValueError("Output file must be writable without append mode")

        offset, length = FAT_OFFSET, FAT_SIZE
        if operation == "read-log":
            relative = int(request.get("offset", 0))
            length = int(request.get("length", MIB))
            if relative < 0 or length <= 0 or length > 64 * MIB or relative + length > 1024 * MIB:
                raise ValueError("Log read must stay inside final 1 GiB, at most 64 MiB per request")
            if relative % 4096 or length % 4096:
                raise ValueError("Log offset and length must be aligned to 4096 bytes")
            offset = PART_SIZE - 1024 * MIB + relative

        if device["mounted"]:
            diskutil("unmount", device["partition"])
            info = plistlib.loads(diskutil("info", "-plist", device["partition"]))
            if (not all(info.get(key) == value for key, value in EXPECTED.items())
                    or info.get("MountPoint")):
                raise ValueError("Device changed during preparation or remains mounted")

        disk_fd = os.open("/dev/r" + device["partition"], os.O_RDWR if writing else os.O_RDONLY)
        try:
            if not stat.S_ISCHR(os.fstat(disk_fd).st_mode):
                raise ValueError("Target is not a raw disk character device")
            check_header(disk_fd)
        finally:
            os.close(disk_fd)

        block_size = MIB if offset % MIB == 0 and length % MIB == 0 else 4096
        command = ["/bin/dd", f"bs={block_size}", f"count={length // block_size}"]
        disk_path = "/dev/r" + device["partition"]
        if writing:
            command.extend([f"of={disk_path}", f"seek={offset // block_size}", "conv=notrunc"])
        else:
            command.extend([f"if={disk_path}", f"skip={offset // block_size}"])

        os.lseek(client_fd, 0, os.SEEK_SET)
        self.record(total=length, command=command)
        process = subprocess.Popen(command,
                                   stdin=client_fd if writing else subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL if writing else client_fd,
                                   stderr=subprocess.PIPE)
        position = 0
        while True:
            completed = False
            try:
                _, output = process.communicate(timeout=1)
                completed = True
            except subprocess.TimeoutExpired as error:
                output = error.stderr or b""
                process.send_signal(signal.SIGINFO)

            reports = re.findall(rb"(\d+) bytes transferred", output)
            if reports:
                transferred = int(reports[-1])
                if transferred != position:
                    position = transferred
                    self.record(bytes=position)
                    try:
                        send(connection, {"progress": position, "total": length})
                    except OSError:
                        pass

            if completed:
                break

        if process.returncode != 0 or position != length:
            diagnostic = output.decode(errors="replace").strip()
            raise OSError(f"dd exited {process.returncode} after {position}/{length} bytes: {diagnostic}")

        if writing:
            subprocess.run(["/bin/sync"], check=True, timeout=120)
        else:
            os.ftruncate(client_fd, length)
            os.fsync(client_fd)

    def run(self, detach=False):
        root = directory(self.uid)
        try:
            os.mkdir(root, 0o711)
        except FileExistsError:
            pass

        metadata = os.lstat(root)
        if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != 0 or metadata.st_mode & 0o022:
            raise PermissionError("Service directory must be root-owned and not writable by other users")

        os.chmod(root, 0o711)
        lock_fd = os.open(root + "/lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        path = root + "/service.sock"
        if os.path.lexists(path):
            os.unlink(path)

        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(path)
        os.chown(path, self.uid, -1)
        os.chmod(path, 0o600)
        server.listen(8)
        server.settimeout(1)
        signal.signal(signal.SIGTERM, lambda _number, _frame: self.stopping.set())
        self.record(operation="idle", result="ready")
        if detach:
            output_fd = os.open(root + "/daemon.log", os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
            input_fd = os.open("/dev/null", os.O_RDONLY)
            sys.stdout.flush()
            sys.stderr.flush()
            process = os.fork()
            if process:
                server.close()
                os.close(lock_fd)
                os.close(output_fd)
                os.close(input_fd)
                print(f"T733 MSC is running in the background (PID {process}). You can close this window.", flush=True)
                return

            os.setsid()
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
            os.dup2(input_fd, 0)
            os.dup2(output_fd, 1)
            os.dup2(output_fd, 2)
            os.close(input_fd)
            os.close(output_fd)

        print(f"T733 MSC ready for UID {self.uid}.", flush=True)
        try:
            while not self.stopping.is_set():
                try:
                    connection, _ = server.accept()
                except socket.timeout:
                    continue

                threading.Thread(target=self.handle, args=(connection,)).start()
        except KeyboardInterrupt:
            print("Waiting for the active disk operation before stopping.", flush=True)
        finally:
            self.stopping.set()
            with self.lock:
                server.close()
                os.unlink(path)
            os.close(lock_fd)


def client(args):
    uid = os.getuid()
    if uid == 0:
        raise PermissionError("Run the client as your normal user")
    request = {"operation": args.operation}
    file_fd = None
    try:
        if args.operation in {"write-fat", "read-fat", "read-log"}:
            if args.file is None:
                raise ValueError("This operation requires a file path")
            flags = os.O_RDONLY if args.operation == "write-fat" else os.O_WRONLY | os.O_CREAT | os.O_EXCL
            file_fd = os.open(args.file, flags | os.O_NOFOLLOW, 0o600)
            request.update(offset=args.offset, length=args.length)

        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.connect(directory(uid) + "/service.sock")
            ancillary = []
            if file_fd is not None:
                ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [file_fd]))]
            connection.sendmsg([json.dumps(request).encode() + b"\n"], ancillary)
            success = False
            for line in connection.makefile("rb"):
                response = json.loads(line)
                print(json.dumps(response), flush=True)
                if "ok" in response:
                    success = response["ok"]
            return 0 if success else 1
    finally:
        if file_fd is not None:
            os.close(file_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["serve", "status", "list", "logs", "read-fat", "write-fat", "read-log", "eject", "shutdown"])
    parser.add_argument("file", nargs="?")
    parser.add_argument("--offset", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--length", type=lambda value: int(value, 0), default=MIB)
    parser.add_argument("--detach", action="store_true")
    args = parser.parse_args()
    try:
        if args.operation == "serve":
            uid = int(os.environ.get("SUDO_UID", "0"))
            if os.geteuid() != 0 or uid == 0:
                raise PermissionError("Start through sudo from the normal user's Terminal")
            os.umask(0o077)
            Service(uid).run(args.detach)
            return 0
        return client(args)
    except Exception as error:
        print(json.dumps({"ok": False, "error": str(error)}), flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
