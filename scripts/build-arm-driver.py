#!/usr/bin/env python3
"""Build ARM kernel driver archives with the XNU IOKit compiler arguments."""
import argparse
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIRECTORIES = {
    'ArmVirtPlatform': 'armvirt-platform',
    'ArmDTPlatform': 'armdt-platform',
    'ArmCpu': 'armcpu',
    'NativeUfs': 'native-ufs',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=OUTPUT_DIRECTORIES)
    parser.add_argument('--tree', type=Path, default=ROOT)
    parser.add_argument('--verified-block', action='store_true',
                        help='Compile NativeUfs root publication with the owner-policy provider')
    parser.add_argument('--owner-root', action='store_true',
                        help='Provide mounted-volume authorization through verified UFS windows')
    parser.add_argument('--storage-kernel', type=Path)
    parser.add_argument('--storage-layout', type=Path)
    args = parser.parse_args()
    args.verified_block = args.verified_block or args.owner_root
    if args.verified_block and args.component != 'NativeUfs':
        parser.error('--verified-block requires NativeUfs')
    if bool(args.storage_kernel) != bool(args.storage_layout):
        parser.error('--storage-kernel and --storage-layout are required together')
    if args.storage_kernel and not args.verified_block:
        parser.error('Storage bindings require --verified-block or --owner-root')
    tree = args.tree.resolve()

    recipe = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    source = tree / 'drivers' / args.component / (args.component + '.cpp')
    output = tree / 'build' / OUTPUT_DIRECTORIES[args.component]
    output.mkdir(parents=True, exist_ok=True)
    destination = output / (args.component + '.o')

    arguments = record['arguments']
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(destination)])
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1

    command.extend([
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
        '-I', str(tree / '.cache/xnu-work/osfmk'),
    ])
    if args.component == 'NativeUfs':
        command.extend(['-I', str(tree / 'build/storage-abi/include')])
        if args.verified_block:
            command.extend([
                '-DNATIVE_VERIFIED_BLOCK=1',
                '-I', str(tree / 'kits/VerifiedRoot/include'),
                '-I', str(tree / 'drivers/NativeVirtioBlock'),
            ])
        if args.owner_root:
            command.append('-DNATIVE_UFS_OWNER_ROOT_API=1')
    (output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    (output / 'compile.log').write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit((result.stdout + result.stderr)[-6000:])

    archive = output / ('lib' + args.component + '.a')
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, destination], check=True)
    if args.storage_kernel:
        subprocess.run([
            'python3', ROOT / 'scripts/build-native-storage-bindings.py',
            '--kernel', args.storage_kernel.resolve(), '--layout', args.storage_layout.resolve(),
            '--object', destination, '--output', output,
        ], check=True)
    print(f'{args.component}: {archive}')


if __name__ == '__main__':
    main()
