#!/usr/bin/env python3
"""Explain an nxrt guest crash: map the log's f_<guest addr> back to its NSO function and generated C.

    python3 tools/crash_report.py --port /path/to/port --log run.log
"""
import argparse, glob, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso  # noqa: E402
from analyze import eh_frame_functions  # noqa: E402
import capstone  # noqa: E402


def port_modules(port):
    mods = []
    mk = os.path.join(port, "Makefile")
    if os.path.exists(mk):
        for line in open(mk, encoding="utf-8", errors="replace"):
            m = re.match(r"\s*GAME_MODULES\s*:?=\s*(.*)", line)
            if not m:
                continue
            for item in m.group(1).split():
                if ":" in item:
                    name, base = item.split(":", 1)
                    try:
                        mods.append((name, int(base, 0)))
                    except ValueError:
                        pass
    if not mods:
        mods = [("main", 0x7000000000), ("sdk", 0x7006000000)]
    return mods


def parse_crash(log):
    with open(log, encoding="utf-8", errors="replace") as f:
        text = f.read()
    blocks = re.split(r"(?=\*\*\* signal )", text)
    candidates = []
    for b in blocks:
        if not b.startswith("*** signal "):
            continue
        sig = re.search(r"\*\*\* signal (\d+) accessing (0x[0-9a-fA-F]+)", b)
        pc = re.search(r"host pc\s+[0-9a-f]+ \(f_([0-9a-f]+)\+([0-9a-f]+)\)", b)
        lr = re.search(r"host lr\s+[0-9a-f]+ \(f_([0-9a-f]+)", b)
        if not pc:
            continue
        regs = {}
        rm = re.search(r"guest: sp=([0-9a-f]+) lr=([0-9a-f]+)\n((?:\s+x\d+\s+[0-9a-f]+\n?)+)", b)
        if not rm:
            rm = re.search(r"guest: sp=([0-9a-f]+) lr=([0-9a-f]+)\n((?:\s+x\d+\s+[0-9a-f]+\n?)+)", text)
        if rm:
            regs["sp"], regs["lr"] = rm.group(1), rm.group(2)
            for m in re.finditer(r"x(\d+)\s+([0-9a-f]+)", rm.group(3)):
                regs["x" + m.group(1)] = m.group(2)
        th = re.search(r"\[crash\] thread=(\d+) cpu=(0x[0-9a-f]+) sp=([0-9a-f]+)", b)
        bt = [m.group(1) for m in re.finditer(r"f_([0-9a-f]+) \+", b)]
        bt_off = [m.group(2) for m in re.finditer(r"f_([0-9a-f]+) \+ ([0-9a-f]+)", b)]
        bt_pairs = [(m.group(1), m.group(2)) for m in re.finditer(r"f_([0-9a-f]+) \+ ([0-9a-f]+)", b)]
        if not bt_pairs:
            bt_pairs = [(m.group(1), m.group(2)) for m in re.finditer(r"f_([0-9a-f]+) \+ ([0-9a-f]+)", text)]
        frames = [m.group(1) for m in re.finditer(r"guest frame \d+: lr=([0-9a-f]+)", b)]
        if not frames:
            frames = [m.group(1) for m in re.finditer(r"guest frame \d+: lr=([0-9a-f]+)", text)]
        candidates.append({"signal": sig.group(1) if sig else "?", "access": sig.group(2) if sig else "?",
                           "pc": int(pc.group(1), 16), "pc_off": int(pc.group(2), 16), "lr": int(lr.group(1), 16) if lr else None,
                           "thread": th.group(1) if th else None, "cpu": th.group(2) if th else None,
                           "cpu_sp": th.group(3) if th else None,
                           "bt_frames": bt_pairs,
                           "regs": regs, "backtrace": bt, "guest_frames": frames, "raw": b})
    return (next((x for x in candidates if x["regs"]), candidates[0]) if candidates else None)


