#!/usr/bin/env python3
"""Run GCC's gcc.c-torture/execute tests on the emulated Propeller.

Each test is compiled with the installed propeller-elf-gcc for one memory
model and one optimisation level, linked with exit.c (exit and abort end
with propgcc's serial exit sequence), and run in emu/'s p1run: from hub
RAM for LMM and CMM, from the emulated SPI SRAM for XMMC and the XMM
models (through tests/xmm/mkxmm.py).  A test passes when it exits with
status 0; abort() gives 255.

Results, one line per test, in OUT/MODEL-OPT/results.txt:
  PASS        exit status 0
  FAIL        another status, or the program stopped without exiting
  TIMEOUT     still running after the time limit
  ICE         the compiler crashed (internal compiler error)
  CE          another compile or link error
  NOFIT       too big for the memory model (the linker says so)
  UNSUPPORTED a dg- directive asks for something the target lacks
  XFAIL/XPASS a dg-xfail directive applies, and it failed / passed
Logs of the tests that did not pass are in OUT/MODEL-OPT/logs.

The tests' DejaGnu directives are read as GCC's torture driver would:
dg-options and dg-additional-options add options, dg-require-* and
dg-skip-if can leave a test out, dg-xfail-* mark it as expected to
fail, dg-add-options stack_size defines STACK_SIZE, dg-timeout-factor
stretches the time limit.  Target selectors are
evaluated for propeller-unknown-elf with the effective targets of
TARGET below.

Usage:
  tests/torture/run.py [--model lmm] [--opt -O2] [-j N] [--src DIR]
                       [--p1run emu/build/p1run] [--out DIR] [--known FILE]
                       [--seconds 20] [tests...]
  --model and --opt take comma-separated lists; tests are names without
  .c (shell patterns allowed); --src is the GCC source tree (default:
  work/gcc-16.2.0 next to build.sh, or $GCC_SRC); --known lists failures
  that are not to fail the run (see known.txt).
"""
import argparse
import concurrent.futures
import fnmatch
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TRIPLET = "propeller-unknown-elf"
CC = "propeller-elf-gcc"
BASE_FLAGS = ["-m64bit-doubles", "-w", "-fno-diagnostics-color"]
# The stack the tests may count on: dg-add-options stack_size defines
# STACK_SIZE (the tests then scale their arrays), dg-require-stack-size
# above it is UNSUPPORTED.  The stack is in hub RAM in every model.
STACK_SIZE = 8192

# Effective targets (gcc/testsuite/lib/target-supports.exp) as they hold
# for propgcc: 32-bit int, long and pointers, 64-bit long long, 64-bit
# double with -m64bit-doubles; no files, no mmap, no __int128.
TARGET = {
    "int32plus": True, "int32": True, "int16": False, "longlong64": True,
    "ptr32plus": True, "size32plus": True, "size20plus": True, "lp64": False,
    "ilp32": True, "double64": True, "double64plus": True, "large_double": True,
    "stdint_types": True, "c99_runtime": True, "unwrapped": True,
    "label_values": True, "indirect_jumps": True, "indirect_calls": True,
    "trampolines": True, "nonlocal_goto": True, "return_address": True,
    "untyped_assembly": True, "alloca": True, "signal": False,
    "int128": False, "mmap": False, "fileio": False, "dfp": False, "dfprt": False,
    "run_expensive_tests": False, "lto": False, "pthread": False,
    "sync_char_short": False, "sync_int_long": False, "vect_int": False,
    "avx_runtime": False, "sse2_runtime": False, "ieee": True,
    "nonpic": True, "keeps_null_pointer_checks": False, "wchar": True,
    "hard_float": False, "bitint": False, "fopenacc": False, "fopenmp": False,
}
unknown_targets = set()


# --- DejaGnu directives --------------------------------------------------

