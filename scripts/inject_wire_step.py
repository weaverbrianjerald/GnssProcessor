#!/usr/bin/env python3
"""Copy a network product file with an undeclared step on the records of a list of satellites.

    python scripts/inject_wire_step.py <in.txt> <out.txt> --class pb|clk --sats G13,E04[,...]
           --step-m 10 --tow-from 270000
    python scripts/inject_wire_step.py --selftest

Two classes of fault, both persistent from the first epoch at or after --tow-from:

  pb    the step is added to the bias of every P (phase bias) record of each listed satellite, all
        signals, so the satellite's whole phase family moves together.  The trailing fields of the
        record (the optional discontinuity counter, sigma=, sigc= and the hold generation gen=) are
        left as they are.
  clk   the step is added to the clock c0 of the C record of each listed satellite.  The record's
        other fields (the drift, the discontinuity counter, iode=, lvl=) are left as they are.

Nothing declares the step: the wire carries no counter change and no R record for it.  Every other
line is copied byte for byte (line endings included).

The epoch time of an E record is compared with a half-second tolerance, because the producer writes
it as a floating-point sum that can fall just below the exact second (262799.99998664856 stands for
262800): a bare comparison starts the step one epoch late.

A satellite with no record of the class at or after --tow-from is an error, so a run never reads a
fault that was not written.

What a replaying reader makes of the step is its own business: it recomputes the phase-bias
discontinuity counter from the values (a step above the threshold increments it), whereas the clock
counter has no value test.  The tools that grade a replay read the consumer's reaction, not the file.
"""
import argparse
import collections
import io
import sys

TOL_S = 0.5          # half the shortest epoch interval the wire is written at (30 s); the replay's sync tolerance
CLASSES = ("pb", "clk")


def inject(fin, fout, cls, sats, step_m, tow_from):
    """Copy the lines of `fin` to `fout` with the step added.  Returns (records stepped per satellite,
    epochs at or after tow_from).  `fin` and `fout` are text streams opened with newline=''."""
    if cls not in CLASSES:
        raise ValueError("class must be one of %s, not %r" % (", ".join(CLASSES), cls))
    want = set(sats)
    tag = "P," if cls == "pb" else "C,"
    col = 3 if cls == "pb" else 2          # P,<sat>,<signal>,<bias_m>,...   C,<sat>,<clock_c0_m>,...
    tow = None
    n_epochs = 0
    stepped = collections.Counter()
    for line in fin:
        if line.startswith("E,"):
            tow = float(line.split(",")[2])
            if tow + TOL_S >= tow_from:
                n_epochs += 1
        elif tow is not None and tow + TOL_S >= tow_from and line.startswith(tag):
            body = line.rstrip("\r\n")
            parts = body.split(",")
            if parts[1] in want:
                eol = line[len(body):]
                parts[col] = repr(float(parts[col]) + step_m)
                stepped[parts[1]] += 1
                line = ",".join(parts) + eol
        fout.write(line)
    missing = sorted(want - set(stepped))
    if missing:
        raise ValueError("no %s record at or after TOW %s for %s" % (tag[0], tow_from, ", ".join(missing)))
    return dict(stepped), n_epochs


def inject_file(src, dst, cls, sats, step_m, tow_from):
    """inject() between two files; the bytes are copied through latin-1, so nothing is re-encoded."""
    with open(src, "r", encoding="latin-1", newline="") as fin, \
         open(dst, "w", encoding="latin-1", newline="") as fout:
        return inject(fin, fout, cls, sats, step_m, tow_from)


