"""Run the Debug unit suite (gnss_tests.exe) as N GoogleTest shards at once and merge the results.

The full suite runs one case after another in one process (about 51 to 55 minutes on a loaded
machine, 2026-11-20 to 11-22).  GoogleTest's own sharding (GTEST_TOTAL_SHARDS, GTEST_SHARD_INDEX)
splits the selected cases round-robin over N processes; this script starts the N processes
together, waits for them, and merges their XML reports into one summary.

What the cases share, and what each shard gets of its own (read from tests/, 2026-11-23):

  * The working directory.  Cases read reference files through absolute paths compiled into the
    binary (GNSS_PROJECT_ROOT, GNSS_TEST_DATA_DIR) and a few through working-directory paths
    (data/static/nc/EOT20_load.nc, for example), and no case writes there: a full run leaves no
    file in the checkout or the build tree.  Every shard therefore runs in the SAME working
    directory as an unsharded run (default: the checkout of the binary), so a file a case looks for is
    found, or missing, exactly as in an unsharded run.
  * The system temp directory.  About 230 test files take a per-process, per-test directory from
    tests/support/TestTempDir.hpp, but some fifteen still write fixed names directly under
    std::filesystem::temp_directory_path() (config_load_capture_<tag>, gnss_chain_dump_case,
    network_live_record_test, ...).  Each shard gets a temp directory of its own (TMP and TEMP,
    which temp_directory_path() reads on Windows), removed after the run.
  * Loopback sockets.  The two socket suites (NTRIP credentials, TCP silence) bind port 0, an
    ephemeral port, so shards never collide.
  * Order.  A shard runs a case without the cases that preceded it in an unsharded run.  The
    merged result is compared case by case with an unsharded run (``compare``).

Cases that cannot share a run go in SERIAL_TAIL with the reason; they are left out of the
shards and run afterwards in one process of their own.  The table is empty: no case was found
that needs it (proof runs of 2026-11-23 on main 33c5fef).

Usage:
  python scripts/unit_shards.py run [--shards 6] [--exe build/x64-Debug/gnss_tests.exe]
                                    [--filter PATTERN] [--out DIR] [--cwd DIR] [--keep-tmp]
  python scripts/unit_shards.py compare DIR_A DIR_B     # two result folders of `run`
  python scripts/unit_shards.py selftest

``run --shards 1`` is the unsharded run: one process, the caller's environment unchanged (no
shard variables, the caller's temp directory), with the same report.

Output folder of ``run`` (default <temp>/unit_shards_<time>): summary.txt (the summary printed at
the end), summary.json, cases.tsv (every case: name, result, seconds, shard), skipped.txt,
failed.txt, and per shard shard_<k>.log (its console output) and shard_<k>.xml (its report).

Exit code of ``run``: 0 when every selected case ran exactly once and none failed and every
process exited 0; 1 otherwise (a failed case, a crashed or nonzero process, a case missing or
run twice); 2 on a usage error.  ``compare`` exits 0 when the two runs give every case the same
result, 1 otherwise.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build" / "x64-Debug" / "gnss_tests.exe"

# (GoogleTest pattern, reason): cases that cannot run beside other shards.  Empty: see the docstring.
SERIAL_TAIL = []

SHARD_ENV = ("GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX", "GTEST_SHARD_STATUS_FILE", "GTEST_OUTPUT",
             "GTEST_FILTER")

PASSED, SKIPPED, FAILED = "passed", "skipped", "failed"


# ---------------------------------------------------------------------------------------------
# GoogleTest filter and listing
# ---------------------------------------------------------------------------------------------

def _pattern_regex(pattern):
    """Regex of one GoogleTest pattern: '*' any string, '?' one character, the rest literal."""
    out = []
    for ch in pattern:
        out.append(".*" if ch == "*" else "." if ch == "?" else re.escape(ch))
    return re.compile("".join(out) + r"\Z")


def filter_matcher(gtest_filter):
    """A function name -> bool with GoogleTest's filter semantics ('POS1:POS2-NEG1:NEG2')."""
    pos, _, neg = gtest_filter.partition("-")
    pos_re = [_pattern_regex(p) for p in pos.split(":") if p] or [_pattern_regex("*")]
    neg_re = [_pattern_regex(p) for p in neg.split(":") if p]
    return lambda name: (any(r.match(name) for r in pos_re) and not any(r.match(name) for r in neg_re))


