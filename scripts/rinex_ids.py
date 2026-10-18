#!/usr/bin/env python3
"""Satellite identifiers of RINEX 3 records, as the scripts must read them.

    python scripts/rinex_ids.py lint       list the readers that still slice a record's first three bytes
    python scripts/rinex_ids.py selftest

A RINEX 3 observation or navigation record starts with the satellite identifier in three
bytes (system letter, two-digit number).  Some writers pad a number below ten with a blank
instead of a zero: ``G 7`` for ``G07``.  16 of the 310 observation files of 2024-129 do.  The
C++ readers (src/core/rinex/RinexParser.cpp, RinexNavParser.cpp) turn the blank into a zero.
A script that keys on the raw three bytes treats ``G 7`` as a satellite that no product, no
selector and no other file knows, and drops it without a word.

The rule here is the readers' rule: a blank second byte becomes ``0``.  A script compares and
keys on ``norm_sat(line[0:3])``.  The record text itself is never rewritten: a script that
writes a file back writes the original bytes, so an edited file stays one change away from the
input.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")

# The scripts that read RINEX 3 records by id and normalize it through this module.  The
# selftest lints these; `lint` with no argument covers every script (slow on a cold disk).
READERS = ("ab2_l5_raw_read.py", "ewl_bridge_oracle.py", "ewl_census.py", "extcb_gim_verdict.py",
           "extcb_raw_if_check.py", "fleet_bias_diurnal.py", "mw_census.py", "mw_census_allpairs.py",
           "mwwl_event_census.py", "rinex_edit.py", "simnet_mw_widelane.py",
           "clock_level/scl2_codepred.py", "clock_level/scl_brdc_clk.py")


def norm_sat(sat):
    """The canonical identifier of the three bytes `sat` of a RINEX 3 record: a blank in the
    number's tens place becomes a zero (``G 7`` -> ``G07``).  Anything else is returned as it
    came, so a canonical id, a short string and a RINEX 2 style id pass through."""
    return sat[:1] + "0" + sat[2:3] if len(sat) >= 3 and sat[1] == " " else sat


def record_sat(line):
    """The canonical satellite identifier at the start of a RINEX 3 record line."""
    return norm_sat(line[0:3])


# A slice of the first three bytes of a record under the names the readers give it, outside
# a norm_sat( ... ) call.  `p[:3]` style slices of other things are not matched on purpose.
_RAW = re.compile(r"(?<!norm_sat\()(?<!record_sat\()\b(?:line|ln|sl)\[(?:0)?:3\]")
# A script reads RINEX text when it names one of these header labels or the epoch marker.
_READS_RINEX = re.compile(r"END OF HEADER|OBS TYPES|startswith\(\s*['\"]>")


def lint(paths=None):
    """The (path, line number, text) of every raw first-three-bytes slice in a script that
    reads RINEX text.  Without `paths`, every script under scripts/ and scripts/clock_level/."""
    if paths is None:
        paths = sorted(glob.glob(HERE + "/*.py") + glob.glob(HERE + "/clock_level/*.py"))
    hits = []
    for path in paths:
        if os.path.abspath(path).replace("\\", "/") == HERE + "/rinex_ids.py":
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
        if not any(_READS_RINEX.search(ln) for ln in lines):
            continue
        for k, ln in enumerate(lines):
            if _RAW.search(ln) and not ln.lstrip().startswith("#"):
                hits.append((path, k + 1, ln.strip()))
    return hits


def selftest():
    n = 0

    def chk(cond, what):
        nonlocal n
        n += 1
        assert cond, what

    chk(norm_sat("G 7") == "G07", "a blank-padded GPS number")
    chk(norm_sat("E 1") == "E01", "a blank-padded Galileo number")
    chk(norm_sat("G07") == "G07", "a canonical id is unchanged")
    chk(norm_sat("G17") == "G17", "a two-digit number is unchanged")
    chk(norm_sat("R 1") == "R01", "a blank-padded GLONASS slot")
    chk(norm_sat("G") == "G" and norm_sat("") == "", "short strings pass through")
    chk(record_sat("G 7  21345678.123 7") == "G07", "the id of a full record line")
    chk(record_sat("G07  21345678.123 7") == "G07", "the id of a canonical record line")

    ok = "            sat = norm_sat(line[0:3])\n   OBS TYPES\n"
    bad = "            sat = line[0:3]\n   OBS TYPES\n"
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        a, b, c = d + "/a.py", d + "/b.py", d + "/c.py"
        for path, text in ((a, ok), (b, bad), (c, "sat = line[0:3]\n")):
            with open(path, "w") as fh:
                fh.write(text)
        chk(lint([a]) == [], "a normalized slice passes the lint")
        chk([h[1] for h in lint([b])] == [1], "a raw slice in a RINEX reader is listed")
        chk(lint([c]) == [], "a raw slice in a file that reads no RINEX is not listed")
    raw = lint([HERE + "/" + r for r in READERS])
    chk(raw == [], "no reader of RINEX 3 records slices the id raw: %s" % raw)
    print("rinex_ids selftest: %d/%d PASS" % (n, n))


def main(argv):
    if argv[:1] == ["selftest"]:
        selftest()
        return 0
    if argv[:1] == ["lint"]:
        hits = lint()
        for path, k, text in hits:
            print("%s:%d: %s" % (os.path.relpath(path, os.path.dirname(HERE)), k, text))
        print("%d raw slice(s)" % len(hits))
        return 1 if hits else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
