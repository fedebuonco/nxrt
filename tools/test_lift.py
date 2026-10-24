"""Differential test of the lifter against this Mac's own CPU, one instruction at a time.

Usage: test_lift.py <nso> <base> [--count N] [--cases K] [--seed S] [--show M]

Picks N distinct instruction words from the module's code, lifts each (lifter.py), and runs K cases of each from random
starting states, both as the lifted C and as the instruction itself on the CPU (tools/test_lift_harness.c: Apple
Silicon is AArch64, so the reference is the hardware). x0-x30, sp, nzcv, v0-v31 and a 256 KiB memory buffer must match.

Not tested: instructions whose result depends on where they are (branches, ADR / ADRP, literal loads: the native copy
runs elsewhere), system instructions and supervisor calls, and instructions naming register 18 (reserved on macOS).
"""
import argparse
import os
import random
import struct
import subprocess
import sys
from collections import Counter

from lifter import Lifter, Unsupported, bits, emit_asm_batch
from nso import Nso

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUF = 0x7200000000  # (test_lift_harness.c maps the buffer here)
BUF_MID = BUF + 0x10000
SPECIAL = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF, 0x7FFFFFFF,
           0x80000000, 0x100000000, 0x3F800000, 0x3FF0000000000000, 0xFF, 0xFFFF, 63, 64, 31, 32]


def rand64(r):
    k = r.random()
    if k < 0.3:
        return r.choice(SPECIAL)
    if k < 0.5:
        return r.getrandbits(8)
    if k < 0.6:
        return struct.unpack('<I', struct.pack('<f', r.uniform(-1000, 1000)))[0] | (r.getrandbits(32) << 32)
    return r.getrandbits(64)


def rand_v(r):
    k = r.random()
    if k < 0.5:
        fs = [r.uniform(-100, 100) for _ in range(4)]
        if r.random() < 0.1:
            fs[r.randrange(4)] = r.choice([0.0, -0.0, float('inf'), float('nan'), 1e-40])
        return int.from_bytes(struct.pack('<4f', *fs), 'little')
    if k < 0.7:
        return int.from_bytes(struct.pack('<2d', *[r.uniform(-1e6, 1e6) for _ in range(2)]), 'little')
    return r.getrandbits(128)


def is_mem(w):
    return bits(w, 25, 1) == 0 and bits(w, 27, 1) == 1


FIELDS = (0, 5, 10, 16)  # Rd/Rt, Rn, Ra/Rt2, Rm/Rs


def skip_reason(w):
    """Why an instruction can't be compared one step at a time, or None."""
    if w == 0 or (w & 0xFFFF0000) == 0:
        return 'udf'
    if (w & 0x7C000000) == 0x14000000 or (w & 0xFE000000) == 0x54000000 or (w & 0x7E000000) == 0x34000000 or \
            (w & 0x7E000000) == 0x36000000 or (w & 0xFE000000) == 0xD6000000:
        return 'branch'
    if (w & 0x1F000000) == 0x10000000:
        return 'adr'
    if (w & 0x3B000000) == 0x18000000:
        return 'literal'
    if (w & 0xFFC00000) == 0xD5000000 or (w & 0xFF000000) == 0xD4000000:
        return 'system'
    if any(bits(w, f, 5) == 18 for f in FIELDS):
        return 'x18'
    return None


