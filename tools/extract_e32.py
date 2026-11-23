#!/usr/bin/env python3
"""Extract the local E32 build inputs and, optionally, the files the SIS installs on its drive."""

import argparse
from pathlib import Path
import struct
import zipfile
import zlib


def fields(data, array_type=None):
    pos = 0
    while pos < len(data):
        if array_type is None:
            kind, size = struct.unpack_from('<II', data, pos)
            pos += 8
        else:
            kind, size = array_type, struct.unpack_from('<I', data, pos)[0]
            pos += 4
        end = pos + size
        if end > len(data) or size >= 1 << 31:
            raise ValueError('invalid or unsupported SIS field size')
        yield kind, data[pos:end]
        pos = (end + 3) & ~3


def decompress(body):
    codec, expected = struct.unpack_from('<IQ', body)
    if codec not in (0, 1):
        raise ValueError(f'unsupported SIS compression {codec}')
    raw = zlib.decompress(body[12:]) if codec else body[12:]
    if len(raw) != expected:
        raise ValueError('SIS payload size mismatch')
    return raw


def payloads(data, array_type=None):
    for kind, body in fields(data, array_type):
        if kind in (12, 30, 31, 32):  # Contents, Data, DataUnit, FileData
            yield from payloads(body)
        elif kind == 2:
            yield from payloads(body[4:], struct.unpack_from('<I', body)[0])
        elif kind == 3:
            yield decompress(body)


MODULES = {
    0x2000BBE5: 'IsLand_3rd.exe',
    0x2000BBE6: 'SimMediaEngineDll.dll',
    0x2000BBE7: 'SimDeviceEngineDll.dll',
}


APP_RESOURCES = {
    r'!:\resource\apps\island_3rd.rsc': 'resource/apps/IsLand_3rd.rsc',
    r'!:\resource\apps\island_3rd_reg.mif': 'resource/apps/IsLand_3rd_reg.mif',
    r'!:\private\10003a3f\import\apps\island_3rd_reg.rsc':
        'private/10003a3f/import/apps/IsLand_3rd_reg.rsc',
}


# Game data the SIS installs on the installation drive (the save slots and the save list `ls`).
GAME_DATA_PREFIX = '!:\\system\\apps\\island\\'


def resource_payloads(controller, files):
    install_block, = (body for kind, body in fields(controller) if kind == 28)
    array, = (body for kind, body in fields(install_block)
              if kind == 2 and struct.unpack_from('<I', body)[0] == 24)
    result = {}
    for _, description in fields(array[4:], 24):
        kind, size = struct.unpack_from('<II', description)
        if kind != 1 or size % 2 or 8 + size > len(description) - 28:
            raise ValueError('invalid SIS file description path')
        path = description[8:8 + size].decode('utf-16le').lower()
        if path in APP_RESOURCES:
            target = APP_RESOURCES[path]
        elif path.startswith(GAME_DATA_PREFIX) and '*' not in path and not path.endswith('\\'):
            target = path[3:].replace('\\', '/')
            if any(part in ('', '.', '..') for part in target.split('/')):
                raise ValueError(f'invalid SIS install path {path}')
        else:
            continue
        length, index = struct.unpack_from('<QI', description, len(description) - 12)
        if target in result or index >= len(files) or len(files[index]) != length:
            raise ValueError(f'invalid SIS file payload for {path}')
        result[target] = files[index]
    if not set(APP_RESOURCES.values()) <= result.keys():
        raise ValueError('missing Lament Island application resources')
    return result


def extract(source, out, resource_root=None):
    with zipfile.ZipFile(source) as archive:
        sis, = (name for name in archive.namelist() if name.lower().endswith('.sis'))
        data = archive.read(sis)
    if struct.unpack_from('<I', data)[0] != 0x10201A7A:
        raise ValueError('expected Symbian OS 9 SIS')
    contents, = (body for kind, body in fields(data[16:]) if kind == 12)
    data_section, = (body for kind, body in fields(contents) if kind == 30)
    files = list(payloads(data_section))
    images = {}
    for raw in files:
        if raw[16:20] != b'EPOC':
            continue
        uid = struct.unpack_from('<I', raw, 8)[0]
        if uid not in MODULES or uid in images:
            raise ValueError(f'unexpected E32 module: UID {uid:#010x}')
        if struct.unpack_from('<I', raw, 0x1c)[0] != 0:
            raise ValueError('expected an uncompressed E32 image')
        images[uid] = raw
    if images.keys() != MODULES.keys():
        raise ValueError('missing Lament Island E32 modules')
    resources = {}
    if resource_root is not None:
        packed_controller, = (body for kind, body in fields(contents) if kind == 3)
        raw_controller = decompress(packed_controller)
        controller, = (body for kind, body in fields(raw_controller) if kind == 13)
        resources = resource_payloads(controller, files)
    out.mkdir(parents=True, exist_ok=True)
    for uid, raw in images.items():
        path = out / MODULES[uid]
        path.write_bytes(raw)
        entry, base = struct.unpack_from('<II', raw, 0x48)
        print(f'{path}: {len(raw)} bytes, E32 entry {base + entry:#010x}')
    for name, raw in resources.items():
        path = resource_root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
        print(f'{path}: {len(raw)} bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--resource-root', type=Path, help='local root for the application resources')
    args = parser.parse_args()
    extract(args.archive, args.out, args.resource_root)
