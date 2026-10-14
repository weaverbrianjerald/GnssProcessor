#!/usr/bin/env python3
"""Gates G-TD and G-EP: two runs of one arm print and write the same run end (G-TD, card NOM2
section 6), or the same whole run (G-EP, card NOM3 section 5, `--whole`).

    python scripts/nom_td_gate.py <run A> <run B> [--whole] [--mask-time] [--show N]
                                  [--no-arm-check]

A run is a directory, or a name under runs/ (`_rap_<tag>` or `<tag>` as rapid_arm.py names
them).  The two runs are one arm on two executables built with the same Release flags (the
reference and the candidate), or on one executable twice (the self-identity check of the
masks).  Exit status 0 when every part below is IDENTICAL.

One script owns both gates.  G-EP widens two parts of G-TD (part 1 to every CSV in the run
directory, part 3 from the run-end block to the whole logs, since the steps of card NOM3 move
per-epoch and startup lines); parts 0, 2 and 4 and every mask are shared, so a mask proven on
one gate serves the other and lives in one table.

What is compared, in order:
  0. The two arm configs (arm.json), equal once the run directory name is masked (rapid_arm
     writes it into product_output_path), so the runs differ by the executable alone.
     --no-arm-check skips this part, for a negative control between two different arms.
  1. Products and traces: scripts/prm_ab.py's comparison (compare_dirs with --all-csv):
     products.txt without its '#' lines, closed_loop_trace.csv, network_epoch_status.csv
     and every other logs/*.csv both runs wrote, with prm_ab's wall-clock columns
     (VOLATILE_COLS) dropped.  Every CSV is compared in file ROW ORDER: prm_ab.lines reads a
     file in order and compare_dirs compares the two lists, nothing is sorted.  So
     network_events.csv, which the phase-datum election writes interleaved with other seams'
     events in one epoch, is compared in row order, and so is every other CSV several seams
     write in one epoch; no CSV needs an override.  With --whole, also every CSV elsewhere in
     the run directory, which prm_ab's logs/*.csv glob does not reach (arm cil writes
     logs/rhs_attrib/{attrib,rows,vec}_<tow>.csv, network.rhs_attrib_on_trip), in full and in
     row order.  Then products.txt in full, '#' and 'S,' lines included (the serialized
     contract the B6 round trip reads back).  A run that emits no epoch writes no
     products.txt; absent from both runs is consistent, from one is a difference.
  2. The run manifest, logs/network_run_manifest.json, record by record and in order, as
     printed (key order and number formatting included), with the values of `written_utc`,
     `build.compiled` and `loop_ms` masked, the run directory name replaced by <RUN>, and
     `config_hash_fnv1a64` masked (the hash of arm.json, which names the run directory;
     part 0 compares the configs themselves); the last record must be `complete` in both.
  3. G-TD: the run-end log block, from the first line containing
     'Network[timing]: epoch-loop wall' to the end of the file, in stdout.txt and in the
     concatenated main_log (logs/main_log.N.log ... main_log.1.log, main_log.log, oldest
     first, since spdlog rotates at 5 MB), line by line and in order, with the
     `[time] [logger] [level]` prefix kept.
     G-EP: the whole stdout.txt and the whole concatenated main_log, likewise.  stdout.txt
     carries the console sink of every logger (main_log, gnss_config, rt_engine,
     gnss_processor), so it also holds every line of logs/gnss_config.log and
     logs/rt_engine.log.  spdlog keeps three rotated files (LogManager.cpp); with
     main_log.3.log present in either run the head may have been rotated out, at a byte
     offset that differs between the runs (wall-clock values print with different widths), so
     the main_log comparison then covers the common tail and says so; stdout.txt is whole.
  4. logs/rt_engine.log in full (the closed-loop consumer's log; the three consumer
     summaries the run end prints are its last lines, so the full file contains the run-end
     tail); and the first token of exitcode.txt (the second is wall seconds).

Every log goes through one pipeline (mask_lines): whole-line drops, family value masks,
value masks, the run directory name, --mask-time, then the parallel families.  The masks,
and why each exists:

From G-TD (measured on same-build pairs, card NOM2 section 6):
  - Values: `-?<number> ms` becomes `<MS> ms` (and `<number>ms` becomes `<MS>ms`), a pair
    `<number> / <number> ms` becomes `<MS> / <MS> ms`, and `in <number>.<number> s` becomes
    `in <T> s`.  Wall-clock durations sit inside otherwise deterministic counter lines (the
    datum graph, prior-residual, redundancy, MDB, block-Baarda, congruence, roster and
    ledger-emit cost figures; card AGT's cycle-basis cost clause, which prints p50 and max
    as one `p50 / max` pair; the B6 round trip's "in {:.2f} s").  The consumer's
    `is <number>s behind wall-clock GPS` lag is the epoch's age against the wall clock at
    the moment it ran (rt_engine.log).
  - Whole lines dropped, because their presence, their order or every number in them is
    wall clock: `Network[timing]`; `Network (D0)` and `Network (D0b)` (the D0b rows are
    ranked by wall time with an unstable sort, so two 0 ms rows can swap); the D0 per-op
    lines `Network (D0/INC-C0) carry-driver per-op` and `Network (D0/LF-1) lane-fold
    per-op`; `Network (datum graph): worst census pass` and `Network (FC-1): slowest tick`
    (printed or not depending on wall time); `Network (FC-1)   tick wall`.  The
    deterministic D0 lines (the INC-C0 carry census, the K_c histogram, the D-c viability
    screen) are kept.
  - The run directory name becomes <RUN> (it is printed on failure branches and in the
    manifest's paths).
  - The time inside the prefix is the logger's GNSS epoch stamp, which is deterministic,
    so it is compared as printed.  --mask-time replaces it with <TIME> for a log whose stamp
    is wall clock.

Added for G-EP, each found by comparing the whole stdout.txt and main_log of the
same-executable pair _rap_td1b_same_f2 / _rap_td1b_new_f2 (both gnss_processor_td1b.exe) and
of the 18 pairs _rap_td1b_ref_<x> / _rap_td1b_new_<x> that G-TD certified (card NOM2 section
8).  Every family below prints before the run-end block, so none changes what G-TD compares.
  - The consumer's lag mask applies to every log, not to rt_engine.log alone: the rt_engine
    logger's console sink prints the same `[RT] obs backlog: ... is <number>s behind
    wall-clock GPS` line into stdout.txt.  Evidence: it differs in stdout.txt on 17 of the
    19 pairs, the same-executable pair and 16 of the 18 (zero and cil print none); main_log
    does not carry the rt_engine logger.
  - Parallel families.  The canonical-signal decision lines of PppModel.cpp (`Canonical
    signals decided for`, `Canonical latch for`, and `Canonical latch FOLLOWS` from the same
    block, not printed on these arms) are written per station from PASS B's produce loop
    (`omp parallel for schedule(dynamic)`), so their order is the thread schedule, and their
    stamp is the epoch on the master thread and the placeholder on a worker (the epoch is
    thread-local, LogManager::setCurrentEpochTime).  Each maximal run of consecutive lines of
    these families has its stamp replaced by [<TIME>] and is sorted: the run is compared as a
    multiset at its place in the stream, with every station's line, counts and signals still
    compared.  Evidence: under G-TD's masks the whole stdout.txt / main_log of the
    same-executable pair differ at 104 / 97 positions, every one of them in these lines or
    the lag above; these lines differ in order and in stamp on 18 of the 19 pairs, in both
    logs (arm zero prints none).  A line of another family printed in the same parallel
    region splits a run and reads DIFFERENT, the safe direction; such a family is added to
    PARALLEL_FAMILIES with its evidence.
  - The per-epoch section-timer lines `Network (D0) TOW <tow>: ...` and `Network (D0b) TOW
    <tow>: ...` (one of each per epoch while section timers are armed, 240 of each on arm
    f2) are masked rather than dropped: each duration becomes <MS> (D0: total,
    assembly-wall, reduce, solve, cov-inv, cov-consumers, other, est.addStation; D0b: every
    block's `=<number>`, Σnet, other, unattributed), and the TOW, the cov-consumer call
    count, the shared-column count and the station count stay compared.  The startup line
    `Network (D0): per-epoch section timers ARMED` is deterministic and is kept.  G-TD's
    drops of `Network (D0)` and `Network (D0b)` are narrowed to exclude these three lines,
    which can only reduce what is hidden.  Evidence: the durations differ in both logs on
    all 18 pairs that print these lines (arm zero prints none); with them masked, the kept
    fields agree on all 18, and the ARMED line agrees on all 19.
  - Reached by G-TD's own value masks on new families, with no new mask: the `<number> ms`
    mask on the per-epoch DD-frac tracer (`Network (T2b) DD-frac tracer TOW <tow>: pool <n>
    ms (<n> sta), collect <n> ms (<n> DD), cov <n> ms (<n> reqs), emit <n> ms`; the
    durations differ on all 18 pairs that print it, the counts stay compared), and the
    `in <number>.<number> s` mask on the startup `RINEX ingestion: parsed <n> stations in
    <s> s (parallel)` (differs on all 19).  On these arms the `<number> ms` mask touches one
    deterministic line besides, the prose `a 1 ms step` of the measurement-admission banner
    (a literal of the format string, not a value).
The masks are checked by mutation as well as by agreement: on a copy of one run, a changed
shared-column count in one D0 line, a renamed D0b block, a changed count in one canonical
latch line, a changed startup banner value, a changed DD-frac tracer count, a swap of two
network_events.csv rows and a CSV under logs/rhs_attrib/ in one run only each read
DIFFERENT under --whole, while a swap of two lines inside one canonical run and a changed
backlog lag read IDENTICAL.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE).replace(os.sep, '/')
RUNS = ROOT + '/runs'
# A second place to look for a run named without a path: rapid_arm.py's RAPID_RUNS_ROOT.
RUNS_ALT = (os.environ.get('RAPID_RUNS_ROOT') or '').replace(os.sep, '/').rstrip('/')
sys.path.insert(0, HERE)
import prm_ab  # noqa: E402  (the products and traces comparison)

START = 'Network[timing]: epoch-loop wall'
# spdlog's rotating sink for every module log keeps this many rotated files (LogManager.cpp).
MAIN_LOG_ROTATED_KEEP = 3

# Whole lines dropped (their presence, order or every number is wall clock).  The D0 and D0b
# drops exclude the per-epoch section-timer lines, which FAMILY_MASKS masks instead, and the
# deterministic startup line that arms the timers.
LINE_DROPS = [re.compile(p) for p in (
    r'Network\[timing\]',
    r'Network \(D0\)(?! TOW \d|: per-epoch section timers ARMED)',
    r'Network \(D0b\)(?! TOW \d)',
    r'Network \(D0/INC-C0\) carry-driver per-op',
    r'Network \(D0/LF-1\) lane-fold per-op',
    r'Network \(datum graph\): worst census pass',
    r'Network \(FC-1\)   tick wall',
    r'Network \(FC-1\): slowest tick',
)]
_NUM = r'-?\d+(?:\.\d+)?'
# Value masks that apply only on the lines of one family: (family, [(value, replacement)]).
FAMILY_MASKS = [
    # Network (D0) TOW <tow>: total <ms> ms | assembly-wall <ms> | reduce <ms> | solve <ms> |
    # cov-inv <ms> | cov-consumers <ms> (<n> calls) | other <ms> | est.addStation <ms>
    # (cross-cutting) | <n> shared cols, <n> stations
    (re.compile(r'\] Network \(D0\) TOW \d+(?:\.\d+)?: total '),
     [(re.compile(r'\b(total|assembly-wall|reduce|solve|cov-inv|cov-consumers|other'
                  r'|est\.addStation) ' + _NUM), r'\1 <MS>')]),
    # Network (D0b) TOW <tow>: other-net ms [ <block>=<ms> ... ] Σnet <ms> | other <ms> |
    # unattributed <ms>
    (re.compile(r'\] Network \(D0b\) TOW \d+(?:\.\d+)?: other-net ms \['),
     [(re.compile(r'=' + _NUM), '=<MS>'),
      (re.compile(r'(Σnet|\bother|\bunattributed) ' + _NUM), r'\1 <MS>')]),
]
VALUE_MASKS = [
    # A `p50 / max` pair (card AGT's cost clauses): the first figure carries no unit of its
    # own, so the single-figure mask below would leave it.  Applied first.
    (re.compile(r'-?\d+(?:\.\d+)?(\s*/\s*)-?\d+(?:\.\d+)?(\s*)ms\b'), r'<MS>\1<MS>\2ms'),
    (re.compile(r'-?\d+(?:\.\d+)?(\s*)ms\b'), r'<MS>\1ms'),
    (re.compile(r'\bin \d+\.\d+ s\b'), 'in <T> s'),
    # The consumer's lag against the wall clock (rt_engine.log, and stdout.txt through the
    # rt_engine logger's console sink).
    (re.compile(r'is -?\d+(?:\.\d+)?s behind wall-clock GPS'), 'is <S>s behind wall-clock GPS'),
]
# Lines written from inside a parallel loop: their order is the thread schedule and their
# epoch stamp names the thread (see the docstring).  Each maximal run of consecutive lines of
# these families is compared as a multiset, stamp masked.
PARALLEL_FAMILIES = [
    re.compile(r'^\[[^\]]*\] \[main_log\] \[info\] Canonical '
               r'(?:signals decided for|latch for|latch FOLLOWS) '),
]
PREFIX_TIME = re.compile(r'^\[[^\]]*\](?= \[)')
MANIFEST_MASKS = [
    (re.compile(r'"written_utc"\s*:\s*"[^"]*"'), '"written_utc":"<UTC>"'),
    (re.compile(r'"compiled"\s*:\s*"[^"]*"'), '"compiled":"<COMPILED>"'),
    (re.compile(r'"loop_ms"\s*:\s*[-+0-9.eE]+'), '"loop_ms":"<MS>"'),
    (re.compile(r'"config_hash_fnv1a64"\s*:\s*"[^"]*"'), '"config_hash_fnv1a64":"<HASH>"'),
]


def resolve(run):
    roots = [RUNS] + ([RUNS_ALT] if RUNS_ALT else [])
    for cand in [run] + [c for r in roots for c in (f'{r}/{run}', f'{r}/_rap_{run}')]:
        if os.path.isdir(cand):
            return cand.replace(os.sep, '/')
    sys.exit(f'no run directory for {run}')


def read_lines(path):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        text = f.read().replace('\r\n', '\n').replace('\r', '\n')
    lines = text.split('\n')
    while lines and lines[-1] == '':
        lines.pop()
    return lines


def main_log_files(run):
    """The main_log rotation set, oldest first: main_log.N.log ... main_log.1.log, main_log.log."""
    rotated = sorted(glob.glob(f'{run}/logs/main_log.*.log'),
                     key=lambda p: int(re.search(r'main_log\.(\d+)\.log$', p).group(1)),
                     reverse=True)
    return [p for p in rotated + [f'{run}/logs/main_log.log'] if os.path.exists(p)]


def main_log_lines(run):
    out = []
    for p in main_log_files(run):
        out += read_lines(p)
    return out


def main_log_rotation_full(run):
    """Whether spdlog kept its last rotated file, so the head may have been rotated out."""
    return os.path.exists(f'{run}/logs/main_log.{MAIN_LOG_ROTATED_KEEP}.log')


def from_start(lines):
    i = next((k for k, l in enumerate(lines) if START in l), None)
    return [] if i is None else lines[i:]


def sort_parallel_runs(lines):
    """Each maximal run of consecutive PARALLEL_FAMILIES lines, stamp masked and sorted."""
    out, run = [], []
    for l in lines + [None]:
        if l is not None and any(p.search(l) for p in PARALLEL_FAMILIES):
            run.append(PREFIX_TIME.sub('[<TIME>]', l, count=1))
            continue
        out += sorted(run)
        run = []
        if l is not None:
            out.append(l)
    return out


def mask_lines(lines, run_name, mask_time):
    out = []
    for l in lines:
        if any(d.search(l) for d in LINE_DROPS):
            continue
        for family, masks in FAMILY_MASKS:
            if family.search(l):
                for pat, rep in masks:
                    l = pat.sub(rep, l)
        for pat, rep in VALUE_MASKS:
            l = pat.sub(rep, l)
        l = l.replace(run_name, '<RUN>')
        if mask_time:
            l = PREFIX_TIME.sub('[<TIME>]', l, count=1)
        out.append(l)
    return sort_parallel_runs(out)


def first_differences(a, b):
    """Indices of the differing lines (positional; a length difference counts from the end
    of the shorter list)."""
    diffs = [k for k in range(min(len(a), len(b))) if a[k] != b[k]]
    diffs += list(range(min(len(a), len(b)), max(len(a), len(b))))
    return diffs


def report(name, a, b, show):
    diffs = first_differences(a, b)
    status = 'IDENTICAL' if not diffs else f'DIFFER ({len(diffs)} line(s))'
    print(f'  {name:44s} {len(a):6d} / {len(b):6d} lines  {status}')
    for k in diffs[:show]:
        x = a[k] if k < len(a) else '<eof>'
        y = b[k] if k < len(b) else '<eof>'
        print(f'     line {k + 1}\n       A: {x[:220]}\n       B: {y[:220]}')
    return not diffs


def manifest_records(run):
    """The manifest's records as printed (JSONL), masked textually so that number formatting
    and key order are still compared: the values of `written_utc`, `build.compiled` (the only
    `compiled` key) and `loop_ms` are replaced, and the run directory name becomes <RUN>."""
    path = f'{run}/logs/network_run_manifest.json'
    if not os.path.exists(path):
        return None
    name = os.path.basename(run)
    recs = []
    for l in read_lines(path):
        for pat, rep in MANIFEST_MASKS:
            l = pat.sub(rep, l)
        recs.append(l.replace(name, '<RUN>'))
    return recs


def record_kind(rec_line):
    return json.loads(rec_line).get('record')


def other_csvs(run):
    """Every CSV in the run directory that prm_ab's logs/*.csv glob does not reach."""
    top = {os.path.normpath(p) for p in glob.glob(f'{run}/logs/*.csv')}
    return {os.path.relpath(p, run).replace(os.sep, '/')
            for p in glob.glob(f'{run}/**/*.csv', recursive=True)
            if os.path.normpath(p) not in top}


