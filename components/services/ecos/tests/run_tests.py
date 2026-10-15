"""Run production wire code on the host using the installed ESP RISC-V GCC.

Requires unicorn==2.1.4 and pyelftools; use --deps for an isolated install.
"""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--gcc', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--deps', type=Path)
args = parser.parse_args()
if args.deps:
    sys.path.insert(0, str(args.deps.resolve()))
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE
from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_A0
from elftools.elf.elffile import ELFFile

work = Path(__file__).resolve().parent
args.output.mkdir(parents=True, exist_ok=True)
target = args.output / 'ecos_tests.elf'
subprocess.run([str(args.gcc), '-march=rv32im', '-mabi=ilp32', '-std=c11', '-O1',
                '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
                '-nostdlib', '-nostartfiles', '-msmall-data-limit=0', '-Wl,--no-relax',
                '-Wl,-Ttext=0x10000', '-Wl,-e,_start', '-I', str(work / 'freestanding'),
                '-I', str(work.parent), str(work / 'test_wire.c'), '-o', str(target)], check=True)
machine = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
machine.mem_map(0, 0x200000)
with target.open('rb') as stream:
    elf = ELFFile(stream)
    entry = elf.header['e_entry']
    for segment in elf.iter_segments():
        if segment.header['p_type'] == 'PT_LOAD':
            machine.mem_write(segment.header['p_vaddr'], segment.data())
machine.reg_write(UC_RISCV_REG_SP, 0x10f000)
finished = False

def on_code(uc, address, size, data):
    global finished
    if bytes(uc.mem_read(address, size)) == b'\x73\x00\x10\x00':
        finished = True
        uc.emu_stop()

machine.hook_add(UC_HOOK_CODE, on_code, begin=entry, end=entry + 16)
machine.emu_start(entry, 0x110000, timeout=20_000_000, count=50_000_000)
if not finished:
    raise SystemExit('FAIL: emulator time/instruction limit')
result = machine.reg_read(UC_RISCV_REG_A0)
if result:
    raise SystemExit(f'FAIL: test_wire.c:{result}')
print('PASS: pairing and family C codecs, frame bounds, transcript binding, status encoding, epochs and anti-replay')