def tcl_words(text):
    """Split a Tcl list: words, "quoted strings" and {braced groups}."""
    words, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "{":
            depth, j = 1, i + 1
            while j < n and depth:
                depth += {"{": 1, "}": -1}.get(text[j], 0)
                j += 1
            words.append(("brace", text[i + 1:j - 1]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            words.append(("quote", text[i + 1:j]))
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "{}":
                j += 1
            words.append(("word", text[i:j]))
            i = j
    return words


def directives(source):
    """The dg- directives of a test: (name, [args as (kind, text)])."""
    out = []
    for m in re.finditer(r"\{\s*dg-", source):
        depth, j = 0, m.start()
        while j < len(source):
            depth += {"{": 1, "}": -1}.get(source[j], 0)
            j += 1
            if depth == 0:
                break
        words = tcl_words(source[m.start() + 1:j - 1])
        if words:
            out.append((words[0][1], words[1:]))
    return out


def selector_true(text):
    """A target selector: "target LIST", "target { EXPR }", or a bare list
    (dg-skip-if, dg-xfail-if) of triplet patterns and effective targets,
    with !, && and ||."""
    words = tcl_words(text)
    if words and words[0] == ("word", "target"):
        words = words[1:]
    elif words and words[0][1] in ("xfail", "skip"):
        words = words[1:]
    return expr_true(words)


def expr_true(words):
    # a list of alternatives, each a term possibly joined by && / ||
    result, op, neg, any_term = False, None, False, False
    for kind, w in words:
        if kind == "word" and w == "!":
            neg = not neg
            continue
        if kind == "word" and w in ("&&", "||"):
            op = w
            continue
        value = expr_true(tcl_words(w)) if kind == "brace" else atom_true(w)
        if neg:
            value, neg = not value, False
        if not any_term:
            result = value
        elif op == "&&":
            result = result and value
        else:
            result = result or value
        any_term, op = True, None
    return result


def atom_true(w):
    if w in ("any-opts", "no-opts"):
        return w == "no-opts"
    if "-" in w or "*" in w:
        return fnmatch.fnmatchcase(TRIPLET, w)
    if w not in TARGET:
        unknown_targets.add(w)
    return TARGET.get(w, False)


def opts_match(patterns, flags):
    """dg-skip-if / dg-xfail-if option lists: any pattern matching the flags."""
    joined = " " + " ".join(flags) + " "
    for kind, p in tcl_words(patterns):
        if p == "":
            continue
        if p == "*" or fnmatch.fnmatchcase(joined, "* " + p + " *"):
            return True
    return False


def plan(path, flags):
    """What to do with one test: (status or None, extra flags, xfail,
    timeout factor, compile only, reason)."""
    source = open(path, encoding="utf-8", errors="replace").read()
    extra, xfail, factor, compile_only = [], False, 1.0, False
    for name, args in directives(source):
        texts = [a[1] for a in args]
        if name in ("dg-options", "dg-additional-options"):
            if not texts:
                continue
            if len(texts) > 1 and not selector_true(texts[1]):
                continue
            extra += shlex.split(texts[0])
        elif name == "dg-require-effective-target":
            if texts and not (len(texts) > 1 and not selector_true(texts[1])):
                t = texts[0]
                if t not in TARGET:
                    unknown_targets.add(t)
                if not TARGET.get(t, False):
                    return "UNSUPPORTED", [], False, 1, False, "needs " + t
        elif name == "dg-require-stack-size":
            try:
                need = int(eval(texts[0], {"__builtins__": {}}))
            except Exception:
                need = 0
            if need > STACK_SIZE:
                return "UNSUPPORTED", [], False, 1, False, "stack %d" % need
        elif name == "dg-skip-if":
            sel = texts[1] if len(texts) > 1 else "*-*-*"
            inc = texts[2] if len(texts) > 2 else "*"
            exc = texts[3] if len(texts) > 3 else ""
            if selector_true(sel) and opts_match(inc, flags) and not opts_match(exc, flags):
                return "UNSUPPORTED", [], False, 1, False, "dg-skip-if " + texts[0]
        elif name in ("dg-xfail-if", "dg-xfail-run-if"):
            sel = texts[1] if len(texts) > 1 else "*-*-*"
            inc = texts[2] if len(texts) > 2 else "*"
            exc = texts[3] if len(texts) > 3 else ""
            if selector_true(sel) and opts_match(inc, flags) and not opts_match(exc, flags):
                xfail = True
        elif name == "dg-add-options":
            if texts and texts[0] == "stack_size":
                extra.append("-DSTACK_SIZE=%d" % STACK_SIZE)
        elif name == "dg-timeout-factor":
            try:
                factor = float(texts[0])
            except (IndexError, ValueError):
                pass
        elif name == "dg-do":
            if texts and texts[0] in ("compile", "link", "assemble", "preprocess"):
                if len(texts) < 2 or selector_true(texts[1]):
                    compile_only = True
    return None, extra, xfail, factor, compile_only, ""


# --- running ---------------------------------------------------------------

def run_one(path, model, opt, exit_obj, args, logdir):
    name = os.path.splitext(os.path.basename(path))[0]
    flags = [opt, "-m" + model] + BASE_FLAGS
    status, extra, xfail, factor, compile_only, reason = plan(path, flags)
    if status:
        return name, status, reason
    tmp = tempfile.mkdtemp(prefix="torture-")
    log = []
    try:
        elf = os.path.join(tmp, name + ".elf")
        cmd = [CC] + flags + extra + [path]
        cmd += ["-c", "-o", elf] if compile_only else [exit_obj, "-lm", "-o", elf]
        log.append("$ " + " ".join(shlex.quote(c) for c in cmd))
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        except subprocess.TimeoutExpired:
            return finish(name, "ICE", "compiler timeout", log, logdir)
        log.append(r.stdout + r.stderr)
        if r.returncode:
            text = r.stderr
            if "internal compiler error" in text or "Segmentation fault" in text:
                return finish(name, "ICE", first_line(text, "internal compiler error"), log, logdir)
            if "overflowed" in text or "will not fit" in text:
                return name, "NOFIT", first_line(text, "overflowed")
            return finish(name, "CE", first_line(text, "error"), log, logdir)
        if compile_only:
            return name, "PASS", "compiled"
        if model in ("lmm", "cmm"):
            r = subprocess.run(["propeller-load", "-s", elf], capture_output=True, text=True,
                               cwd=tmp, timeout=60)
            sram = []
        else:
            r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "xmm", "mkxmm.py"), elf],
                               capture_output=True, text=True, timeout=60)
            sram = ["--spi-sram", elf[:-4] + ".xmm"]
        if r.returncode:
            log.append(r.stdout + r.stderr)
            if "do not fit" in r.stdout + r.stderr:
                return name, "NOFIT", "external image too big"
            return finish(name, "CE", "image: " + first_line(r.stdout + r.stderr, ""), log, logdir)
        secs = args.seconds * factor
        cmd = [args.p1run, elf[:-4] + ".binary"] + sram + ["--seconds", "%g" % secs]
        log.append("$ " + " ".join(cmd))
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=secs * 20 + 60)
        except subprocess.TimeoutExpired:
            return finish(name, "TIMEOUT", "host timeout", log, logdir)
        out = r.stdout.decode("latin-1")
        err = r.stderr.decode("latin-1")
        log.append(out + err)
        m = re.search(r"\[p1run: ([0-9.]+) s emulated, ([^\]]*)\]", err)
        why = m.group(2) if m else "no p1run summary"
        if why == "exit 0":
            status, reason = "PASS", ""
        elif why == "time limit":
            status, reason = "TIMEOUT", "%g s" % secs
        else:
            status, reason = "FAIL", why
        if xfail:
            status = "XPASS" if status == "PASS" else "XFAIL"
        if status in ("PASS", "XFAIL"):
            return name, status, reason
        return finish(name, status, reason, log, logdir)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def first_line(text, key):
    for line in text.splitlines():
        if key in line:
            return line.strip()[:160]
    lines = text.strip().splitlines()
    return lines[0][:160] if lines else ""


