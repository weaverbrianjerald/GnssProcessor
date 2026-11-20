"""Links held through a slip (card GRA, plan step G4; bars B7 and B7b of card NRF): bar 7 of section 110 as amended in
110.1 (a Galileo link held through a slip that a reference run flags on it), the slip bar F3 of section 112 and C3 of
section 118 (held through an injected slip), and the injected slips' truth read from a dropslip bed's files.

    py -3.14 scripts/grading/slip_hold.py flagged <reference run> <tested run> [--stations A,B,...] [--list N]
                                          [--out held.jsonl] [--view legacy|lli|check3|chord] [--sys E|G]
    py -3.14 scripts/grading/slip_hold.py injected <injected.jsonl> <run> [<run> ...]
    py -3.14 scripts/grading/slip_hold.py truth <arm> <base> <out.jsonl> [--types L1X,L5X]
    py -3.14 scripts/grading/slip_hold.py --selftest

A flag is an event of the reference run on a link of the constellation (--sys: E Galileo, the default; G GPS) with a slip
action (FLAG_ACTIONS below: section 110's list as
amended in 110.1, T0_band_scope common_band left out).  The tested run holds the link through the flag at epoch t when
its last ring-fix row of the link's chord (the chord whose
first signal is on band 1) before t (within 20 epochs) is held and, at t or the next epoch, the chord
is still held or goes to the tree (RINGFIX reattach or to_tree), with no RINGFIX release of it in that window; when the
chord has no row in that window, its next row (within 20 epochs) decides, with no release in between.

The views of `flagged`:
  legacy  byte for byte slip_hold_check.py (its printout and its --out file): a flag counts only when the event's
          detail names an E1 or E5a signal in its leading tokens, so every lli_slip event (detail "LLI", "LLI;mw_step",
          "LLI;gf_step": one event per satellite for every band with loss of lock, no band named) is dropped; the
          windows are 30 s and 600 s
  lli     the same rule with the lli_slip refloat events counted as flags of the link (card GRA, defect D3), reported
          apart, and the windows set from the tested run's epoch interval (one epoch and 20 epochs; equal to the
          legacy windows at 30 s)
  check3  byte for byte slip_hold_check3.py, the five-day read's copy (its printout and its --out file): legacy's rule,
          the lli_slip refloat events not already a flag counted as flags of the link and printed on a line of their
          own after the total, the 30 s and 600 s windows; with --sys G a flag on any GPS L1 or L2 signal (L[12]x)
          counts against the link's L1/L2 chord, so a gap of a third signal (L2S, L2L), which no ring-fix chord uses,
          counts as a flag of the L1C/L2W chord (card GRA part 2, results of 2026-11-19)
  chord   the corrected rule of the five-day read's ruling: a flag counts against a held link only when it names a
          signal of that link's chord, read from the tested run's ring-fix row before the flag (else the first row
          after it); flags on other signals only are counted on a line of their own; the lli_slip refloat events at
          an epoch with no flag of the chord are counted apart as the chord's (their band is unnamed: an upper bound);
          windows from the tested run's epoch interval, as in lli
`legacy` and `lli` read Galileo by default and take --sys G as well (the signal rule L[15]x for E, L[12]x for G).
`injected` is grade_slip_bar.py (one view: its truth names its signals); `truth` is injected_slips.py.
"""
import argparse
import bisect
import collections
import datetime
import json
import re
import sys

import runs

SIG = re.compile(r"^L[15][A-Z]$")
SYS_SIG = {"E": SIG, "G": re.compile(r"^L[12][A-Z]$")}   # the legacy signal rule per constellation (slip_hold_check3.py)
ANY_SIG = re.compile(r"^L[1-9][A-Z]$")                  # the chord view: any signal, matched against the chord after
SYS_NAME = {"E": "Galileo E1/E5a", "G": "GPS L1/L2"}
FLAG_ACTIONS = {
    ("T0_gap", "reentry_relevel"): {"refloat"},
    ("T0_gap", "arc_resume"): {"shifted_fresh"},
    ("T0_gap", "sig_resume"): {"shifted"},
    ("T0_mw_detect", "lli_slip"): {"refloat"},
    ("T0_mw_detect", "mw_wl_slip"): {"refloat"},
    ("T1_gf_jump", "gf_jump"): {"refloat"},
    ("T1_if_slip", "if_slip"): {"refloat"},
}
LEGACY_EPOCH = 30.0
LOOK_EPOCHS = 20