def find_source(port, addr):
    pat = re.compile(r"void f_%x\(" % addr)
    for p in glob.glob(os.path.join(port, "generated", "*", "*.c")):
        try:
            with open(p, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        except OSError:
            continue
        hits = [i for i, line in enumerate(lines) if pat.search(line)]
        for i in hits:
            if "(Cpu *CPU_RESTRICT" in lines[i] or (i + 1 < len(lines) and lines[i + 1].strip() == "{"):
                return p, i + 1, lines[i:i + 45]
        if hits:
            i = hits[-1]
            return p, i + 1, lines[i:i + 45]
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    c = parse_crash(a.log)
    if not c:
        print("no guest crash found in log", file=sys.stderr)
        return 2
    modules = port_modules(a.port)
    modname, base = None, 0
    for name, b in modules:
        if c["pc"] >= b and c["pc"] < b + 0x10000000:
            modname, base = name, b
    exefs = os.path.join(a.port, "data", "extracted", "exefs")
    nso_path = os.path.join(exefs, modname or "main")
    info = {"pc": "0x%x" % c["pc"], "signal": c["signal"], "access": c["access"], "module": modname,
            "base": "0x%x" % base, "offset": "0x%x" % (c["pc"] - base), "regs": c["regs"]}
    if os.path.exists(nso_path):
        n = Nso(nso_path)
        starts = [base + x for x in eh_frame_functions(n)] if n.eh_start else []
        starts = sorted(set(starts + [s[3] + base for s in n.syms if s[2] and (s[1] & 0xF) == 2]))
        start = max([s for s in starts if s <= c["pc"]], default=None)
        info["function"] = "0x%x" % start if start else None
        info["function_offset"] = "0x%x" % (c["pc"] - start) if start else None
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        at = start if start else c["pc"]
        off = at - base
        info["disassembly"] = [{"address": "0x%x" % i.address, "text": f"{i.mnemonic} {i.op_str}".strip()}
                               for i in md.disasm(bytes(n.image[off:off + 64]), at)]
    src = find_source(a.port, c["pc"])
    if src:
        info["source"], info["line"] = src[0], src[1]
    if a.json:
        import json
        print(json.dumps(info, indent=2, sort_keys=True))
        return 0
    print(f"signal {c['signal']} accessing {c['access']} at guest f_{c['pc']:x} ({modname} +{c['pc']-base:#x})")
    if c.get("thread"):
        print(f"thread {c['thread']} cpu={c['cpu']} cpu_sp={c['cpu_sp']}")
    frames = c.get("bt_frames") or []
    if frames and (int(frames[0][0], 16) != c["pc"] or int(frames[0][1], 16) != c["pc_off"]):
        print(f"note: host pc label disagrees with the host backtrace ({frames[0][0]}+{frames[0][1]}); "
              "use the backtrace offset, the pc label is nearest-symbol only")
    if info.get("function"):
        print(f"function {info['function']} + {info['function_offset']}")
    for i in info.get("disassembly", [])[:10]:
        print(f"  {i['address']}  {i['text']}")
    if c["access"].startswith("0x"):
        a = int(c["access"], 16)
        if a < 0x10000:
            same = [k for k, v in c["regs"].items() if k.startswith("x") and int(v, 16) == a]
            if same:
                print(f"hint: {', '.join(same)} equals the fault address: an argument/register is being used as a pointer")
            else:
                print("hint: tiny fault address: inspect guest SP/stack setup or a small pointer argument")
    for k in ("x0", "x1", "x2", "x3", "x4", "x5", "x29", "sp", "lr"):
        if k in c["regs"]:
            print(f"  {k} = 0x{c['regs'][k]}")
    if c.get("guest_frames"):
        print("guest frames (lr): " + " -> ".join("0x" + x for x in c["guest_frames"][:8]))
    if src:
        print(f"\n{src[0]}:{src[1]}")
        for j, line in enumerate(src[2]):
            print(f"{src[1]+j:6d}  {line}")
    print("\nlast kernel/HLE lines:")
    with open(a.log, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines[-80:]:
        if "[kernel]" in line or "[hle]" in line or "[app] [fs]" in line:
            print("  " + line[-200:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