def selftest():
    wire = (
        "# gnss_network_product v2\n"
        "S,MBAR00UGA,1,nominal-wu93,0,0,1,0,0.05,1,APRIORI,G:G01,cbjump=1\n"
        "H,2313,262770,ok,2.6686,3,-\n"
        "E,2313,262769.9999883175,0,0,0,0\n"
        "C,G13,0.5\n"
        "B,G13,C1W,-1.0\n"
        "P,G13,L1W,16.0,1,0,gen=1\n"
        "P,G13,L2W,20.0,1,0,gen=1\n"
        "C,E04,0.25,0.001,3,iode=7\n"
        "P,E04,L1X,30.0,1,0,2,sigma=0.01,gen=1\n"
        "E,2313,262799.99998664856,0,0,0,0\r\n"
        "C,G13,0.75\r\n"
        "P,G13,L1W,16.5,1,0,gen=1\r\n"
        "P,G13,L2W,20.5,1,0,gen=1\r\n"
        "C,E04,0.5,0.002,3,iode=7\r\n"
        "P,E04,L1X,30.5,1,0,2,sigma=0.01,gen=1\r\n"
        "U,E04,0.8\r\n"
        "E,2313,262829.9999847412,0,0,0,0\n"
        "C,G13,1.25\n"
        "P,G13,L1W,17.0,1,0,gen=1\n"
        "P,E04,L1X,31.0,1,0,2,gen=1\n"
    )

    def run(cls, sats, step, tow_from):
        out = io.StringIO(newline="")
        counts, n_ep = inject(io.StringIO(wire, newline=""), out, cls, sats, step, tow_from)
        return out.getvalue(), counts, n_ep

    a, b = wire.split("\n"), None
    # pb: G13 from 262800 (the E record sits 1.3e-5 s below the second): both signals of both epochs after it
    text, counts, n_ep = run("pb", ["G13"], 10.0, 262800.0)
    assert counts == {"G13": 3}, counts
    assert n_ep == 2, n_ep
    got = text.split("\n")
    want = wire.split("\n")
    changed = [i for i, (x, y) in enumerate(zip(got, want)) if x != y]
    assert len(got) == len(want)
    assert [want[i] for i in changed] == [
        "P,G13,L1W,16.5,1,0,gen=1\r", "P,G13,L2W,20.5,1,0,gen=1\r", "P,G13,L1W,17.0,1,0,gen=1"], changed
    assert got[changed[0]] == "P,G13,L1W,26.5,1,0,gen=1\r"          # CRLF kept, gen= kept
    assert got[changed[1]] == "P,G13,L2W,30.5,1,0,gen=1\r"
    assert got[changed[2]] == "P,G13,L1W,27.0,1,0,gen=1"
    # the clock records, the code biases and the other satellite are untouched
    assert all(not want[i].startswith(("C,", "B,", "U,", "H,", "S,", "#")) for i in changed)
    # pb on E04 keeps its trailing fields (counter, sigma=, gen=)
    text, counts, _ = run("pb", ["E04"], 2.0, 262800.0)
    assert counts == {"E04": 2}, counts
    assert "P,E04,L1X,32.5,1,0,2,sigma=0.01,gen=1\r" in text
    assert "P,E04,L1X,33.0,1,0,2,gen=1\n" in text
    # clk: only the C record of the listed satellite, from the same epoch, drift and counter kept
    text, counts, _ = run("clk", ["E04", "G13"], 92.409, 262800.0)
    assert counts == {"E04": 1, "G13": 2}, counts
    assert "C,E04,%r,0.002,3,iode=7\r\n" % (0.5 + 92.409) in text
    assert "C,G13,%r\r\n" % (0.75 + 92.409) in text
    assert "C,G13,%r\n" % (1.25 + 92.409) in text
    assert "C,G13,0.5\n" in text and "P,G13,L1W,16.5,1,0,gen=1\r\n" in text          # earlier epoch, other class
    # from the very first epoch every epoch counts
    _, counts, n_ep = run("pb", ["G13"], 0.0, 0.0)
    assert counts == {"G13": 5} and n_ep == 3, (counts, n_ep)
    # a satellite without a record, and a bad class, are errors
    for bad in (lambda: run("pb", ["G99"], 1.0, 0.0), lambda: run("clk", ["G13"], 1.0, 999999.0),
                lambda: run("bias", ["G13"], 1.0, 0.0)):
        try:
            bad()
        except ValueError:
            continue
        raise AssertionError("expected ValueError")
    # step 0 reproduces the input byte for byte
    text, _, _ = run("pb", ["G13", "E04"], 0.0, 0.0)
    assert text.replace("26.5", "") != "" and text == wire.replace("16.0,1,0,gen=1\n", "16.0,1,0,gen=1\n"), "step 0 changed bytes"
    print("inject_wire_step selftest: ok")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--class", dest="cls", choices=CLASSES)
    ap.add_argument("--sats", help="comma list of satellites, e.g. G13,E04")
    ap.add_argument("--step-m", type=float)
    ap.add_argument("--tow-from", type=float)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        selftest()
        return 0
    if None in (a.src, a.dst, a.cls, a.sats, a.step_m, a.tow_from):
        ap.error("src, dst, --class, --sats, --step-m and --tow-from are required")
    counts, n_ep = inject_file(a.src, a.dst, a.cls, [s for s in a.sats.split(",") if s], a.step_m, a.tow_from)
    print("%s records stepped by %s m from TOW %s (%d epochs): %s"
          % ("P" if a.cls == "pb" else "C", a.step_m, a.tow_from, n_ep,
             ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
