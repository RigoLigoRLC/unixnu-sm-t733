#!/usr/bin/env python3
"""Extract firmware and board configuration from the T733 vendor and odm images."""

import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import stat

from dissect.extfs import ExtFS
from dissect.fat import FATFS


def extract_image(image, output, partition):
    records = []
    prefixes = ('firmware/', 'etc/', 'lib/modules/', 'lib64/modules/', 'gpu/', 'bin/init.')

    with image.open('rb') as stream:
        filesystem = ExtFS(stream)
        pending = [(PurePosixPath(), filesystem.get('/'))]

        while pending:
            parent, directory = pending.pop()
            for inode in directory.iterdir():
                if inode.filename in ('.', '..'):
                    continue

                path = parent / inode.filename
                relative = path.as_posix()
                selected = partition in ('odm', 'dsp') or relative in ('build.prop', 'default.prop', 'ueventd.rc') or relative.startswith(prefixes)
                record = {
                    'partition': partition,
                    'image': str(image.resolve()),
                    'path': '/' + relative,
                    'inode': inode.inum,
                    'mode': oct(inode.inode.i_mode),
                    'bytes': inode.size,
                    'extracted': False,
                }

                if inode.filetype == stat.S_IFDIR:
                    pending.append((path, inode))
                    record['type'] = 'directory'
                elif inode.filetype == stat.S_IFLNK:
                    record['type'] = 'symlink'
                    record['target'] = inode.link
                elif inode.filetype == stat.S_IFREG:
                    record['type'] = 'file'
                    if selected:
                        destination = output / partition / relative
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        with inode.open() as source, destination.open('wb') as target:
                            shutil.copyfileobj(source, target, 1024 * 1024)
                        record['extracted'] = True
                        record['output'] = str(destination.resolve())
                else:
                    record['type'] = 'special'

                records.append(record)

    return records


def extract_fat_image(image, output, partition):
    records = []

    with image.open('rb') as stream:
        filesystem = FATFS(stream)
        pending = [(PurePosixPath(), filesystem.get('/'))]

        while pending:
            parent, directory = pending.pop()
            for entry in directory.iterdir():
                if entry.name in ('.', '..') or entry.is_volume_id():
                    continue

                path = parent / entry.name
                record = {
                    'partition': partition,
                    'image': str(image.resolve()),
                    'path': '/' + path.as_posix(),
                    'bytes': entry.size,
                    'extracted': False,
                }

                if entry.is_directory():
                    record['type'] = 'directory'
                    pending.append((path, entry))
                else:
                    destination = output / partition / path.as_posix()
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with entry.open() as source, destination.open('wb') as target:
                        shutil.copyfileobj(source, target, 1024 * 1024)
                    record['type'] = 'file'
                    record['extracted'] = True
                    record['output'] = str(destination.resolve())

                records.append(record)

    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--partitions', type=Path)
    parser.add_argument('--apnhlos', type=Path,
                        help='Standalone APNHLOS image for the GPU ZAP input')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []

    for partition in ('vendor', 'odm'):
        records.extend(extract_image(args.images / (partition + '.img'), args.output, partition))

    if args.partitions:
        for partition in ('apnhlos', 'modem'):
            records.extend(extract_fat_image(args.partitions / (partition + '.img'), args.output, partition))
        records.extend(extract_image(args.partitions / 'dsp.img', args.output, 'dsp'))
    elif args.apnhlos:
        records.extend(extract_fat_image(args.apnhlos, args.output, 'apnhlos'))

    records.sort(key=lambda item: (item['partition'], item['path']))
    (args.output / 'inventory.json').write_text(json.dumps(records, indent=2) + '\n')
    with (args.output / 'inventory.tsv').open('w') as table:
        table.write('partition\tpath\ttype\tbytes\textracted\ttarget\n')
        for item in records:
            table.write(f"{item['partition']}\t{item['path']}\t{item['type']}\t{item['bytes']}\t"
                        f"{int(item['extracted'])}\t{item.get('target', '')}\n")

    files = [item for item in records if item['extracted']]
    print(f'Extracted {len(files)} files, {sum(item["bytes"] for item in files)} bytes')
    print(f'Inventory: {args.output / "inventory.json"}')


if __name__ == '__main__':
    main()