def flag_signals(tier, detail, sig=SIG):
    """The signals an event names (slip_hold_check.py's rule, sig the signal pattern): T0_gap the first token;
    otherwise the leading tokens that are signals."""
    toks = detail.split(";")
    if tier == "T0_gap":
        return [toks[0]] if toks and sig.match(toks[0]) else []
    if tier == "T0_band_scope":
        out, on = [], False
        for t in toks:
            if t.startswith("implicated="):
                on, t = True, t.split("=", 1)[1]
            elif "=" in t:
                on = False
            if on and sig.match(t):
                out.append(t)
        return out
    out = []
    for t in toks:
        if sig.match(t):
            out.append(t)
        else:
            break
    return out


def ref_events(run, stations, sysc="E", sig=SIG):
    """The reference run's slip events on links of one constellation: (flags {(station, sat, epoch key):
    {"tier:event"}}, signals {the same key: {signal}}, lli {keys of lli_slip refloat events naming no signal})."""
    flags, sigs, lli = collections.defaultdict(set), collections.defaultdict(set), set()
    _, it = runs.raw_rows(run, "network_events.csv")
    for r in it:
        if len(r) < 10 or not r[4].startswith(sysc):
            continue
        acts = FLAG_ACTIONS.get((r[2], r[3]))
        if not acts or r[8] not in acts:
            continue
        if stations and r[5] not in stations:
            continue
        key = (r[5], r[4], runs.tow_key(r[1]))
        named = flag_signals(r[2], r[9], sig)
        if named:
            flags[key].add(f"{r[2]}:{r[3]}")
            sigs[key].update(named)
        elif (r[2], r[3]) == ("T0_mw_detect", "lli_slip"):
            lli.add(key)
    return flags, sigs, lli


def ref_flags(run, stations, with_lli=False, sysc="E"):
    """{(station, sat, epoch key): {"tier:event"}} of the reference run; with_lli adds the lli_slip refloat events,
    which name no signal, and returns them apart: (flags, lli_only)."""
    flags, _, lli = ref_events(run, stations, sysc, SYS_SIG[sysc])
    lli_only = {k: {"T0_mw_detect:lli_slip"} for k in lli if k not in flags} if with_lli else {}
    return flags, lli_only


def tested(run, keys, sysc="E", chords=None):
    """The tested run's band-1 chord rows (epoch key, state, n1, n2) and RINGFIX release / to_tree / reattach events,
    per link of keys; chords, when given, is filled with {(station, sat, epoch key): (sig1, sig2)}."""
    rows = collections.defaultdict(list)
    header, it = runs.raw_rows(run, "network_ring_fix.csv")
    ix = {k: i for i, k in enumerate(header)}
    ista, isat, is1, is2, itow, ist, in1, in2 = (ix[k] for k in ("station", "sat", "sig1", "sig2", "tow", "state",
                                                                 "n1", "n2"))
    for r in it:
        if r[isat].startswith(sysc) and r[is1][:2] == "L1" and (r[ista], r[isat]) in keys:
            t = runs.tow_key(r[itow])
            rows[(r[ista], r[isat])].append((t, r[ist], r[in1], r[in2]))
            if chords is not None:
                chords[(r[ista], r[isat], t)] = (r[is1], r[is2])
    for v in rows.values():
        v.sort()
    ev = collections.defaultdict(list)
    _, it = runs.raw_rows(run, "network_events.csv")
    for r in it:
        if len(r) > 9 and r[2] == "RINGFIX" and r[3] in ("release", "to_tree", "reattach") and "sig1=L1" in r[9]:
            if (r[5], r[4]) in keys:
                ev[(r[5], r[4])].append((runs.tow_key(r[1]), r[3]))
    return rows, ev