def compare_other_csvs(da, db, show):
    ca, cb = other_csvs(da), other_csvs(db)
    ok = True
    for rel in sorted(ca ^ cb):
        print(f'  {rel}: present in one run only')
        ok = False
    for rel in sorted(ca & cb):
        ok = report(rel, read_lines(f'{da}/{rel}'), read_lines(f'{db}/{rel}'), show) and ok
    if not (ca | cb):
        print('  CSVs outside logs/*.csv                    none in either run')
    return ok


def compare_main_log(da, db, na, nb, a, whole):
    la, lb = main_log_lines(da), main_log_lines(db)
    if not whole:
        la, lb = from_start(la), from_start(lb)
    la, lb = mask_lines(la, na, a.mask_time), mask_lines(lb, nb, a.mask_time)
    name = 'main_log whole (rotation set)' if whole else 'main_log block (rotation set)'
    if not la or not lb:
        print('  main_log: no', 'lines' if whole else 'block start', 'in', 'A' if not la else 'B')
        return report(name, la, lb, a.show) and False
    if whole and (main_log_rotation_full(da) or main_log_rotation_full(db)):
        n = min(len(la), len(lb))
        print(f'  main_log: main_log.{MAIN_LOG_ROTATED_KEEP}.log present, the head may have been '
              f'rotated out; the common tail of {n} lines is compared (stdout.txt is whole)')
        la, lb, name = la[-n:], lb[-n:], 'main_log common tail (rotation set)'
    return report(name, la, lb, a.show)


