#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luigi C.
"""Optional development tool; normal C builds use the checked-in image header."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def checksum(data):
    """Reproduce the measured PDA word-wise LFSR, not a standard CRC variant."""
    state = 0
    for word, in struct.iter_unpack('<I', data):
        parity = (state & 0x10921111).bit_count() & 1
        state = (state >> 1) ^ (parity << 31) ^ word
    return state


def main():
    """Assemble original NDS32 instructions and generate the ROM download trailer."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembler', default='nds32le-elf-as')
    parser.add_argument('--linker', default='nds32le-elf-ld')
    parser.add_argument('--objcopy', default='nds32le-elf-objcopy')
    parser.add_argument('--check', action='store_true', help='Compare without replacing the header')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='birriasdr-build-') as directory:
        temp = Path(directory)
        layout = temp / 'layout.ld'
        layout.write_text('ENTRY(_start)\nSECTIONS { .text 0x00915000 : { *(.text.entry) *(.text*) } }\n')
        subprocess.run([args.assembler, '-EL', '-march=v3', '-mreduced-regs',
                        '-mno-all-relax', '-o', str(temp / 'iq.o'),
                        str(ROOT / 'firmware/iq_stream.S')], check=True)
        subprocess.run([args.linker, '--no-relax', '-T', str(layout), '-o',
                        str(temp / 'iq.elf'), str(temp / 'iq.o')], check=True)
        subprocess.run([args.objcopy, '-O', 'binary', '-j', '.text',
                        str(temp / 'iq.elf'), str(temp / 'iq.code')], check=True)
        code = (temp / 'iq.code').read_bytes()
    if not code or len(code) > 4096 or len(code) % 2:
        raise ValueError('Unexpected instruction size')
    image = code + bytes(-len(code) % 16) + bytes(16)
    image = image[:-4] + struct.pack('<I', checksum(image))
    assert checksum(image) == 0
    digest = hashlib.sha256(image).hexdigest()
    header = ('/* SPDX-License-Identifier: MIT\n * Copyright (c) 2026 Luigi C.\n'
              ' * Generated from firmware/iq_stream.S by tools/rebuild_firmware.py.\n'
              f' * SHA256: {digest}\n */\n#ifndef MT7921_IMAGE_H\n#define MT7921_IMAGE_H\n'
              '#include <stdint.h>\nstatic const uint8_t mt7921_image[] = {\n')
    header += '\n'.join('    ' + ', '.join(f'0x{x:02x}' for x in image[i:i + 16]) + ','
                        for i in range(0, len(image), 16))
    header += '\n};\n#endif\n'
    target = ROOT / 'src/chips/mt7921_image.h'
    if args.check:
        if target.read_text() != header:
            raise ValueError('Firmware source and embedded header differ')
    else:
        target.write_text(header)
    print(f'{len(code)} instruction bytes, {len(image)} download bytes, SHA256 {digest}')


if __name__ == '__main__':
    main()