def free_reg(w, r):
    """A register 0..28 the instruction doesn't name (nor the next one: pairs), for the harness's state pointer."""
    used = set()
    for f in FIELDS:
        v = bits(w, f, 5)
        used |= {v, v + 1}
    free = [i for i in range(29) if i not in used and i != 18]
    return r.choice(free)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('--count', type=int, default=3000)
    ap.add_argument('--cases', type=int, default=3)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=40)
    args = ap.parse_args()
    r = random.Random(args.seed)
    n = Nso(args.nso)
    lo, hi = n.text[0], n.text[0] + n.text[1]
    words = struct.unpack(f'<{(hi - lo) // 4}I', bytes(n.image[lo:hi]))

    # distinct instruction words, spread over the module
    lifter = Lifter(args.base)
    seen, picks, skipped, unsupported = set(), [], Counter(), Counter()
    order = list(range(len(words)))
    r.shuffle(order)
    for i in order:
        w = words[i]
        if w in seen:
            continue
        seen.add(w)
        why = skip_reason(w)
        if why:
            skipped[why] += 1
            continue
        pc = args.base + lo + i * 4
        try:
            ins = lifter.lift(w, pc)
        except Unsupported as e:
            unsupported[str(e).split(' ')[0]] += 1
            continue
        if ins.kind == 'op':
            body = ins.code
        elif ins.kind == 'asm':
            body = emit_asm_batch([ins.asm])
        else:
            skipped['branch'] += 1
            continue
        picks.append((w, pc, body))
        if len(picks) >= args.count:
            break

    work = os.path.join(ROOT, 'build', 'test_lift')
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'tests.c'), 'w') as f:
        f.write('#include "cpu.h"\n')
        for k, (w, pc, body) in enumerate(picks):
            f.write(f'static void t{k}(Cpu *CPU_RESTRICT c) {{ {body} }} /* {w:08x} @ {pc:x} */\n')
        f.write('GuestFn tests[] = {' + ','.join(f't{k}' for k in range(len(picks))) + '};\n')
        f.write('const uint32_t test_words[] = {' + ','.join(f'0x{w:08x}u' for w, _, _ in picks) + '};\n')
        f.write(f'int ntests = {len(picks)};\n')
    exe = os.path.join(work, 'harness')
    subprocess.check_call([os.environ.get('NXRT_CC', '/usr/bin/clang'), '-O1', '-fno-strict-aliasing', '-Wno-return-type', '-I', os.path.join(ROOT, 'runtime'),
                           '-o', exe, os.path.join(ROOT, 'tools', 'test_lift_harness.c'), os.path.join(work, 'tests.c')])

    cases = []
    with open(os.path.join(work, 'cases.bin'), 'wb') as f:
        for k, (w, pc, body) in enumerate(picks):
            for _ in range(args.cases):
                if is_mem(w):  # address registers into the buffer (an index register small)
                    x = [BUF_MID + r.randrange(0, 0x100, 8) for _ in range(31)]
                    if bits(w, 21, 1) and bits(w, 10, 2) == 2 or (w & 0xBFA00000) == 0x0C800000:
                        rm = bits(w, 16, 5)
                        if rm != bits(w, 5, 5) and rm < 31:
                            x[rm] = r.randrange(0, 0x100)
                else:
                    x = [rand64(r) for _ in range(31)]
                sp = BUF_MID + r.randrange(0, 0x100, 16)
                v = [rand_v(r) for _ in range(32)]
                f.write(struct.pack('<QQ', k, free_reg(w, r)))
                f.write(struct.pack('<32Q', *(x + [0])))
                f.write(struct.pack('<QQ', sp, r.getrandbits(4) << 28))
                for q in v:
                    f.write(q.to_bytes(16, 'little'))
                cases.append(k)
    out = subprocess.run([exe, os.path.join(work, 'cases.bin')], capture_output=True, text=True)
    if out.returncode:
        print('harness failed', out.returncode, out.stderr[-2000:])
    results, fails = Counter(), {}
    for line in out.stdout.splitlines():
        parts = line.split(' ', 2)
        if len(parts) < 2:
            continue
        idx, kind = int(parts[0]), parts[1]
        results[kind if kind != 'skip' else 'skip ' + parts[2].split()[0]] += 1
        if kind == 'diff':
            w = picks[cases[idx]][0]
            fails.setdefault(w, (picks[cases[idx]][1], parts[2].strip()))
    print(f'{os.path.basename(args.nso)}: {len(picks)} instruction words, {len(cases)} cases: {dict(results)}')
    print(f'  failing words: {len(fails)}; not tested: {dict(skipped)}; unsupported by the lifter: {dict(unsupported)}')
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for w, (pc, d) in list(fails.items())[:args.show]:
        dis = next(md.disasm(struct.pack('<I', w), pc), None)
        print(f'  {w:08x} {dis.mnemonic + " " + dis.op_str if dis else "?":40} {d[:300]}')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