def verdict(rows, ev, t, epoch=LEGACY_EPOCH):
    """(verdict, the row before, the row after) of a link at a flag's epoch t."""
    look = LOOK_EPOCHS * epoch
    tows = [x[0] for x in rows]
    i = bisect.bisect_left(tows, t)
    if i == 0 or rows[i - 1][0] < t - look or rows[i - 1][1] != "held":
        return "n/a", rows[i - 1] if i else None, None
    prev = rows[i - 1]
    win = [e for e in ev if t <= e[0] <= t + epoch]
    if any(k == "release" for _, k in win):
        return "released", prev, None
    if any(k in ("reattach", "to_tree") for _, k in win):
        return "held_through_tree", prev, None
    inwin = [x for x in rows[i:] if x[0] <= t + epoch]
    if inwin:
        return ("held_through" if all(x[1] == "held" for x in inwin) else "released"), prev, inwin[-1]
    nxt = rows[i] if i < len(rows) and rows[i][0] <= t + look else None
    if nxt is None:
        return "no_row_after", prev, None
    if any(k == "release" for tw, k in ev if t <= tw <= nxt[0]):
        return "released", prev, nxt
    return ("held_through" if nxt[1] == "held" else "released"), prev, nxt


def epoch_interval(run):
    """The run's epoch interval (s): the most common spacing of the first 200 epochs of products.txt."""
    tows = []
    for t, _ in runs.product_epochs(run):
        tows.append(t)
        if len(tows) > 200:
            break
    gaps = collections.Counter(b - a for a, b in zip(tows, tows[1:]))
    return float(gaps.most_common(1)[0][0]) if gaps else LEGACY_EPOCH


def grade(flags, rows, ev, epoch):
    per, bad = collections.defaultdict(collections.Counter), []
    for (sta, sat, t), kinds in sorted(flags.items(), key=lambda kv: kv[0][2]):
        v, prev, after = verdict(rows.get((sta, sat), []), ev.get((sta, sat), []), t, epoch)
        per[sta]["flags"] += 1
        per[sta][v] += 1
        if v.startswith("held_through"):
            bad.append({"station": sta, "sat": sat, "tow": t, "flag": sorted(kinds), "verdict": v,
                        "before": prev, "after": after})
    return per, bad


def flagged_report(ref, arm, stations, n_list, per, bad, sysc="E"):
    tot = collections.Counter()
    for c in per.values():
        tot.update(c)
    out = [f"== flags of {ref} on {SYS_NAME[sysc]} links, holds of {arm}" + (f" ({len(stations)} receivers)" if stations else ""),
           f"   total: {dict(tot)}"]
    for sta in sorted(per, key=lambda s: -(per[s]['held_through'] + per[s]['held_through_tree'])):
        c = per[sta]
        n = c["held_through"] + c["held_through_tree"]
        if n:
            out.append(f"   {sta}: held through {n} (into the tree {c['held_through_tree']}) of {c['flags']} flags; "
                       f"released {c['released']}, not held before {c['n/a']}")
    for b in bad[:n_list]:
        out.append(f"      {b['tow']} {b['station']}-{b['sat']} {','.join(b['flag'])} {b['verdict']} before {b['before']} "
                   f"after {b['after']}")
    return out


