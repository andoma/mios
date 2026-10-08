#!/usr/bin/env python3
"""Exercise the repository's lazy FP switch and UsageFault entry in QEMU.

Requires arm-none-eabi-gcc and qemu-system-arm. No board is accessed.
Tests ordinary NOCP ownership transfers and a synthetic ICI exception return.
QEMU restarts transfers; hardware partial-transfer behavior needs board testing.
"""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
CPU = HERE.parents[1] / 'src/cpu/cortexm'


def function(filename, name):
    source = (CPU / filename).read_text()
    start = source.index(name + '(')
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


shim = '''
#include <stddef.h>
#include <stdint.h>
typedef struct { int32_t *t_fpuctx; void *t_sp; } thread_t;
typedef struct { struct { thread_t *current_fpu; } sched; } cpu_t;
static cpu_t cpu0;
#define curcpu() (&cpu0)
// Read asynchronously by the real UsageFault handler.
static thread_t *volatile current;
static thread_t *thread_current(void) { return current; }
static volatile unsigned short *const UFSR = (void *)0xe000ed2a;
static uint32_t fpu_nocp_restores;
uint32_t cpu_fpu_ici_restores;
'''
source = shim
source += 'static inline void\n' + function('cpu.h', 'cpu_fpu_enable')
source += 'void\n' + function('cpu.c', 'cpu_fpu_switch')
source += 'void\n' + function('cpu.c', 'cpu_fpu_resume')
source += 'int\n' + function('exc.c', 'exc_handle_usage_fault')


assembly = (CPU / 'isr.S').read_text()
assembly = assembly[assembly.index('exc_common_fault:'):]
assembly = assembly[:assembly.index('// ----------- Vectors')]
assembly = '''.syntax unified
.thumb
.section .vectors,"a"
.word 0x20010000
.word reset
.word exc_nmi0
.word exc_hard_fault0
.word exc_mm_fault0
.word exc_bus_fault0
.word exc_usage_fault0
.text
.thumb_func
''' + assembly

pendsv = (CPU / 'isr.S').read_text().split('        .text', 1)[0]
continuation_assembly = assembly.replace(
    '.word exc_usage_fault0\n.text',
    '.word exc_usage_fault0\n.rept 4\n.word 0\n.endr\n.word svc_entry\n.text')
continuation_assembly += pendsv + '\n.text\n' + (HERE / 'continuation.S').read_text()

cases = [
    ('ordinary NOCP', 'check.c', assembly, []),
    ('ICI with old policy', 'continuation.c', continuation_assembly,
     ['-DTEST_OLD_POLICY=1']),
    ('ICI with resume fix', 'continuation.c', continuation_assembly,
     ['-DTEST_OLD_POLICY=0']),
]
for name, test, entry, flags in cases:
    print(name, flush=True)
    with tempfile.TemporaryDirectory(prefix='mios-fpu-lazy-') as directory:
        out = Path(directory)
        (out / 'check.c').write_text(source + '#line 1\n' + (HERE / test).read_text())
        (out / 'entry.S').write_text(entry)
        elf = out / 'test.elf'
        subprocess.run([
            'arm-none-eabi-gcc', '-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16',
            '-mfloat-abi=hard', '-mgeneral-regs-only', '-ffreestanding', '-nostdlib',
            '-O1', '-Wl,-T,' + str(HERE / 'link.ld'), str(out / 'entry.S'),
            str(out / 'check.c'), '-o', str(elf), *flags,
        ], check=True, timeout=30)
        subprocess.run([
            'qemu-system-arm', '-M', 'mps2-an500', '-nographic', '-semihosting',
            '-kernel', str(elf),
        ], check=True, timeout=15)
