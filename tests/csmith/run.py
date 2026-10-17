#!/usr/bin/env python3
"""Differential testing with csmith: random C programs, each run on the
host and on the emulated Propeller, must print the same checksum.

csmith (https://github.com/csmith-project/csmith) writes programs that
use only fixed-width integer types and end by printing a checksum of all
their variables.  For each seed:

  1. the program is generated (csmith --seed SEED, small programs);
  2. it is compiled for the host in 32 bits (gcc -m32, so that int, long
     and pointers are 32-bit as on the Propeller) at -O0 and at -O2, and
     run: if the two disagree, or it does not finish, or does not print
     a checksum, the program has undefined behaviour and is dropped;
  3. it is compiled with propeller-elf-gcc for each model and level and
     run in emu/'s p1run (from the emulated SPI SRAM for XMMC and XMM,
     through tests/xmm/mkxmm.py), linked with tests/torture/exit.c.

Results: MATCH, MISMATCH (a wrong-code bug, or something the host and
the Propeller may differ on), ICE, CE, NOFIT (too big, or leaving the
stack less than MIN_STACK bytes), TIMEOUT, CRASH (no checksum).  The sources and logs of every MISMATCH, ICE, CE, TIMEOUT
and CRASH are kept in OUT/SEED/.

Usage:
  tests/csmith/run.py [--count 100] [--seed 1] [--model xmmc,lmm]
                      [--opt=-O2,-Os] [-j N] [--out work/csmith]
                      [--csmith PATH] [--include DIR] [--seconds 30]
  --csmith: the csmith program (default: csmith on the PATH, or
  ~/csmith/root/usr/bin/csmith); --include: its runtime headers
  (csmith.h; default next to it, .../include/csmith).
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CC = "propeller-elf-gcc"
HOST_CC = os.environ.get("HOST_CC", "gcc")
# small programs: they must fit in hub RAM for LMM and CMM, and run in
# seconds in the emulator
CSMITH_OPTS = ["--max-funcs", "4", "--max-block-depth", "3", "--no-packed-struct"]
CHECKSUM = re.compile(rb"checksum = ([0-9A-F]+)")
# The stack is in hub RAM, below hub RAM's end in LMM and CMM, below the
# cache, its tags and the mailboxes of tests/xmm/xmm_boot.spin in the
# external models; nothing detects it running into the data.  A program
# that leaves it less than this is taken as not fitting (csmith programs
# at -O0 chain many calls with big frames).
MIN_STACK = 4096


def run(cmd, timeout, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, timeout=timeout, **kw)
    except subprocess.TimeoutExpired:
        return None


def host_checksum(src, tmp, include):
    sums = []
    for opt in ("-O0", "-O2"):
        exe = os.path.join(tmp, "host" + opt)
        r = run([HOST_CC, "-m32", opt, "-w", "-I", include, src, "-o", exe], 120)
        if r is None or r.returncode:
            return None
        r = run([exe], 5)
        if r is None or r.returncode:
            return None
        m = CHECKSUM.search(r.stdout)
        if not m:
            return None
        sums.append(m.group(1).decode())
    return sums[0] if sums[0] == sums[1] else None


def hub_end(elf):
    out = subprocess.run(["propeller-elf-nm", elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "__hub_end":
            return int(parts[0], 16)
    return 0


def propeller(src, tmp, model, opt, include, exit_obj, args):
    """(status, checksum or detail, log)"""
    elf = os.path.join(tmp, "p%s%s.elf" % (model, opt))
    cmd = [CC, opt, "-m" + model, "-w", "-I", include, src, exit_obj, "-o", elf]
    r = run(cmd, 600)
    if r is None:
        return "ICE", "compiler timeout", " ".join(cmd)
    text = (r.stdout + r.stderr).decode("latin-1")
    log = "$ " + " ".join(cmd) + "\n" + text
    if r.returncode:
        if "internal compiler error" in text or "Segmentation fault" in text:
            return "ICE", text.strip().splitlines()[0][:160], log
        if "overflowed" in text or "not fit" in text:
            return "NOFIT", "", log
        if model == "cmm" and ("Relocation overflows" in text or "too large for field" in text):
            # a CMM branch reaches +-32 KB: a function bigger than that,
            # in a model whose code is in the 32 KB hub
            return "NOFIT", "function over 32 KB", log
        return "CE", text.strip().splitlines()[0][:160] if text.strip() else "", log
    if model in ("lmm", "cmm"):         # (mkxmm.py checks the external models)
        room = 32768 - hub_end(elf)
        if room < MIN_STACK:
            return "NOFIT", "stack room %d bytes" % room, log
    if model in ("lmm", "cmm"):
        r = run(["propeller-load", "-s", elf], 60, cwd=tmp)
        sram = []
    else:
        r = run([sys.executable, os.path.join(ROOT, "tests", "xmm", "mkxmm.py"),
                 "--min-stack", str(MIN_STACK), elf], 60)
        sram = ["--spi-sram", elf[:-4] + ".xmm"]
    if r is None or r.returncode:
        text = "" if r is None else (r.stdout + r.stderr).decode("latin-1")
        return ("NOFIT" if "not fit" in text else "CE"), "image", log + text
    secs = args.seconds * (1 if model in ("lmm", "cmm") else 3)
    cmd = [args.p1run, elf[:-4] + ".binary"] + sram + ["--seconds", "%g" % secs]
    r = run(cmd, secs * 20 + 60)
    if r is None:
        return "TIMEOUT", "host timeout", log
    out = r.stdout + r.stderr
    log += "$ " + " ".join(cmd) + "\n" + out.decode("latin-1")
    m = CHECKSUM.search(r.stdout)
    if m:
        return "SUM", m.group(1).decode(), log
    if b"time limit" in r.stderr:
        return "TIMEOUT", "%g s" % secs, log
    return "CRASH", r.stderr.decode("latin-1").strip().splitlines()[-1] if r.stderr else "", log


def one_seed(seed, args, configs, exit_objs):
    tmp = tempfile.mkdtemp(prefix="csmith-")
    try:
        src = os.path.join(tmp, "t%d.c" % seed)
        with open(src, "wb") as f:
            r = run([args.csmith, "--seed", str(seed)] + CSMITH_OPTS, 60)
            if r is None or r.returncode:
                return seed, None, []
            f.write(r.stdout)
        want = host_checksum(src, tmp, args.include)
        if want is None:
            return seed, None, []          # undefined behaviour, or too slow on the host
        results, keep = [], []
        for model, opt in configs:
            status, detail, log = propeller(src, tmp, model, opt, args.include,
                                            exit_objs[(model, opt)], args)
            if status == "SUM":
                status = "MATCH" if detail == want else "MISMATCH"
                detail = "%s, host %s" % (detail, want)
            results.append((model, opt, status, detail))
            if status in ("MISMATCH", "ICE", "CE", "TIMEOUT", "CRASH"):
                keep.append((model, opt, log))
        if keep:
            d = os.path.join(args.out, str(seed))
            os.makedirs(d, exist_ok=True)
            shutil.copy(src, d)
            for model, opt, log in keep:
                with open(os.path.join(d, "%s%s.log" % (model, opt)), "w", encoding="utf-8") as f:
                    f.write(log)
        return seed, want, results
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description="csmith differential tests on the emulated Propeller")
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--model", default="xmmc,lmm")
    ap.add_argument("--opt", default="-O2,-Os")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "csmith"))
    ap.add_argument("--csmith", default=shutil.which("csmith")
                    or os.path.expanduser("~/csmith/root/usr/bin/csmith"))
    ap.add_argument("--include")
    ap.add_argument("--p1run", default=os.path.join(ROOT, "emu", "build", "p1run"))
    ap.add_argument("--seconds", type=float, default=30)
    args = ap.parse_args()
    if not args.include:
        args.include = os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(args.csmith))),
                                    "include", "csmith")
    if not os.path.exists(os.path.join(args.include, "csmith.h")):
        sys.exit("no csmith.h in %s: give --include" % args.include)
    if not os.path.exists(args.p1run) and os.path.exists(args.p1run + ".exe"):
        args.p1run += ".exe"
    os.makedirs(args.out, exist_ok=True)

    configs = [(m, o) for m in args.model.split(",") for o in args.opt.split(",")]
    exit_objs = {}
    for model, opt in configs:
        exit_objs[(model, opt)] = os.path.join(args.out, "exit-%s%s.o" % (model, opt))
        subprocess.run([CC, opt, "-m" + model, "-c", "-o", exit_objs[(model, opt)],
                        os.path.join(ROOT, "tests", "torture", "exit.c")], check=True)

    counts = {}
    dropped = 0
    seeds = range(args.seed, args.seed + args.count)
    with open(os.path.join(args.out, "results.txt"), "a", encoding="utf-8") as res, \
            concurrent.futures.ThreadPoolExecutor(args.j) as pool:
        for seed, want, results in pool.map(lambda s: one_seed(s, args, configs, exit_objs), seeds):
            if want is None:
                dropped += 1
                continue
            for model, opt, status, detail in results:
                counts[status] = counts.get(status, 0) + 1
                res.write("%d %s %s %s %s\n" % (seed, model, opt, status, detail))
                if status not in ("MATCH", "NOFIT"):
                    print("seed %d %s %s: %s %s" % (seed, model, opt, status, detail), flush=True)
    print("%d seeds (%d dropped: undefined behaviour or slow on the host); %s"
          % (args.count, dropped, "  ".join("%s %d" % kv for kv in sorted(counts.items()))))
    print("results in " + args.out)
    sys.exit(1 if any(s in counts for s in ("MISMATCH", "ICE", "CE", "CRASH")) else 0)


if __name__ == "__main__":
    main()
