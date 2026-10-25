#!/usr/bin/env python3
"""Connect Apple's LLDB to the T733 debugserver over its dedicated USB Ethernet.

Examples:
  python3 tools/t733-lldb.py --configure --attach SpringBoard
  python3 tools/t733-lldb.py --pid 27 --record logs/springboard-lldb.txt
  python3 tools/t733-lldb.py --launch /usr/sbin/spindump \
      --local-executable /path/to/rootfs/usr/sbin/spindump -- -help

The connection runs asynchronously because Platform's synchronous connection
waits for a stopped process before returning, while this server awaits attach.
PID attachment uses vAttach; name attachment uses waitfor/include-existing so
debugserver itself resolves the name through vAttachOrWait. No platform server
or remote process-list RPC is required. Debugserver's RNBRemote.cpp has neither
qPlatform_shell nor vFile handlers; --launch uses normal debugged process launch.

Sources:
https://github.com/llvm/llvm-project/blob/main/lldb/source/Target/Process.cpp
https://github.com/llvm/llvm-project/blob/main/lldb/source/Plugins/Process/gdb-remote/ProcessGDBRemote.cpp
https://github.com/llvm/llvm-project/blob/main/lldb/tools/debugserver/source/debugserver.cpp
https://github.com/llvm/llvm-project/blob/main/lldb/tools/debugserver/source/RNBRemote.cpp
"""

import argparse
import os
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import sys
import time


HOST_MAC = "02:73:30:00:00:01"
MASKED_MAC = "02:00:00:00:00:00"
USB_SERIAL = "T733DBG001"
NETWORK_SERVICE = "T733 Kernel Log and Ethernet"
HOST_IP = "192.168.73.1"
DEVICE_IP = "192.168.73.2"
NETMASK = "255.255.255.252"
SERVER_PORT = 1234


def network_services():
    result = subprocess.run(["/usr/sbin/networksetup", "-listnetworkserviceorder"],
                            capture_output=True, text=True, check=True)
    services = []
    service = None
    for line in result.stdout.splitlines():
        heading = re.match(r"^\((?:[0-9]+|\*)\) (.+)$", line)
        if heading:
            service = heading.group(1).lstrip("*")
            continue

        device = re.search(r"Hardware Port: (.+), Device: ([^)]+)\)", line)
        if device and service:
            services.append({"service": service, "port": device.group(1),
                             "interface": device.group(2)})
            service = None
    return services


def usb_network_interfaces():
    result = subprocess.run(
        ["/usr/sbin/ioreg", "-a", "-l", "-p", "IOService", "-r", "-c", "IOUSBHostDevice"],
        capture_output=True, check=True)
    inventory = plistlib.loads(result.stdout)

    def descendants(node):
        yield node
        for child in node.get("IORegistryEntryChildren", []):
            yield from descendants(child)

    devices = {}
    for tree in inventory:
        for node in descendants(tree):
            if (node.get("IOObjectClass") == "IOUSBHostDevice"
                    and node.get("USB Serial Number") == USB_SERIAL):
                identity = node.get("IORegistryEntryID")
                devices[identity if identity is not None else id(node)] = node

    if len(devices) > 1:
        raise RuntimeError("Multiple USB devices report " + USB_SERIAL)
    if not devices:
        return set()

    device = next(iter(devices.values()))
    names = {node["BSD Name"] for node in descendants(device)
             if isinstance(node.get("BSD Name"), str)
             and re.fullmatch(r"en[0-9]+", node["BSD Name"])}
    services = network_services()
    if names:
        return names & {entry["interface"] for entry in services}

    # Some registry views omit the network child. The current USB serial and
    # product must match before the exact saved service supplies its interface.
    if device.get("USB Product Name") != NETWORK_SERVICE:
        return set()
    matches = [entry["interface"] for entry in services
               if entry["service"] == NETWORK_SERVICE]
    if len(matches) > 1:
        raise RuntimeError("Multiple saved interfaces match the attached T733 USB device.")
    return set(matches)