def cmd_flagged(a):
    if a.view == "check3":
        return flagged_check3(a)
    if a.view == "chord":
        return flagged_chord(a)
    stations = set(s for s in a.stations.split(",") if s)
    lli = a.view == "lli"
    flags, lli_only = ref_flags(a.ref, stations, with_lli=lli, sysc=a.sys)
    keys = {(s, v) for s, v, _ in flags} | {(s, v) for s, v, _ in lli_only}
    rows, ev = tested(a.arm, keys, a.sys)
    epoch = epoch_interval(a.arm) if lli else LEGACY_EPOCH
    per, bad = grade(flags, rows, ev, epoch)
    out = flagged_report(a.ref, a.arm, stations, a.list, per, bad, a.sys)
    if lli:
        per2, bad2 = grade(lli_only, rows, ev, epoch)
        tot2 = collections.Counter()
        for c in per2.values():
            tot2.update(c)
        out.append(f"   lli_slip refloat events not already a flag (their band unnamed, taken as the link's E1/E5a; epoch "
                   f"{epoch:g} s): {len(lli_only)}; {dict(tot2)}")
        for b in bad2[: a.list]:
            out.append(f"      {b['tow']} {b['station']}-{b['sat']} lli_slip {b['verdict']} before {b['before']} after {b['after']}")
        bad = bad + bad2
    write_out(out, bad, a.out)


def write_out(out, bad, path):
    """The printout, and the held-through cases as JSON lines when a --out file is named."""
    sys.stdout.write("\n".join(out) + "\n")
    if path:
        with open(path, "w") as fh:
            for b in bad:
                fh.write(json.dumps(b) + "\n")


def flagged_check3(a):
    """slip_hold_check3.py byte for byte: its printout and its --out file."""
    stations = set(s for s in a.stations.split(",") if s)
    flags, _, lli = ref_events(a.ref, stations, a.sys, SYS_SIG[a.sys])
    lli_flags = {k: {"T0_mw_detect:lli_slip"} for k in lli if k not in flags}
    keys = {(s, v) for s, v, _ in flags} | {(s, v) for s, v, _ in lli_flags}
    rows, ev = tested(a.arm, keys, a.sys)
    per, bad = grade(flags, rows, ev, LEGACY_EPOCH)
    for b in bad:
        b["source"] = "signal"
    per_l, bad_l = grade(lli_flags, rows, ev, LEGACY_EPOCH)
    for b in bad_l:
        b["source"] = "lli_slip"
    tot_l = collections.Counter()
    for c in per_l.values():
        tot_l.update(c)
    out = flagged_report(a.ref, a.arm, stations, a.list, per, bad + bad_l, a.sys)
    out.insert(2, f"   lli_slip refloat events (no signal named; the original drops them): {len(lli)}; of them not "
                  f"already a flag, counted as flags of the link: {dict(tot_l)}")
    write_out(out, bad + bad_l, a.out)


def chord_at(chords, rows, link, t):
    """The (sig1, sig2) of a link's ring-fix row before epoch t, else of its first row after t, else None."""
    if not rows:
        return None
    i = bisect.bisect_left([x[0] for x in rows], t)
    return chords.get(link + (rows[i - 1 if i else 0][0],))


def counter_total(per):
    tot = collections.Counter()
    for c in per.values():
        tot.update(c)
    return dict(tot)