def add_negatives(gtest_filter, patterns):
    """The filter with @p patterns appended to its negative part."""
    if not patterns:
        return gtest_filter
    sep = ":" if "-" in gtest_filter else "-"
    return gtest_filter + sep + ":".join(patterns)


_SUITE_LINE = re.compile(r"^(\S+)\.(\s+#.*)?$")
_TEST_LINE = re.compile(r"^  (\S+)(\s+#.*)?$")


def parse_listing(text):
    """Full case names ('Suite.Test') from the output of --gtest_list_tests, in listing order."""
    names, suite = [], None
    for line in text.splitlines():
        m = _SUITE_LINE.match(line)
        if m:
            suite = m.group(1)
            continue
        m = _TEST_LINE.match(line)
        if m and suite is not None:
            names.append(suite + "." + m.group(1))
    return names


def clean_env(extra=None):
    """The caller's environment without GoogleTest's sharding and output variables, plus @p extra."""
    env = {k: v for k, v in os.environ.items() if k.upper() not in SHARD_ENV}
    env.update(extra or {})
    return env


def list_cases(exe, gtest_filter, cwd):
    """The cases the filter selects, as the binary lists them."""
    r = subprocess.run([str(exe), "--gtest_list_tests", "--gtest_filter=" + gtest_filter], cwd=cwd,
                       env=clean_env(), capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"--gtest_list_tests exited {r.returncode}: {r.stderr.strip()[:400]}")
    return parse_listing(r.stdout)


# ---------------------------------------------------------------------------------------------
# Results of one process: its XML report, or its console log when the process died first
# ---------------------------------------------------------------------------------------------

def parse_xml(path):
    """(cases, suite_failures) from a GoogleTest XML report.

    cases: {full name: (result, seconds)}; suite_failures: names of failures recorded outside a
    case (a SetUpTestSuite or TearDownTestSuite failure, an environment failure).
    """
    cases, suite_failures = {}, []
    root = ET.parse(path).getroot()
    for tc in root.iter("testcase"):
        suite, name = tc.get("classname", ""), tc.get("name", "")
        failed = tc.find("failure") is not None or tc.find("error") is not None
        if not name:
            if failed:
                suite_failures.append(f"{suite or '(global)'} (outside a case)")
            continue
        if tc.get("status") == "notrun":        # a DISABLED_ case: not selected, not counted
            continue
        if failed:
            result = FAILED
        elif tc.get("result") == "skipped" or tc.find("skipped") is not None:
            result = SKIPPED
        else:
            result = PASSED
        cases[suite + "." + name] = (result, float(tc.get("time", "0") or 0))
    return cases, suite_failures


_LOG_MARK = re.compile(r"^\[ +(RUN|OK|FAILED|SKIPPED) +\] (\S+)")


def parse_log(path):
    """(cases, crashed_in) from a console log, for a process that wrote no XML report.

    crashed_in is the case that started and never ended (empty when none).
    """
    cases, running = {}, ""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = _LOG_MARK.match(line)
            if not m:
                continue
            mark, name = m.group(1), m.group(2)
            if mark == "RUN":
                running = name
            elif name == running:
                cases[name] = ({"OK": PASSED, "FAILED": FAILED, "SKIPPED": SKIPPED}[mark], 0.0)
                running = ""
    if running:
        cases[running] = (FAILED, 0.0)
    return cases, running


# ---------------------------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------------------------

def checkout_of(exe):
    """The checkout a binary at <checkout>/build/<config>/<exe> was built from; else this script's."""
    guess = exe.parent.parent.parent
    return guess if (guess / "CMakeLists.txt").is_file() else ROOT


def start_process(exe, gtest_filter, cwd, log_path, xml_path, env):
    log = open(log_path, "wb")
    args = [str(exe), "--gtest_filter=" + gtest_filter, "--gtest_output=xml:" + str(xml_path)]
    return subprocess.Popen(args, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT), log


