#!/usr/bin/env python3
"""Build block-driver imports from the selected prelinked IOStorageFamily image."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/NativePrelink'))
import native_prelink as np


def symbols(data, image):
    command = next(at for kind, at, size in image['commands'] if kind == 2)
    offset, count, strings, _ = struct.unpack_from('<4I', data, command + 8)
    result = []
    for index in range(count):
        name, kind, _, _, value = struct.unpack_from('<IBBHQ', data, offset + index * 16)
        end = data.index(b'\0', strings + name)
        result.append((data[strings + name:end].decode(), kind, value))
    return result


def concrete_methods(data, image, entries, class_name):
    mangled = str(len(class_name)) + class_name
    table = next(value for name, kind, value in entries if name == '__ZTV' + mangled)
    end = min(value for name, kind, value in entries
              if not kind & 0xe0 and kind & 0xe == 0xe and value > table)
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if not section['va'] <= table < section['va'] + section['size']:
                continue
            offset, count = struct.unpack_from('<II', data, section['command_offset'] + 56)
            for index in range(count):
                address, word = struct.unpack_from('<II', data, offset + index * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = entries[word & 0xffffff][0]
    methods = []
    for address in range(table + 16, end, 8):
        if address not in relocations:
            break
        methods.append(relocations[address])
    if not methods or '___cxa_pure_virtual' in methods:
        raise ValueError('The compiled block driver must implement every abstract operation')
    return methods, mangled


def adrp(address, instruction):
    immediate = ((instruction >> 5 & 0x7ffff) << 2) | (instruction >> 29 & 3)
    if immediate & 0x100000:
        immediate -= 0x200000
    return (address & ~4095) + immediate * 4096


def branch(address, instruction):
    immediate = instruction & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--layout', type=Path, required=True)
    parser.add_argument('--object', type=Path, required=True)
    parser.add_argument('--class-name', default='NativeUfsBlock')
    parser.add_argument('--archive-name', default='NativeUfs')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    kernel = args.kernel.read_bytes()
    image = np.macho(kernel)
    layout = json.loads(args.layout.read_text())
    extents = [entry for entry in layout['extents']
               if entry['module'] == 'com.apple.iokit.IOStorageFamily']
    text = next(entry for entry in extents if entry['segment'] == '__TEXT_EXEC')
    const = next(entry for entry in extents if entry['segment'] == '__DATA_CONST')
    exports = np.exports(kernel, image)
    by_name = {name: address for address, names in exports.items() for name in names}

    def read(address, length):
        offset = np.backed_offset(image['segments'], address, length, len(kernel))
        return kernel[offset:offset + length]

    def in_text(address):
        return text['va'] <= address < text['va'] + text['size']

    def thunk_names(address):
        first, load, jump = struct.unpack('<3I', read(address, 12))
        if (first & 0x9f00001f != 0x90000010 or
                load & 0xffc003ff != 0xf9400210 or jump != 0xd61f0200):
            return []
        slot = adrp(address, first) + (load >> 10 & 4095) * 8
        target = struct.unpack('<Q', read(slot, 8))[0]
        return exports.get(target, [])

    obj = args.object.read_bytes()
    obj_image = np.macho(obj)
    entries = symbols(obj, obj_image)
    methods, mangled = concrete_methods(obj, obj_image, entries, args.class_name)
    inherited = [(slot, name) for slot, name in enumerate(methods) if name in by_name]
    if not inherited:
        raise ValueError('The driver has no inherited kernel methods to locate its base vtable')
    get_meta = next(slot for slot, name in enumerate(methods) if name.endswith('12getMetaClassEv'))
    owned = [(slot, name) for slot, name in enumerate(methods)
             if 'IOBlockStorageDevice' in name]
    blob = read(const['va'], const['size'])
    candidates = []
    for offset in range(0, len(blob) - (len(methods) + 2) * 8 + 1, 8):
        if blob[offset:offset + 16] != bytes(16):
            continue
        pointers = struct.unpack_from('<' + 'Q' * len(methods), blob, offset + 16)
        if not all(pointers[slot] == by_name[name] for slot, name in inherited):
            continue
        if not all(in_text(pointers[slot]) for slot, name in owned):
            continue
        if not all(in_text(pointers[slot]) and read(pointers[slot], 4) == b'\x20\x00\x20\xd4'
                   for slot in (0, 1)):
            continue
        candidates.append((const['va'] + offset, pointers))
    if len(candidates) != 1:
        raise ValueError(f'Expected one original abstract block vtable, found {len(candidates)}')
    table, pointers = candidates[0]

    constructors = []
    for address in range(text['va'], text['va'] + text['size'] - 31, 4):
        words = struct.unpack('<8I', read(address, 32))
        if (words[:2] != (0xa9bf7bfd, 0x910003fd) or words[2] & 0xfc000000 != 0x94000000 or
                words[3] & 0x9f00001f != 0x90000008 or words[4] & 0xffc003ff != 0x91000108 or
                words[5:] != (0xf9000008, 0xa8c17bfd, 0xd65f03c0)):
            continue
        stored = adrp(address + 12, words[3]) + (words[4] >> 10 & 4095)
        parent_names = thunk_names(branch(address + 8, words[2]))
        if stored == table + 16 and '__ZN9IOServiceC2EPK11OSMetaClass' in parent_names:
            constructors.append(address)
    if len(constructors) != 1:
        raise ValueError('Cannot locate a unique IOBlockStorageDevice base constructor')
    destructor = pointers[0] - 4
    instruction = struct.unpack('<I', read(destructor, 4))[0]
    if (instruction & 0xfc000000 != 0x14000000 or
            '__ZN9IOServiceD2Ev' not in thunk_names(branch(destructor, instruction))):
        raise ValueError('The original base destructor does not tail-call IOService D2')
    getter = pointers[get_meta]
    first, add, ret = struct.unpack('<3I', read(getter, 12))
    if (first & 0x9f00001f != 0x90000000 or add & 0xffc003ff != 0x91000000 or
            ret != 0xd65f03c0):
        raise ValueError('The original metaclass getter has an unsupported instruction sequence')
    meta = adrp(getter, first) + (add >> 10 & 4095)

    targets = {name: pointers[slot] for slot, name in owned}
    targets.update({
        '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': constructors[0],
        '__ZN20IOBlockStorageDeviceD2Ev': destructor,
        '__ZN20IOBlockStorageDevice10gMetaClassE': meta,
        '__ZTV20IOBlockStorageDevice': table,
    })
    imports = [name for name, kind, value in entries if kind == 1 and 'IOBlockStorageDevice' in name]
    if not imports or any(name not in targets for name in imports):
        raise ValueError('Compiled driver has absent or unsupported original storage imports')
    bindings = {name: targets[name] for name in imports}
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    assembly = output / 'original-storage-bindings.S'
    assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n'
                                for name, value in sorted(bindings.items())))
    compiled = output / 'original-storage-bindings.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                    '-Wno-incompatible-sysroot', '-c', str(assembly), '-o', str(compiled)], check=True)
    archive = output / ('lib' + args.archive_name + '.a')
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(args.object.resolve()), str(compiled)], check=True)
    record = {
        'kernel': str(args.kernel.resolve()), 'layout': str(args.layout.resolve()),
        'object': str(args.object.resolve()), 'class': args.class_name,
        'module': 'com.apple.iokit.IOStorageFamily', 'module_extents': extents,
        'prelink_slide': layout['slide'], 'method_slots': len(methods),
        'inherited_kernel_slots': len(inherited), 'original_metaclass': hex(meta),
        'original_vtable': hex(table), 'original_constructor': hex(constructors[0]),
        'original_destructor': hex(destructor),
        'bindings': {name: hex(value) for name, value in bindings.items()},
        'runtime_metadata': None,
    }
    (output / 'bindings.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'{archive}: {len(bindings)} imports from the selected IOStorageFamily image')


if __name__ == '__main__':
    main()
