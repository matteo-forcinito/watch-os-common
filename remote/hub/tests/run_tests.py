"""Run the real freestanding C state engine in an isolated RISC-V emulator."""
from pathlib import Path
import argparse
import subprocess
import sys
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parent.parent)
parser.add_argument('--gcc', type=Path)
parser.add_argument('--output', type=Path, default=Path.cwd())
parser.add_argument('--deps', type=Path)
args = parser.parse_args()
work = Path(__file__).resolve().parent
if args.deps:
    sys.path.insert(0, str(args.deps.resolve()))
from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32, UC_HOOK_CODE
from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_A0
from elftools.elf.elffile import ELFFile

gcc = args.gcc or shutil.which('riscv32-esp-elf-gcc')
if not gcc:
    raise SystemExit('Supply --gcc or activate the ESP-IDF environment')
args.output.mkdir(parents=True, exist_ok=True)
elf_path = args.output / 'hub_tests.elf'
subprocess.run([str(gcc), '-march=rv32im', '-mabi=ilp32', '-std=c11', '-O1',
                '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
                '-DHUB_EMULATOR_ENTRY', '-nostdlib', '-nostartfiles',
                '-msmall-data-limit=0', '-Wl,--no-relax',
                '-Wl,-Ttext=0x10000', '-Wl,-e,_start',
                '-I', str(args.source), str(args.source / 'hub_state.c'),
                str(work / 'test_hub_state.c'), '-o', str(elf_path)], check=True)
machine = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
machine.mem_map(0x0, 0x200000)
with elf_path.open('rb') as stream:
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
    raise SystemExit('FAIL: test did not finish within emulator instruction/time budget')
result = machine.reg_read(UC_RISCV_REG_A0)
if result:
    raise SystemExit(f'FAIL: test_hub_state.c:{result}')
print('PASS: actual C engine, RISC-V emulator; no radio, USB or PC input used')
