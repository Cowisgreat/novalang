#!/usr/bin/env python3
"""NovaLang test runner.

Each tests/cases/*.nov file describes its expected behaviour in comments:

    // expect: <line>      one line of expected stdout (repeat for each line)
    // exit: <code>        expected process exit code (default 0)
    // error: <text>       compilation must fail with <text> in its diagnostics
    // runtime-error: <text>  the program must abort (exit 101) with <text> on stderr
    // stats: <text>       `novac --stats` must report <text> (e.g. "bounds checks:   0 emitted, 2 elided")

Usage: run_tests.py <path-to-novac> <cases-dir> [filter]
Extra compiler flags can be passed through NOVAC_FLAGS, e.g. NOVAC_FLAGS=-O2.
Set NOVAC_MODE=jit to run every program with `novac --run` instead of compiling an executable.
"""
import os
import re
import subprocess
import sys
import tempfile

DIRECTIVE = re.compile(r"//\s*(expect|exit|error|runtime-error|stats):\s?(.*)$")


def parse(path):
    spec = {"expect": [], "exit": 0, "errors": [], "runtime": [], "stats": []}
    with open(path) as f:
        for line in f:
            m = DIRECTIVE.search(line)
            if not m:
                continue
            kind, value = m.group(1), m.group(2).rstrip()
            if kind == "expect":
                spec["expect"].append(value)
            elif kind == "exit":
                spec["exit"] = int(value)
            elif kind == "stats":
                spec["stats"].append(value)
            elif kind == "runtime-error":
                spec["runtime"].append(value)
                spec["exit"] = 101
            else:
                spec["errors"].append(value)
    return spec


def run_case(novac, path, workdir):
    spec = parse(path)
    exe = os.path.join(workdir, os.path.basename(path) + ".out")
    flags = os.environ.get("NOVAC_FLAGS", "").split()
    if spec["stats"]:
        flags.append("--stats")
    jit = os.environ.get("NOVAC_MODE") == "jit" and not spec["errors"]
    if jit:
        # Compile and run in one step; stdout/stderr/exit come from the program itself.
        result = subprocess.run([novac, path, "--run", *flags], capture_output=True, text=True, timeout=60)
        return check_run(spec, result, result.stderr)

    compile_ = subprocess.run([novac, path, "-o", exe, *flags], capture_output=True, text=True, timeout=60)

    if spec["errors"]:
        if compile_.returncode == 0:
            return "expected a compile error, but compilation succeeded"
        for err in spec["errors"]:
            if err not in compile_.stderr:
                return f"missing diagnostic {err!r}; got:\n{compile_.stderr}"
        return None

    if compile_.returncode != 0:
        return f"compilation failed:\n{compile_.stderr}"
    result = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    return check_run(spec, result, compile_.stderr)


def check_run(spec, result, compile_stderr):
    for stat in spec["stats"]:
        if stat not in compile_stderr:
            return f"missing stats line {stat!r}; got:\n{compile_stderr}"
    actual = result.stdout.splitlines()
    if actual != spec["expect"]:
        return "output mismatch\n  expected: %r\n  actual:   %r" % (spec["expect"], actual)
    if result.returncode != spec["exit"]:
        return f"exit code {result.returncode}, expected {spec['exit']}"
    for err in spec["runtime"]:
        if err not in result.stderr:
            return f"missing runtime error {err!r}; got:\n{result.stderr}"
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    novac, cases_dir = os.path.abspath(sys.argv[1]), sys.argv[2]
    pattern = sys.argv[3] if len(sys.argv) > 3 else ""
    cases = sorted(
        os.path.join(cases_dir, f) for f in os.listdir(cases_dir) if f.endswith(".nov") and pattern in f
    )

    failures = []
    with tempfile.TemporaryDirectory() as workdir:
        for path in cases:
            name = os.path.basename(path)
            err = run_case(novac, path, workdir)
            if err:
                failures.append(name)
                print(f"FAIL {name}\n  {err}")
            else:
                print(f"PASS {name}")

    print(f"\n{len(cases) - len(failures)}/{len(cases)} tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
