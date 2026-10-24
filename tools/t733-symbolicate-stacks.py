#!/usr/bin/env python3
"""Symbolicate T733 USB user stacks using recorded load addresses and original files."""

import argparse
import bisect
import json
import mmap
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]


def number(value):
    return int(str(value), 0)


def cstring(data, offset):
    end = data.find(b"\0", offset)
    if end < offset:
        raise ValueError("Unterminated string")
    return data[offset:end].decode("utf-8", errors="replace")


def uleb(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise ValueError("Invalid ULEB128")


def exports(data, base):
    pending = [(0, "")]
    seen = set()
    result = []
    while pending:
        offset, prefix = pending.pop()
        if offset in seen:
            continue
        seen.add(offset)
        length, cursor = uleb(data, offset)
        children = cursor + length
        if length:
            flags, cursor = uleb(data, cursor)
            if not flags & 8:
                address, cursor = uleb(data, cursor)
                if flags & 3 == 0:
                    result.append((base + address, prefix, "export"))
        count = data[children]
        cursor = children + 1
        for _ in range(count):
            edge = cstring(data, cursor)
            cursor = data.index(0, cursor) + 1
            child, cursor = uleb(data, cursor)
            pending.append((child, prefix + edge))
    return result


class Cache:
    """Cache maps original subcache bytes and resolves per-image symbol tables lazily."""

    def __init__(self, directory):
        self.files = []
        self.mappings = []
        self.symbol_tables = {}
        for path in sorted(directory.glob("dyld_shared_cache_arm64*")):
            if path.name.endswith(".symbols"):
                continue
            stream = path.open("rb")
            data = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
            self.files.append((stream, data))
            if data[:16].rstrip(b"\0") != b"dyld_v1   arm64":
                raise ValueError(f"Unexpected cache architecture: {path}")
            offset, count = struct.unpack_from("<II", data, 16)
            for index in range(count):
                va, size, fileoff, _, _ = struct.unpack_from("<QQQII", data, offset + index * 32)
                self.mappings.append((va, size, fileoff, data))
            if path.name == "dyld_shared_cache_arm64":
                self.main = data
        self.region = struct.unpack_from("<Q", self.main, 224)[0]
        offset, count = struct.unpack_from("<QQ", self.main, 136)
        self.images = []
        for index in range(count):
            _, address, size, pathoff = struct.unpack_from("<16sQII", self.main, offset + index * 32)
            self.images.append((address, size, cstring(self.main, pathoff)))
        self.images.sort()
        self.starts = [item[0] for item in self.images]
        self.locals = {}
        path = directory / "dyld_shared_cache_arm64.symbols"
        self.symbols = None
        if path.exists():
            stream = path.open("rb")
            self.symbols = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
            self.files.append((stream, self.symbols))
            self.local_offset = struct.unpack_from("<Q", self.symbols, 72)[0]
            no, nc, so, ss, eo, ec = struct.unpack_from("<6I", self.symbols, self.local_offset)
            self.local_info = no, nc, so, ss
            for index in range(ec):
                address, first, count = struct.unpack_from("<QII", self.symbols, self.local_offset + eo + index * 16)
                self.locals[address + self.region] = first, count

    def read(self, address, size):
        for start, length, offset, data in self.mappings:
            if start <= address and address + size <= start + length:
                position = offset + address - start
                return data[position:position + size]
        raise ValueError(f"No cache mapping for {address:#x}+{size:#x}")

    def symbols_for(self, image):
        base = image[0]
        if base in self.symbol_tables:
            return self.symbol_tables[base]
        symbols = []
        if base in self.locals:
            first, count = self.locals[base]
            no, nc, so, ss = self.local_info
            if first + count > nc:
                raise ValueError("Local symbol range exceeds table")
            for index in range(first, first + count):
                string, kind, section, _, address = struct.unpack_from("<IBBHQ", self.symbols, self.local_offset + no + index * 16)
                if not kind & 0xe0 and kind & 0xe == 0xe and section and string < ss:
                    symbols.append((address, cstring(self.symbols, self.local_offset + so + string), "local"))
        symbols.extend(macho_symbols(self.read(base, 32), lambda off, size: self.read(base + off, size), base, self.read))
        symbols.sort()
        self.symbol_tables[base] = symbols
        return symbols

    def resolve(self, address):
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0:
            return None
        image = self.images[index]
        if address >= image[0] + image[1]:
            return None
        return describe(address, image[0], image[2], self.symbols_for(image))


def macho_symbols(header, read_file, base, read_va=None):
    if struct.unpack_from("<I", header)[0] != 0xfeedfacf:
        raise ValueError("Expected little-endian Mach-O 64")
    ncmds, size = struct.unpack_from("<II", header, 16)
    commands = read_file(32, size)
    segments = []
    symtab = None
    trie = None
    cursor = 0
    for _ in range(ncmds):
        cmd, length = struct.unpack_from("<II", commands, cursor)
        if length < 8 or cursor + length > len(commands):
            raise ValueError("Invalid Mach-O command")
        if cmd == 0x19:
            va, vsize, fileoff, filesize = struct.unpack_from("<4Q", commands, cursor + 24)
            segments.append((va, vsize, fileoff, filesize))
        elif cmd == 2:
            symtab = struct.unpack_from("<4I", commands, cursor + 8)
        elif cmd == 0x80000033:
            trie = struct.unpack_from("<II", commands, cursor + 8)
        elif cmd in (0x22, 0x80000022):
            trie = struct.unpack_from("<II", commands, cursor + 40)
        cursor += length

    def linkedit(offset, length):
        if read_va is None:
            return read_file(offset, length)
        for va, _, fileoff, filesize in segments:
            if fileoff <= offset and offset + length <= fileoff + filesize:
                return read_va(va + offset - fileoff, length)
        raise ValueError("LINKEDIT offset is outside image segments")

    result = []
    if symtab:
        offset, count, strings, string_size = symtab
        table = linkedit(offset, count * 16)
        names = linkedit(strings, string_size)
        for index in range(count):
            string, kind, section, _, address = struct.unpack_from("<IBBHQ", table, index * 16)
            if not kind & 0xe0 and kind & 0xe == 0xe and section and string < len(names):
                result.append((address, cstring(names, string), "nlist"))
    if trie and trie[1]:
        result.extend(exports(linkedit(*trie), base))
    return result


def describe(address, base, image, symbols):
    result = {"image": image, "image_offset": hex(address - base)}
    index = bisect.bisect_right(symbols, (address, chr(0x10ffff), "")) - 1
    if index >= 0 and symbols[index][0] >= base:
        value, name, source = symbols[index]
        result.update(symbol=name, symbol_offset=hex(address - value), symbol_source=source)
    return result


def objc_methods(data, base):
    """Original executable metadata supplies Objective-C instance and class methods."""
    segments = []
    sections = []
    fixups = None
    cursor = 32
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        command, size = struct.unpack_from("<II", data, cursor)
        if command == 0x19:
            va, length, offset, filelength = struct.unpack_from("<4Q", data, cursor + 24)
            segments.append((va, length, offset, filelength))
            count = struct.unpack_from("<I", data, cursor + 64)[0]
            for index in range(count):
                position = cursor + 72 + index * 80
                name = data[position:position + 16].split(b"\0", 1)[0]
                address, length = struct.unpack_from("<QQ", data, position + 32)
                sections.append((name, address, length))
        elif command == 0x80000034:
            fixups = struct.unpack_from("<II", data, cursor + 8)
        cursor += size

    def read(address, size):
        for va, _, offset, length in segments:
            if va <= address and address + size <= va + length:
                position = offset + address - va
                return data[position:position + size]
        raise ValueError(f"No executable bytes for Objective-C address {address:#x}")

    def integer(address, format="<Q"):
        return struct.unpack(format, read(address, struct.calcsize(format)))[0]

    def string(address):
        for va, _, offset, length in segments:
            if va <= address < va + length:
                position = offset + address - va
                end = data.find(b"\0", position, offset + length)
                if end >= position:
                    return data[position:end].decode("utf-8", errors="replace")
        raise ValueError(f"No executable string for Objective-C address {address:#x}")

    rebases = {}
    if fixups:
        start, length = fixups
        payload = data[start:start + length]
        starts = struct.unpack_from("<I", payload, 4)[0]
        count = struct.unpack_from("<I", payload, starts)[0]
        for index in range(count):
            relative = struct.unpack_from("<I", payload, starts + 4 + index * 4)[0]
            if not relative:
                continue
            position = starts + relative
            page_size, pointer_format = struct.unpack_from("<HH", payload, position + 4)
            segment_offset = struct.unpack_from("<Q", payload, position + 8)[0]
            pages = struct.unpack_from("<H", payload, position + 20)[0]
            if pointer_format not in (2, 6):
                continue
            for page in range(pages):
                first = struct.unpack_from("<H", payload, position + 22 + page * 2)[0]
                if first == 0xffff:
                    continue
                chain_starts = []
                if first & 0x8000:
                    overflow = first & 0x7fff
                    while True:
                        entry = struct.unpack_from("<H", payload, position + 22 + overflow * 2)[0]
                        chain_starts.append(entry & 0x7fff)
                        overflow += 1
                        if entry & 0x8000:
                            break
                else:
                    chain_starts.append(first)
                for first in chain_starts:
                    address = base + segment_offset + page * page_size + first
                    while True:
                        raw = integer(address)
                        following = (raw >> 51) & 0xfff
                        if raw >> 63:
                            rebases[address] = 0
                        else:
                            target = raw & ((1 << 36) - 1)
                            if pointer_format == 6:
                                target += base
                            target |= ((raw >> 36) & 0xff) << 56
                            rebases[address] = target
                        if not following:
                            break
                        address += following * 4

    def pointer(address):
        if address in rebases:
            return rebases[address]
        return integer(address)

    result = []
    visited = set()

    def methods(address, class_name, prefix):
        if not address:
            return
        flags, count = struct.unpack("<II", read(address, 8))
        stride = flags & 0xffff & ~3
        relative = bool(flags & 0x80000000)
        if stride < (12 if relative else 24):
            raise ValueError("Invalid Objective-C method entry size")
        for index in range(count):
            entry = address + 8 + index * stride
            if relative:
                selector = entry + integer(entry, "<i")
                if not flags & 0x40000000:
                    selector = pointer(selector)
                implementation = entry + 8 + integer(entry + 8, "<i")
            else:
                selector = pointer(entry)
                implementation = pointer(entry + 16)
            if selector and implementation:
                name = f"{prefix}[{class_name} {string(selector)}]"
                result.append((implementation, name, "objc-method"))

    for name, address, length in sections:
        if name != b"__objc_classlist":
            continue
        for entry in range(address, address + length, 8):
            klass = pointer(entry)
            if not klass or klass in visited:
                continue
            visited.add(klass)
            try:
                metadata = pointer(klass + 32) & ~7
                class_name = string(pointer(metadata + 24))
                methods(pointer(metadata + 32), class_name, "-")
                metaclass = pointer(klass)
                if metaclass:
                    metadata = pointer(metaclass + 32) & ~7
                    methods(pointer(metadata + 32), class_name, "+")
            except (ValueError, IndexError, struct.error):
                # External classes and unsupported pointer formats retain image offsets.
                continue
    return result


class Executables:
    """Executable files supply preferred addresses and symbols for recorded image loads."""

    def __init__(self, roots):
        self.roots = roots
        self.loaded = {}

    def resolve(self, path, load, address):
        if address < load:
            return None
        if path not in self.loaded:
            source = next((root / path.lstrip("/") for root in self.roots if (root / path.lstrip("/")).is_file()), None)
            if source is None:
                return None
            data = source.read_bytes()
            if data[:4] == b"\xca\xfe\xba\xbe":
                count = struct.unpack_from(">I", data, 4)[0]
                for index in range(count):
                    cpu, _, offset, length, _ = struct.unpack_from(">5I", data, 8 + index * 20)
                    if cpu == 0x100000c:
                        data = data[offset:offset + length]
                        break
            cursor = 32
            base = None
            end = 0
            for _ in range(struct.unpack_from("<I", data, 16)[0]):
                cmd, size = struct.unpack_from("<II", data, cursor)
                if cmd == 0x19:
                    va, length, off, filelength = struct.unpack_from("<4Q", data, cursor + 24)
                    if off == 0 and filelength:
                        base = va
                    end = max(end, va + length)
                cursor += size
            if base is None:
                raise ValueError(f"Missing file-backed header segment: {source}")
            symbols = macho_symbols(data[:32], lambda off, size: data[off:off + size], base)
            try:
                symbols.extend(objc_methods(data, base))
            except (ValueError, IndexError, struct.error):
                # Existing nlist and export symbols remain usable without ObjC metadata.
                pass
            symbols.sort()
            self.loaded[path] = base, end, symbols
        base, end, symbols = self.loaded[path]
        unslid = address - load + base
        if not base <= unslid < end:
            return None
        return describe(unslid, base, path, symbols)


def parse_log(path):
    records = []
    for line_number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if "STACK" not in line.upper():
            continue
        fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
        fields = {key.lower(): value.strip('"').rstrip(",") for key, value in fields.items()}
        fields.update(line=line_number, raw=line)
        records.append(fields)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--cache", type=Path, default=ROOT / ".cache/volumes/systemos/System/Library/Caches/com.apple.dyld")
    parser.add_argument("--root", type=Path, action="append", help="Original mounted OS root; repeat for SystemOS")
    parser.add_argument("--output", type=Path, required=True, help="Text output path; JSON uses the same stem")
    parser.add_argument("--metadata", type=Path, help='Optional JSON: {"PID": {"cache_slide": "0x...", "images": [{"path": "/...", "load": "0x..."}]}}')
    args = parser.parse_args()
    cache = Cache(args.cache)
    executables = Executables(args.root or [ROOT / ".cache/volumes/os", ROOT / ".cache/volumes/systemos"])
    supplied = json.loads(args.metadata.read_text()) if args.metadata else {}
    processes = {}
    output = []
    records = parse_log(args.log)
    for record in records:
        pid = record.get("pid")
        if pid is None:
            continue
        sample = record.get("sample", "unspecified")
        key = f"{sample}:{pid}"
        if key not in processes:
            process = dict(supplied.get(pid, {}))
            process["images"] = list(process.get("images", []))
            process.update(pid=pid, sample=sample, name=record.get("name"))
            processes[key] = process
        process = processes[key]
        if record.get("valid", "1") == "1" and ("cache_slide" in record or "slide" in record and "CACHE" in record["raw"]):
            process["cache_slide"] = record.get("cache_slide", record.get("slide"))
            process["cache_base"] = record.get("base")
            process["main_load"] = record.get("main_load")
        if "path" in record and "load" in record:
            process.setdefault("images", []).append({"path": record["path"], "load": record["load"]})
    for record in records:
        pid = record.get("pid")
        if pid is None:
            continue
        sample = record.get("sample", "unspecified")
        process = processes[f"{sample}:{pid}"]
        for field in ("pc", "lr", "address"):
            if field not in record:
                continue
            kind = record.get("kind", field)
            address = number(record[field])
            if not address:
                continue
            row = {"line": record["line"], "sample": sample, "pid": pid, "tid": record.get("tid"), "name": record.get("name"), "frame": record.get("frame"), "kind": kind, "address": hex(address)}
            lookup = address - 4 if kind in ("lr", "fp") else address
            row["lookup_address"] = hex(lookup)
            resolved = None
            try:
                if "cache_slide" in process:
                    slide = number(process["cache_slide"])
                    resolved = cache.resolve(lookup - slide)
                    if resolved:
                        row["cache_slide"] = hex(slide)
                if resolved is None:
                    for image in reversed(process.get("images", [])):
                        resolved = executables.resolve(image["path"], number(image["load"]), lookup)
                        if resolved:
                            break
            except (ValueError, IndexError, struct.error, OSError) as error:
                row["error"] = str(error)
            row.update(resolved or {"unresolved": "no matching image with recorded load address"})
            output.append(row)
    lines = []
    for row in output:
        detail = row.get("symbol", row.get("image", row.get("unresolved", "?")))
        offset = row.get("symbol_offset", row.get("image_offset"))
        if offset:
            detail += "+" + offset
        lines.append(f"L{row['line']} sample={row['sample']} pid={row['pid']} tid={row['tid']} frame={row['frame']} {row['kind']}={row['address']} {detail}" + (f" [{row['image']}]" if "symbol" in row else ""))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n")
    endings = [record for record in records if "STACK_END:" in record["raw"]]
    args.output.with_suffix(".json").write_text(json.dumps({"log": str(args.log), "frames": output, "processes": processes, "stack_ends": endings}, indent=2) + "\n")
    print(f"Wrote {len(output)} addresses to {args.output} and {args.output.with_suffix('.json')}")


if __name__ == "__main__":
    main()