def main():
    # The log lines hold non-ASCII text (sigma, arrows); a differing line must print on a
    # cp1252 console instead of ending the gate with an encoding error.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run_a')
    ap.add_argument('run_b')
    ap.add_argument('--whole', action='store_true',
                    help='gate G-EP: the whole logs and every CSV, not the run end alone')
    ap.add_argument('--mask-time', action='store_true',
                    help='mask the time inside the [time] [logger] [level] prefix')
    ap.add_argument('--no-arm-check', action='store_true',
                    help='skip part 0 (a negative control between two different arms)')
    ap.add_argument('--show', type=int, default=8, help='differing lines shown per part')
    a = ap.parse_args()
    da, db = resolve(a.run_a), resolve(a.run_b)
    na, nb = os.path.basename(da), os.path.basename(db)
    ok = True

    print(f'{"G-EP" if a.whole else "G-TD"}: {da}  vs  {db}')
    print('(0) arm configs')
    if a.no_arm_check:
        print('  arm.json                                   SKIPPED (--no-arm-check)')
    else:
        ca = [l.replace(na, '<RUN>') for l in read_lines(f'{da}/arm.json')]
        cb = [l.replace(nb, '<RUN>') for l in read_lines(f'{db}/arm.json')]
        ok = report('arm.json (run directory masked)', ca, cb, a.show) and ok
    print('(1) products and traces (prm_ab.compare_dirs --all-csv; every CSV in row order)')
    ok = prm_ab.compare_dirs(da, db, True, 'products+traces', absent_ok=True) and ok
    if a.whole:
        print('  -- CSVs outside logs/*.csv, in full and in row order')
        ok = compare_other_csvs(da, db, a.show) and ok
    has_a, has_b = os.path.exists(f'{da}/products.txt'), os.path.exists(f'{db}/products.txt')
    if has_a and has_b:
        pa, pb = read_lines(f'{da}/products.txt'), read_lines(f'{db}/products.txt')
        ok = report('products.txt in full', pa, pb, a.show) and ok
    elif has_a or has_b:
        print('  products.txt written by one run only')
        ok = False
    else:
        print('  products.txt in full                       absent in both runs (no epoch emitted)')

    print('(2) run manifest, record by record')
    ma, mb = manifest_records(da), manifest_records(db)
    if ma is None or mb is None:
        print('  network_run_manifest.json missing in', 'A' if ma is None else 'B')
        ok = ok and ma is None and mb is None
    else:
        ok = report('network_run_manifest.json (masked)', ma, mb, a.show) and ok
        for name, recs in (('A', ma), ('B', mb)):
            last = record_kind(recs[-1]) if recs else None
            if last != 'complete':
                print(f'  {name}: last manifest record is {last!r}, not complete')
                ok = False
        print(f'  records: {", ".join(str(record_kind(r)) for r in ma)}')

    if a.whole:
        print('(3) whole logs: stdout.txt and the main_log rotation set')
        sa = mask_lines(read_lines(f'{da}/stdout.txt'), na, a.mask_time)
        sb = mask_lines(read_lines(f'{db}/stdout.txt'), nb, a.mask_time)
        if not sa or not sb:
            print('  stdout.txt: no lines in', 'A' if not sa else 'B')
            ok = False
        ok = report('stdout.txt whole', sa, sb, a.show) and ok
    else:
        print("(3) run-end log block from '" + START + "'")
        sa = mask_lines(from_start(read_lines(f'{da}/stdout.txt')), na, a.mask_time)
        sb = mask_lines(from_start(read_lines(f'{db}/stdout.txt')), nb, a.mask_time)
        if not sa or not sb:
            print('  stdout.txt: no block start in', 'A' if not sa else 'B')
            ok = False
        ok = report('stdout.txt block', sa, sb, a.show) and ok
    ok = compare_main_log(da, db, na, nb, a, a.whole) and ok

    print('(4) rt_engine.log and the exit code')
    ra = f'{da}/logs/rt_engine.log'
    rb = f'{db}/logs/rt_engine.log'
    if os.path.exists(ra) and os.path.exists(rb):
        ok = report('rt_engine.log', mask_lines(read_lines(ra), na, a.mask_time),
                    mask_lines(read_lines(rb), nb, a.mask_time), a.show) and ok
    elif os.path.exists(ra) != os.path.exists(rb):
        print('  rt_engine.log present in one run only')
        ok = False
    ea = open(f'{da}/exitcode.txt').read().split()[:1]
    eb = open(f'{db}/exitcode.txt').read().split()[:1]
    print(f'  exit code: {ea} / {eb}  {"IDENTICAL" if ea == eb else "DIFFER"}')
    ok = ok and ea == eb

    print('RESULT', 'IDENTICAL' if ok else 'DIFFERENT')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