def interfaces():
    result = subprocess.run(["/sbin/ifconfig", "-a"], check=True,
                            capture_output=True, text=True)
    entries = []
    current = None
    for line in result.stdout.splitlines():
        heading = re.match(r"^(en[0-9]+):\s", line)
        if line and not line[0].isspace():
            current = None
            if heading:
                current = {"name": heading.group(1), "mac": None, "addresses": []}
                entries.append(current)
        if current is None:
            continue
        mac = re.match(r"\s+ether\s+([0-9a-fA-F:]+)\b", line)
        if mac:
            current["mac"] = mac.group(1).lower()
        address = re.match(r"\s+inet\s+(\S+)\s+netmask\s+(\S+)", line)
        if address:
            current["addresses"].append((address.group(1), address.group(2).lower()))
    matches = [entry for entry in entries if entry["mac"] == HOST_MAC]
    masked = [entry for entry in entries if entry["mac"] == MASKED_MAC]
    if masked:
        usb_names = usb_network_interfaces()
        matches.extend(entry for entry in masked if entry["name"] in usb_names)
    return matches


def find_interface(wait):
    deadline = time.monotonic() + wait
    announced = False
    while True:
        matches = interfaces()
        if len(matches) > 1:
            raise RuntimeError("Multiple interfaces match the T733 USB identity; refusing to choose one.")
        if matches:
            return matches[0]
        if time.monotonic() >= deadline:
            raise RuntimeError("No enX interface matches T733 MAC " + HOST_MAC
                               + " or attached USB serial " + USB_SERIAL)
        if not announced:
            print("Waiting for T733 USB Ethernet " + HOST_MAC, flush=True)
            announced = True
        time.sleep(min(1.0, max(0.0, deadline - time.monotonic())))


def lldb_quote(value):
    if any(ord(char) < 32 for char in value) or "`" in value:
        raise ValueError("LLDB arguments cannot contain control characters or backticks.")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def network_configuration_command(interface):
    matches = [entry["service"] for entry in network_services()
               if entry["interface"] == interface]

    if len(matches) != 1:
        raise RuntimeError("Expected one saved network service for " + interface)

    return ["/usr/sbin/networksetup", "-setmanual", matches[0], HOST_IP, NETMASK, ""]


