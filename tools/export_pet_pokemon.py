"""Export Petland's generated sprite source into a file pack for the system pet.

No new downloads: consumes prepare_pokemon.py output, preserves CREDITS.json.
Generated artwork stays outside Git. Python standard library only.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import struct
import zlib


def export(source: Path, destination: Path):
    text = (source / 'pokemon_assets.c').read_text(encoding='utf-8')
    arrays = {}
    for kind, dex, shiny, field, values in re.findall(
            r'static const (uint16_t|uint8_t) p(\d+)_(\d+)_(pal|px)\[\] = \{([^}]+)\};', text):
        key = (int(dex), int(shiny))
        if not 1 <= key[0] <= 151 or key[1] not in (0, 1):
            raise ValueError('Unsupported sprite identity')
        expected = 'uint16_t' if field == 'pal' else 'uint8_t'
        if kind != expected or field in arrays.setdefault(key, {}):
            raise ValueError('Invalid or duplicate sprite array')
        arrays[key][field] = [int(v) for v in values.split(',')]
    if not arrays or len(arrays) > 302:
        raise ValueError('Missing or oversized sprite catalog')
    credits = source / 'CREDITS.json'
    json.loads(credits.read_text(encoding='utf-8'))
    index = bytearray()
    data = bytearray()
    offset = 20 + len(arrays) * 12
    for (dex, shiny), sprite in sorted(arrays.items()):
        pal, px = sprite['pal'], sprite['px']
        frames, remainder = divmod(len(px), 1600)
        if remainder or not 1 <= frames <= 4 or not 1 <= len(pal) <= 255:
            raise ValueError('Invalid palette or frame dimensions')
        if any(not 0 <= c <= 65535 for c in pal) or any(
                not (0 <= p < len(pal) or p == 255) for p in px):
            raise ValueError('Invalid pixel or RGB565 color')
        index.extend(struct.pack('<HBBIHH', dex, shiny, frames, offset + len(data), len(pal), 0))
        data.extend(struct.pack('<' + 'H' * len(pal), *pal))
        data.extend(bytes(px))
    payload = index + data
    header = struct.pack('<4sHHIII', b'PKP1', 1, len(arrays), 20, offset, zlib.crc32(payload))
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / 'pokemon.pkp'
    temporary = target.with_suffix('.tmp')
    temporary.write_bytes(header + payload)
    temporary.replace(target)
    shutil.copyfile(credits, destination / 'CREDITS.json')
    return target, len(arrays)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    target, count = export(args.source, args.output)
    print(f'PET_POKEMON_PACK_READY {count} variants; {target.stat().st_size} bytes; {target}')