def finish(name, status, reason, log, logdir):
    with open(os.path.join(logdir, name + ".log"), "w", encoding="utf-8") as f:
        f.write("\n".join(log))
    return name, status, reason


def load_known(path):
    """known.txt: 'TEST STATUS [MODELS] [OPTS]  # why'; MODELS and OPTS are
    comma-separated or '*'."""
    known = []
    if path and os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                known.append((line[0], line[1], (line[2] if len(line) > 2 else "*").split(","),
                              (line[3] if len(line) > 3 else "*").split(",")))
    return known


def is_known(known, name, status, model, opt):
    for t, s, models, opts in known:
        if (fnmatch.fnmatchcase(name, t) and s == status
                and ("*" in models or model in models) and ("*" in opts or opt in opts)):
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description="GCC torture execute tests on the emulated Propeller")
    ap.add_argument("tests", nargs="*")
    ap.add_argument("--model", default="lmm")
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--src", default=os.environ.get("GCC_SRC", os.path.join(ROOT, "work", "gcc-16.2.0")))
    ap.add_argument("--p1run", default=os.path.join(ROOT, "emu", "build", "p1run"))
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "torture"))
    ap.add_argument("--known", default=os.path.join(HERE, "known.txt"))
    ap.add_argument("--seconds", type=float, default=20)
    args = ap.parse_args()

    tdir = os.path.join(args.src, "gcc", "testsuite", "gcc.c-torture", "execute")
    paths = sorted(glob.glob(os.path.join(tdir, "*.c")))
    if not paths:
        sys.exit("no tests in %s (--src: the GCC 16.2.0 source tree)" % tdir)
    if args.tests:
        paths = [p for p in paths
                 if any(fnmatch.fnmatchcase(os.path.basename(p)[:-2], t) for t in args.tests)]
    if not os.path.exists(args.p1run) and os.path.exists(args.p1run + ".exe"):
        args.p1run += ".exe"
    known = load_known(args.known)

    bad = 0
    for model in args.model.split(","):
        for opt in args.opt.split(","):
            cfg = os.path.join(args.out, model + opt)
            logdir = os.path.join(cfg, "logs")
            shutil.rmtree(cfg, ignore_errors=True)
            os.makedirs(logdir)
            exit_obj = os.path.join(cfg, "exit.o")
            subprocess.run([CC, opt, "-m" + model, "-c", "-o", exit_obj, os.path.join(HERE, "exit.c")],
                           check=True)
            with concurrent.futures.ThreadPoolExecutor(args.j) as pool:
                jobs = [pool.submit(run_one, p, model, opt, exit_obj, args, logdir) for p in paths]
                results = [j.result() for j in jobs]
            counts, new = {}, []
            with open(os.path.join(cfg, "results.txt"), "w", encoding="utf-8") as f:
                for name, status, reason in results:
                    counts[status] = counts.get(status, 0) + 1
                    f.write("%-11s %-24s %s\n" % (status, name, reason))
                    if status in ("FAIL", "TIMEOUT", "ICE", "CE", "XPASS") and \
                            not is_known(known, name, status, model, opt):
                        new.append((name, status, reason))
            summary = "  ".join("%s %d" % (s, counts[s]) for s in
                                ("PASS", "FAIL", "TIMEOUT", "ICE", "CE", "NOFIT", "UNSUPPORTED",
                                 "XFAIL", "XPASS") if s in counts)
            print("%-14s %s" % (model + " " + opt, summary))
            for name, status, reason in new:
                print("  new %-8s %-24s %s" % (status, name, reason))
            bad += len(new)
    if unknown_targets:
        print("effective targets not in TARGET (taken as false): " + " ".join(sorted(unknown_targets)))
    print("results in " + args.out)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