def collect(tag, proc_rc, xml_path, log_path, problems):
    """The cases of one finished process; appends to @p problems what makes the run fail."""
    if xml_path.exists():
        try:
            cases, suite_failures = parse_xml(xml_path)
            for f in suite_failures:
                problems.append(f"{tag}: failure {f}")
            if proc_rc != 0 and not any(r == FAILED for r, _ in cases.values()) and not suite_failures:
                problems.append(f"{tag}: exit {proc_rc} with no failure in its report")
            return cases
        except ET.ParseError as e:
            problems.append(f"{tag}: unreadable XML report ({e}); results read from its log")
    cases, crashed_in = parse_log(log_path)
    problems.append(f"{tag}: exit {proc_rc} and no XML report" +
                    (f"; it died in {crashed_in}" if crashed_in else ""))
    return cases


def cmd_run(a):
    exe = Path(a.exe).resolve()
    cwd = Path(a.cwd).resolve() if a.cwd else checkout_of(exe)
    if not exe.is_file():
        print(f"no test binary at {exe}", file=sys.stderr)
        return 2
    if a.shards < 1:
        print("--shards must be at least 1", file=sys.stderr)
        return 2
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = Path(a.out).resolve() if a.out else Path(tempfile.gettempdir()) / f"unit_shards_{stamp}"
    out.mkdir(parents=True, exist_ok=True)
    t_start = time.time()

    expected = list_cases(exe, a.filter, cwd)
    t_listed = time.time()
    tail_patterns = [p for p, _ in SERIAL_TAIL] + list(a.serial or [])
    tail_match = filter_matcher(":".join(tail_patterns)) if tail_patterns else (lambda _n: False)
    tail_cases = [n for n in expected if tail_match(n)]
    shard_filter = add_negatives(a.filter, tail_patterns)

    tmp_root = Path(tempfile.gettempdir()) / f"unit_shards_{os.getpid()}"
    procs = []
    for k in range(a.shards):
        tag = f"shard_{k}"
        if a.shards == 1:
            env = clean_env()
        else:
            tmp = tmp_root / tag
            tmp.mkdir(parents=True, exist_ok=True)
            env = clean_env({"GTEST_TOTAL_SHARDS": str(a.shards), "GTEST_SHARD_INDEX": str(k),
                             "TMP": str(tmp), "TEMP": str(tmp)})
        p, log = start_process(exe, shard_filter, cwd, out / f"{tag}.log", out / f"{tag}.xml", env)
        procs.append((tag, p, log, time.time()))
    shard_seconds, rcs = {}, {}
    pending = list(procs)
    while pending:
        for item in list(pending):
            tag, p, log, t0 = item
            if p.poll() is not None:
                shard_seconds[tag] = time.time() - t0
                rcs[tag] = p.returncode
                log.close()
                pending.remove(item)
        if pending:
            time.sleep(0.5)
    t_shards = time.time()

    if tail_cases:
        tag = "serial_tail"
        p, log = start_process(exe, ":".join(tail_cases), cwd, out / f"{tag}.log", out / f"{tag}.xml",
                               clean_env())
        rcs[tag] = p.wait()
        log.close()
        shard_seconds[tag] = time.time() - t_shards
    t_end = time.time()
    if a.shards > 1 and not a.keep_tmp:
        shutil.rmtree(tmp_root, ignore_errors=True)

    problems, merged, seen = [], {}, {}
    for tag in list(shard_seconds):
        cases = collect(tag, rcs[tag], out / f"{tag}.xml", out / f"{tag}.log", problems)
        for name, (result, secs) in cases.items():
            if name in merged:
                problems.append(f"{name} ran twice ({seen[name]} and {tag})")
            merged[name], seen[name] = (result, secs), tag
    missing = [n for n in expected if n not in merged]
    extra = sorted(set(merged) - set(expected))
    if missing:
        problems.append(f"{len(missing)} selected case(s) did not run, first: {missing[:5]}")
    if extra:
        problems.append(f"{len(extra)} case(s) ran that the listing did not select, first: {extra[:5]}")

    counts = {r: sum(1 for v, _ in merged.values() if v == r) for r in (PASSED, SKIPPED, FAILED)}
    suites = {n.split(".", 1)[0] for n in merged}
    failed = sorted(n for n, (r, _) in merged.items() if r == FAILED)
    skipped = sorted(n for n, (r, _) in merged.items() if r == SKIPPED)
    ok = not failed and not problems and all(rc == 0 for rc in rcs.values())

    shard_tags = [t for t in shard_seconds if t != "serial_tail"]
    lines = [
        f"unit_shards run {stamp}: {'PASS' if ok else 'FAIL'}",
        f"binary {exe}",
        f"working directory {cwd}",
        f"filter {a.filter}",
        f"shards {a.shards}" + (" (unsharded: one process, caller's environment)" if a.shards == 1 else
                                " (GoogleTest sharding, a temp directory per shard)"),
        f"cases {len(merged)} in {len(suites)} suites: {counts[PASSED]} passed, {counts[SKIPPED]} skipped, "
        f"{counts[FAILED]} failed (selected by the listing: {len(expected)})",
        f"wall time {t_end - t_start:.0f} s (listing {t_listed - t_start:.0f} s, shards {t_shards - t_listed:.0f} s"
        + (f", serial tail {t_end - t_shards:.0f} s" if tail_cases else "") + ")",
        "per shard: " + ", ".join(f"{t} {shard_seconds[t]:.0f} s exit {rcs[t]} "
                                  f"({sum(1 for n in merged if seen[n] == t)} cases)" for t in shard_tags),
    ]
    if tail_patterns:
        lines.append(f"serial tail: {len(tail_cases)} case(s), {shard_seconds.get('serial_tail', 0):.0f} s, "
                     f"exit {rcs.get('serial_tail', '-')}")
        for p, why in SERIAL_TAIL:
            lines.append(f"  {p}: {why}")
        for p in a.serial or []:
            lines.append(f"  {p}: named on the command line")
    else:
        lines.append("serial tail: none (no case needs a run of its own)")
    lines += [f"FAILED {n}" for n in failed]
    lines += [f"PROBLEM {p}" for p in problems]
    lines.append(f"results {out}")
    text = "\n".join(lines) + "\n"

    (out / "summary.txt").write_text(text, encoding="utf-8")
    (out / "skipped.txt").write_text("".join(n + "\n" for n in skipped), encoding="utf-8")
    (out / "failed.txt").write_text("".join(n + "\n" for n in failed), encoding="utf-8")
    with open(out / "cases.tsv", "w", encoding="utf-8") as fh:
        fh.write("case\tresult\tseconds\tshard\n")
        for n in sorted(merged):
            fh.write(f"{n}\t{merged[n][0]}\t{merged[n][1]:.3f}\t{seen[n]}\n")
    (out / "summary.json").write_text(json.dumps({
        "ok": ok, "binary": str(exe), "cwd": str(cwd), "filter": a.filter, "shards": a.shards,
        "cases": len(merged), "suites": len(suites), "listed": len(expected), **counts,
        "failed_cases": failed, "problems": problems, "wall_s": round(t_end - t_start, 1),
        "shard_s": {t: round(s, 1) for t, s in shard_seconds.items()}, "exit": rcs,
        "serial_tail": [{"pattern": p, "reason": w} for p, w in SERIAL_TAIL] +
                       [{"pattern": p, "reason": "named on the command line"} for p in a.serial or []],
    }, indent=1), encoding="utf-8")
    sys.stdout.write(text)
    return 0 if ok else 1