def flagged_chord(a):
    """The corrected rule: a flag counts against a held link only when it names a signal of that link's chord."""
    stations = set(s for s in a.stations.split(",") if s)
    flags, sigs, lli = ref_events(a.ref, stations, a.sys, ANY_SIG)
    keys = {(s, v) for s, v, _ in flags} | {(s, v) for s, v, _ in lli}
    chords = {}
    rows, ev = tested(a.arm, keys, a.sys, chords)
    epoch = epoch_interval(a.arm)
    on, other = {}, {}
    for k, kinds in flags.items():
        ch = chord_at(chords, rows.get(k[:2], []), k[:2], k[2])
        (on if ch and sigs[k] & set(ch) else other)[k] = kinds
    lli_flags = {k: {"T0_mw_detect:lli_slip"} for k in lli if k not in on}
    per, bad = grade(on, rows, ev, epoch)
    per_o, bad_o = grade(other, rows, ev, epoch)
    per_l, bad_l = grade(lli_flags, rows, ev, epoch)
    for src, bb in (("chord", bad), ("other", bad_o), ("lli_slip", bad_l)):
        for b in bb:
            b["source"] = src
            b["signals"] = sorted(sigs.get((b["station"], b["sat"], b["tow"]), ()))
    name = {"E": "Galileo", "G": "GPS"}[a.sys]
    out = [f"== flags of {a.ref} on {name} links, holds of {a.arm}, by the chord's own signals (epoch {epoch:g} s)"
           + (f" ({len(stations)} receivers)" if stations else ""),
           f"   on a signal of the link's chord: {counter_total(per)}",
           f"   on other signals only (not a flag of the chord): {counter_total(per_o)}",
           f"   lli_slip refloat events with no flag of the chord (band unnamed, taken as the chord's): "
           f"{counter_total(per_l)}"]
    for label, p in (("chord", per), ("other signals", per_o), ("lli_slip", per_l)):
        for sta in sorted(p, key=lambda x: (-(p[x]["held_through"] + p[x]["held_through_tree"]), x)):
            c = p[sta]
            n = c["held_through"] + c["held_through_tree"]
            if n:
                out.append(f"   {sta} ({label}): held through {n} (into the tree {c['held_through_tree']}) of "
                           f"{c['flags']} flags; released {c['released']}, not held before {c['n/a']}")
    allbad = bad + bad_o + bad_l
    for b in allbad[: a.list]:
        link = (b["station"], b["sat"])
        ch = chord_at(chords, rows.get(link, []), link, b["tow"])
        out.append(f"      {b['tow']} {b['station']}-{b['sat']} {','.join(b['flag'])} on {','.join(b['signals']) or '-'} "
                   f"(chord {'/'.join(ch) if ch else '-'}; {b['source']}) {b['verdict']} before {b['before']} "
                   f"after {b['after']}")
    write_out(out, allbad, a.out)


def cmd_injected(a):
    with open(a.injected) as fh:
        inj = [json.loads(line) for line in fh]
    flags = {(sta, sat, round(tow)) for sta, sat, typ, tow, step in inj}
    keys = {(sta, sat) for sta, sat, t in flags}
    for tag in a.runs:
        rows, ev = tested(tag, keys)
        first = min((r[0][0] for r in rows.values() if r), default=None)
        last = max((r[-1][0] for r in rows.values() if r), default=None)
        tally, held = collections.Counter(), []
        for (sta, sat, t) in sorted(flags, key=lambda x: x[2]):
            if first is None or t < first or t > last:
                continue
            v, prev, after = verdict(rows.get((sta, sat), []), ev.get((sta, sat), []), t)
            tally[v] += 1
            if v.startswith("held_through"):
                held.append((t, sta, sat, prev, after))
        n = tally["held_through"] + tally["held_through_tree"]
        print(f"== {tag}: injected slips inside the run {sum(tally.values())}; on a held link {sum(tally.values()) - tally['n/a']}; "
              f"held through {n} (into the tree {tally['held_through_tree']}); released {tally['released']}; {dict(tally)}")
        for h in held[:10]:
            print(f"      {h}")


def rinex_phases(path, types):
    """{(tow, sat): {type: value}} of a RINEX 3 observation file for the listed types (injected_slips.py's reader;
    GPS time of week from the epoch's date, the files being in GPS time)."""
    out, idx, tow, pending, sysl, head = {}, {}, None, [], None, True
    g0 = datetime.datetime(1980, 1, 6)
    with open(path, encoding="ascii", errors="replace") as fh:
        for ln in fh:
            if head:
                if ln[60:79] == "SYS / # / OBS TYPES":
                    if ln[0] != " ":
                        sysl = ln[0]
                        pending = ln[7:60].split()
                    else:
                        pending += ln[7:60].split()
                    idx[sysl] = pending
                if "END OF HEADER" in ln:
                    head = False
                continue
            if ln.startswith(">"):
                y, mo, d, h, mi = (int(x) for x in ln[2:18].split())
                dt = datetime.datetime(y, mo, d, h, mi) + datetime.timedelta(seconds=float(ln[18:29]))
                tow = round(((dt - g0).total_seconds()) % (7 * 86400), 3)
                continue
            sat = ln[0:3]
            vals = {}
            for k, c in enumerate(idx.get(sat[0], [])):
                if c not in types:
                    continue
                f = ln[3 + 16 * k: 3 + 16 * k + 14]
                if f.strip():
                    vals[c] = float(f)
            out[(tow, sat)] = vals
    return out


