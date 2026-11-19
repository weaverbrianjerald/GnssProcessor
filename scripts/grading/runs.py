"""Shared readers of network and user runs for the grading scripts of card NRF (card GRA, plan step G1).

One rule for each thing the bars read, so that no bar carries its own copy of it:

  run_dir(run)         a run named by a bare tag (<RAPID_RUNS_ROOT>/_rap_<tag>), by its "_rap_" folder name, or by a
                       path to an existing folder; RAPID_RUNS_ROOT defaults to D:/GnssProcessorSupport/runs_nom
  tow_key(x)           an epoch time rounded to whole seconds; under processing.exact_epoch_time the stream's and the
                       traces' times are not whole seconds, so every match between files is made on this key
  hour_of(t, t0)       the 1-based hour of an epoch from a run's first epoch
  first_epoch(run)     the key of the first E record of products.txt
  product_epochs(run)  the stream's epochs in file order, each with its records (comment lines and records before the
                       first E record left out)
  rows(run, name)      a trace of the run's logs/ folder, row by row, by its header names
  raw_rows(run, name)  the same trace as lists of fields (for readers that keep a historical positional rule)
  name_list(path)      a comma-separated list of names, such as the combined-mode receivers of a day
  clfrac_lines(run)    a user run's resolver lines (CLFRAC): from the console capture stdout.txt when it holds them,
                       which keeps the whole run; else from logs/ambiguity_resolver*.log, which rotate at 5 MB and keep
                       only the last hours of a 24 h run

Every function only reads.

    py -3.14 scripts/grading/runs.py --selftest
"""
import csv
import glob
import os
import sys

DEFAULT_ROOT = "D:/GnssProcessorSupport/runs_nom"


def runs_root():
    """The folder that holds the runs: RAPID_RUNS_ROOT, else the project's default."""
    return os.environ.get("RAPID_RUNS_ROOT") or DEFAULT_ROOT


def run_dir(run):
    """The folder of a run given as a path, a "_rap_" folder name or a bare tag."""
    if os.path.isdir(run):
        return os.path.abspath(run)
    name = run if run.startswith("_rap_") else "_rap_" + run
    return os.path.join(runs_root(), name)


def tow_key(x):
    """An epoch time (string or number) rounded to whole seconds."""
    return round(float(x))


def hour_of(t, t0):
    """The 1-based hour of epoch key t counted from the run's first epoch key t0."""
    return int((t - t0) // 3600) + 1


def first_epoch(run):
    """The key of the first E record of the run's products.txt, or None when the stream has no epoch."""
    with open(os.path.join(run_dir(run), "products.txt"), encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("E,"):
                return tow_key(line.split(",")[2])
    return None


def product_epochs(run):
    """Yield (epoch key, records) for every E record of products.txt, in file order; each record is its list of
    fields (P, B, C and the other record types; comment lines dropped)."""
    tow, recs = None, []
    with open(os.path.join(run_dir(run), "products.txt"), encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            if line.startswith("E,"):
                if tow is not None:
                    yield tow, recs
                tow, recs = tow_key(line.split(",")[2]), []
            elif tow is not None:
                recs.append(line.rstrip().split(","))
    if tow is not None:
        yield tow, recs


def trace_path(run, name):
    """The path of a trace in the run's logs/ folder."""
    return os.path.join(run_dir(run), "logs", name)


def rows(run, name):
    """Yield the rows of logs/<name> as dicts keyed by the header."""
    with open(trace_path(run, name), newline="", encoding="utf-8", errors="replace") as fh:
        yield from csv.DictReader(fh)


def raw_rows(run, name):
    """(header, iterator of field lists) of logs/<name>; the iterator holds the file open until exhausted."""
    fh = open(trace_path(run, name), newline="", encoding="utf-8", errors="replace")
    rd = csv.reader(fh)
    header = next(rd)

    def it():
        try:
            yield from rd
        finally:
            fh.close()
    return header, it()


def name_list(path):
    """The names of a comma-separated list file (one line, as the grading lists are written)."""
    with open(path, encoding="utf-8") as fh:
        return set(fh.read().strip().split(","))


def clfrac_sources(run):
    """The files that hold a user run's CLFRAC lines: stdout.txt when it has one, else the resolver logs."""
    d = run_dir(run)
    so = os.path.join(d, "stdout.txt")
    if os.path.exists(so) and os.path.getsize(so) > 0:
        with open(so, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "CLFRAC sat_i" in line:
                    return [so]
    return sorted(glob.glob(os.path.join(d, "logs", "ambiguity_resolver*.log")))


def clfrac_lines(run):
    """Yield every CLFRAC line of a user run, from the sources clfrac_sources() names."""
    for p in clfrac_sources(run):
        with open(p, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "CLFRAC sat_i" in line:
                    yield line


def selftest():
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g1_reader.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        want = os.path.join(root, "_rap_fx")
        check(run_dir("fx") == want, "run_dir of a bare tag")
        check(run_dir("_rap_fx") == want, "run_dir of a _rap_ name")
        check(run_dir(want) == os.path.abspath(want), "run_dir of a path")
        check(tow_key("172829.9999996") == 172830, "tow_key rounds a time that is not a whole second")
        check(hour_of(176399, 172800) == 1 and hour_of(176400, 172800) == 2, "hour_of at the hour's edge")
        check(first_epoch("fx") == 172800, "first_epoch of an E record at 172800.0000004")
        eps = list(product_epochs("fx"))
        check([t for t, _ in eps] == [172800, 172830, 172860], "product_epochs keys")
        check([len(r) for _, r in eps] == [3, 2, 1], "product_epochs records per epoch (records before E dropped)")
        check(eps[0][1][0][:3] == ["P", "E04", "L1C"], "a record's fields")
        rf = list(rows("fx", "network_ring_fix.csv"))
        check(len(rf) == 4 and rf[0]["state"] == "held", "rows by header name")
        h, it = raw_rows("fx", "network_events.csv")
        check(h[2] == "tier" and len(list(it)) == 2, "raw_rows header and rows")
        check(name_list(os.path.join(root, "x28.txt")) == {"AAAA00XXX", "BBBB00YYY"}, "name_list")
        check(len(list(clfrac_lines("nw_u_stdout"))) == 2, "CLFRAC lines from stdout.txt")
        src = clfrac_sources("nw_u_logs")
        check(len(src) == 2 and src[0].endswith("ambiguity_resolver.1.log"), "CLFRAC sources fall back to the logs")
        check(len(list(clfrac_lines("nw_u_logs"))) == 3, "CLFRAC lines from the rotated logs")
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print(__doc__)