# ---------------------------------------------------------------------------------------------
# compare
# ---------------------------------------------------------------------------------------------

def read_cases(folder):
    cases = {}
    with open(Path(folder) / "cases.tsv", encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            name, result = line.rstrip("\n").split("\t")[:2]
            cases[name] = result
    return cases


def cmd_compare(a):
    ca, cb = read_cases(a.a), read_cases(a.b)

    def totals(c):
        return (len(c), len({n.split(".", 1)[0] for n in c}),
                *(sum(1 for r in c.values() if r == x) for x in (PASSED, SKIPPED, FAILED)))

    ta, tb = totals(ca), totals(cb)
    only_a, only_b = sorted(set(ca) - set(cb)), sorted(set(cb) - set(ca))
    differ = sorted(n for n in set(ca) & set(cb) if ca[n] != cb[n])
    fmt = "{} cases in {} suites, {} passed, {} skipped, {} failed"
    print(f"A {a.a}: " + fmt.format(*ta))
    print(f"B {a.b}: " + fmt.format(*tb))
    skip_a = {n for n, r in ca.items() if r == SKIPPED}
    skip_b = {n for n, r in cb.items() if r == SKIPPED}
    print(f"skipped set {'equal' if skip_a == skip_b else 'DIFFERS'} ({len(skip_a)} and {len(skip_b)})")
    for n in only_a[:20]:
        print(f"only in A: {n} ({ca[n]})")
    for n in only_b[:20]:
        print(f"only in B: {n} ({cb[n]})")
    for n in differ[:50]:
        print(f"result differs: {n}: A {ca[n]}, B {cb[n]}")
    same = not only_a and not only_b and not differ
    print("IDENTICAL: every case has the same result" if same else
          f"DIFFERENT: {len(only_a)} only in A, {len(only_b)} only in B, {len(differ)} with another result")
    return 0 if same else 1


# ---------------------------------------------------------------------------------------------
# selftest
# ---------------------------------------------------------------------------------------------

def cmd_selftest(_a):
    m = filter_matcher("A*.x:B.?-A2.*")
    assert m("A1.x") and m("B.y") and not m("A2.x") and not m("B.yy") and not m("C.x")
    assert filter_matcher("*")("Inst/Suite.Test/0")
    assert add_negatives("*", ["X.*"]) == "*-X.*"
    assert add_negatives("A.*-B.*", ["X.*"]) == "A.*-B.*:X.*"
    assert add_negatives("*", []) == "*"
    listing = ("Running main() from gtest_main.cc\nSuiteA.\n  One\n  Two\n"
               "Inst/P.  # TypeParam = int\n  Case/0  # GetParam() = 3\n")
    assert parse_listing(listing) == ["SuiteA.One", "SuiteA.Two", "Inst/P.Case/0"], parse_listing(listing)
    with tempfile.TemporaryDirectory() as d:
        x = Path(d) / "r.xml"
        x.write_text('<?xml version="1.0"?><testsuites tests="4"><testsuite name="S">'
                     '<testcase name="a" status="run" result="completed" time="0.5" classname="S"/>'
                     '<testcase name="b" status="run" result="skipped" time="0" classname="S"><skipped message="m"/></testcase>'
                     '<testcase name="c" status="run" result="completed" time="0" classname="S"><failure message="f"/></testcase>'
                     '<testcase name="DISABLED_d" status="notrun" result="suppressed" time="0" classname="S"/>'
                     '<testcase name="" status="run" result="completed" time="0" classname="S"><failure message="setup"/></testcase>'
                     '</testsuite></testsuites>', encoding="utf-8")
        cases, sf = parse_xml(x)
        assert cases == {"S.a": (PASSED, 0.5), "S.b": (SKIPPED, 0.0), "S.c": (FAILED, 0.0)}, cases
        assert sf == ["S (outside a case)"], sf
        lg = Path(d) / "l.log"
        lg.write_text("[ RUN      ] S.a\nnoise\n[       OK ] S.a (1 ms)\n[ RUN      ] S.b\n"
                      "[  SKIPPED ] S.b (0 ms)\n[ RUN      ] S.c\n[  FAILED  ] S.c (2 ms)\n[ RUN      ] S.d\n",
                      encoding="utf-8")
        cases, crashed = parse_log(lg)
        assert cases == {"S.a": (PASSED, 0.0), "S.b": (SKIPPED, 0.0), "S.c": (FAILED, 0.0),
                         "S.d": (FAILED, 0.0)} and crashed == "S.d", (cases, crashed)
    print("selftest passed")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="run the shards and merge their results")
    r.add_argument("--exe", default=str(DEFAULT_EXE), help="test binary (default: %(default)s)")
    r.add_argument("--shards", type=int, default=6, help="processes at once (default 6; 1 = unsharded)")
    r.add_argument("--filter", default="*", help="GoogleTest filter applied to every shard (default '*')")
    r.add_argument("--out", help="results folder (default <temp>/unit_shards_<time>)")
    r.add_argument("--cwd", help="working directory of every process (default: the checkout of the binary, "
                                 "<checkout>/build/<config>/gnss_tests.exe, as an unsharded run from its root)")
    r.add_argument("--serial", action="append", metavar="PATTERN",
                   help="also run the cases of PATTERN in the serial tail (repeatable)")
    r.add_argument("--keep-tmp", action="store_true", help="keep the per-shard temp directories")
    c = sub.add_parser("compare", help="compare two result folders case by case")
    c.add_argument("a")
    c.add_argument("b")
    sub.add_parser("selftest", help="check the filter, listing, XML and log readers")
    a = ap.parse_args(argv)
    return {"run": cmd_run, "compare": cmd_compare, "selftest": cmd_selftest}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