def commands_for(args):
    platform = "platform select remote-ios"
    if args.sysroot:
        platform += " --sysroot " + lldb_quote(str(args.sysroot.expanduser().resolve()))
    commands = [platform]
    if args.local_executable:
        commands.append("target create --arch arm64 --no-dependents "
                        + lldb_quote(str(args.local_executable.resolve())))
    if args.launch:
        remote = repr(args.launch)
        commands.append(
            "script t = lldb.debugger.GetSelectedTarget(); "
            "f = lldb.SBFileSpec(" + remote + "); "
            "t.GetModuleAtIndex(0).SetPlatformFileSpec(f); "
            "i = t.GetLaunchInfo(); i.SetExecutableFile(f, True); t.SetLaunchInfo(i)")
    commands.append("script lldb.debugger.SetAsync(True)")
    commands.append(f"process connect --plugin gdb-remote connect://{DEVICE_IP}:{SERVER_PORT}")
    commands.append("script lldb.debugger.SetAsync(False)")
    if args.pid:
        commands.append(f"process attach --pid {args.pid}")
    elif args.attach:
        commands.append("process attach --waitfor --include-existing --name "
                        + lldb_quote(args.attach))
    elif args.launch:
        arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
        commands.append("process launch -- " + " ".join(lldb_quote(value) for value in arguments))
    return commands


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--configure", action="store_true",
                        help="Save a manual address on the matching T733 network service, then open LLDB")
    parser.add_argument("--configure-only", action="store_true",
                        help="Configure the matching interface and exit without connecting")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--attach", metavar="NAME",
                        help="Attach by name, including an existing process; wait if absent")
    action.add_argument("--pid", type=int, help="Attach to this remote PID")
    action.add_argument("--launch", metavar="REMOTE_EXECUTABLE",
                        help="Launch an original device tool; requires its local executable")
    parser.add_argument("--local-executable", type=Path,
                        help="Matching local Mach-O for symbols or remote tool launch")
    parser.add_argument("--sysroot", type=Path,
                        help="Matching original OS root or prepared DeviceSupport symbol root")
    parser.add_argument("--profile-seconds", type=int,
                        help="Collect native running CPU/thread/memory counters for this duration, then detach")
    parser.add_argument("--profile-interval", type=int, default=1,
                        help="Native profile interval in whole seconds (default: 1)")
    parser.add_argument("--profile-output", type=Path,
                        help="New directory for profile JSON, CSV and raw records (default: unique out directory)")
    parser.add_argument("--record", type=Path,
                        help="Save the interactive LLDB terminal and remote stdout with macOS script")
    parser.add_argument("--wait", type=float, default=30,
                        help="Seconds to wait for the matching Ethernet interface (default: 30)")
    parser.add_argument("--print-commands", action="store_true",
                        help="Print LLDB commands without accessing interfaces or connecting")
    parser.add_argument("arguments", nargs=argparse.REMAINDER,
                        help="Arguments after -- for the remotely launched tool")
    args = parser.parse_args()
    if args.pid is not None and args.pid <= 0:
        parser.error("--pid must be positive")
    if args.wait < 0:
        parser.error("--wait must be nonnegative")
    if args.arguments and not args.launch:
        parser.error("trailing arguments require --launch")
    if args.launch and (not args.local_executable or not args.launch.startswith("/")):
        parser.error("--launch requires an absolute remote path and --local-executable")
    if args.local_executable and not args.local_executable.is_file():
        parser.error("--local-executable must name the matching local Mach-O file")
    if args.sysroot and not args.sysroot.expanduser().is_dir():
        parser.error("--sysroot must name an existing original OS or symbol directory")
    if args.profile_seconds is not None:
        if not (args.pid or args.attach) or args.launch:
            parser.error("--profile-seconds requires --pid or --attach for an existing process")
        if args.profile_interval < 1 or args.profile_interval > 60:
            parser.error("--profile-interval must be between 1 and 60 whole seconds")
        if args.profile_seconds < args.profile_interval * 3 or args.profile_seconds > 3600:
            parser.error("--profile-seconds must be at least three intervals and at most 3600")
        if args.configure_only or args.print_commands or args.record:
            parser.error("native profiling cannot combine with --configure-only, --print-commands or --record")
    elif args.profile_output:
        parser.error("--profile-output requires --profile-seconds")
    if args.launch:
        lldb_quote(args.launch)
    commands = commands_for(args)
    if args.print_commands:
        print("\n".join(commands))
        return
    if sys.platform != "darwin":
        parser.error("this host entry point requires macOS")

    interface = find_interface(args.wait)
    name = interface["name"]
    if args.configure or args.configure_only:
        configure = network_configuration_command(name)
        print("Configuring " + name + " (" + HOST_MAC + ")", flush=True)
        subprocess.run(configure, check=True)
    elif (HOST_IP, "0xfffffffc") not in interface["addresses"] and (HOST_IP, NETMASK) not in interface["addresses"]:
        raise RuntimeError("T733 network service needs its saved address. Run:\n  "
                           + shlex.join(network_configuration_command(name)))
    if args.configure_only:
        return
    if args.profile_seconds is not None:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from t733_native_profile import collect
        collect(args, DEVICE_IP, SERVER_PORT)
        return

    lldb = subprocess.check_output(["/usr/bin/xcrun", "--find", "lldb"], text=True).strip()
    invocation = [lldb]
    for command in commands:
        invocation.extend(["-o", command])
    print(f"T733: {name} {HOST_IP}/30 -> {DEVICE_IP}:{SERVER_PORT}", flush=True)
    if args.attach:
        print("Name attachment includes existing processes and waits if absent; Ctrl-C cancels the wait.", flush=True)
    print("Use 'thread backtrace all', 'process continue', and 'process detach' in LLDB.", flush=True)
    if args.record:
        record = args.record.expanduser().resolve()
        record.parent.mkdir(parents=True, exist_ok=True)
        if record.exists():
            raise RuntimeError("Record file already exists: " + str(record))
        invocation = ["/usr/bin/script", "-q", str(record), *invocation]
    os.execv(invocation[0], invocation)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, plistlib.InvalidFileException,
            subprocess.CalledProcessError) as error:
        print("t733-lldb: " + str(error), file=sys.stderr)
        raise SystemExit(1)