def cmd_truth(a):
    types = set(a.types.split(","))
    arm = json.load(open(runs.run_dir(a.arm) + "/arm.json", encoding="utf-8"))
    base = json.load(open(runs.run_dir(a.base) + "/arm.json", encoding="utf-8"))
    src = {s["name"]: s["rinex_path"] for s in base["network"]["stations"]}
    n = 0
    with open(a.out, "w") as fo:
        for s in arm["network"]["stations"]:
            p = s["rinex_path"]
            if f"_rap_{a.arm}/rinex" not in p.replace("\\", "/"):
                continue
            e_arm, e_src = rinex_phases(p, types), rinex_phases(src[s["name"]], types)
            last = {}
            for (tow, sat) in sorted(e_arm):
                for c, v in e_arm[(tow, sat)].items():
                    v0 = e_src.get((tow, sat), {}).get(c)
                    if v0 is None:
                        continue
                    d = round(v - v0)
                    if abs(v - v0 - d) > 1e-3:
                        continue
                    if d != last.get((sat, c), 0):
                        fo.write(json.dumps([s["name"], sat, c, tow, d - last.get((sat, c), 0)]) + "\n")
                        n += 1
                    last[(sat, c)] = d
    print(f"{n} injected step(s) written to {a.out}")


def selftest():
    import os
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g4_slip.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        flags, lli = ref_flags("ref", set(), with_lli=True)
        check(set(flags) == {("AAAA00XXX", "E04", 172860), ("BBBB00YYY", "E05", 172860)}, "flags naming E1/E5a")
        check(set(lli) == {("CCCC00ZZZ", "E09", 172890)}, "an lli_slip event that is not already a flag")
        legacy, _ = ref_flags("ref", set())
        check(set(legacy) == set(flags), "the legacy rule drops every lli_slip event")
        keys = {(s, v) for s, v, _ in flags} | {(s, v) for s, v, _ in lli}
        rows, ev = tested("arm", keys)
        check(verdict(rows[("AAAA00XXX", "E04")], ev[("AAAA00XXX", "E04")], 172860)[0] == "released", "a release")
        check(verdict(rows[("BBBB00YYY", "E05")], ev[("BBBB00YYY", "E05")], 172860)[0] == "held_through",
              "held at the flag and the next epoch")
        check(verdict(rows[("CCCC00ZZZ", "E09")], ev[("CCCC00ZZZ", "E09")], 172890)[0] == "held_through_tree",
              "taken into the tree")
        check(epoch_interval("arm") == 30.0, "the epoch interval")
        check(verdict([(100, "held", "1", "2"), (101, "held", "1", "2")], [], 101, epoch=1.0)[0] == "held_through",
              "a 1 s run's window is one epoch")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("slip_hold.py")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("flagged")
    f.add_argument("ref")
    f.add_argument("arm")
    f.add_argument("--stations", default="")
    f.add_argument("--list", type=int, default=20)
    f.add_argument("--out", default="")
    f.add_argument("--view", choices=("legacy", "lli", "check3", "chord"), default="legacy")
    f.add_argument("--sys", choices=("E", "G"), default="E")
    i = sub.add_parser("injected")
    i.add_argument("injected")
    i.add_argument("runs", nargs="+")
    t = sub.add_parser("truth")
    t.add_argument("arm")
    t.add_argument("base")
    t.add_argument("out")
    t.add_argument("--types", default="L1X,L5X")
    a = ap.parse_args()
    {"flagged": cmd_flagged, "injected": cmd_injected, "truth": cmd_truth}[a.cmd](a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
