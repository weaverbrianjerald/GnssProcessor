"""The users' bars of card NRF (card GRA, plan step G5; bars B9, B10 and B11): the float check of a fixing user's holds
inside and outside the network, the paired per-receiver difference, the hourly matrix and the difference of medians,
and the reference changes.

    py -3.14 scripts/grading/users.py inside <network run> --fix L --flt L --doy D [--pair "E L1C/L5Q"] [--hours 6]
                                      [--conv 0.20] [--per-site] [--list N] [--sites a,b]
    py -3.14 scripts/grading/users.py outside --fix L --flt L --doy D --sites a,b [--pair "E L1X/L5X"] [--conv 0.20]
    py -3.14 scripts/grading/users.py both --fix L --flt L --doy D --sites a,b [--pair P] [--net <network run>]
    py -3.14 scripts/grading/users.py paired <label A> <label B> --doy D --sites a,b [--first 3] [--last 24] [--name N]
    py -3.14 scripts/grading/users.py hourly <kind:label[:shift]> ... --doy D [--set S] [--base K] [--per-site]
                                      [--hours 6] [--matrix]
    py -3.14 scripts/grading/users.py medians <arm> --doy D --sites a,b [--base a24] [--span 14 24] [--view legacy|exact]
    py -3.14 scripts/grading/users.py refch <user run> ... [--sums]
    py -3.14 scripts/grading/users.py --selftest

A user run is _rap_nw_obs<label>_<doy>_<site> (a fixing label ends in x, a float label in f); the day is always given
with --doy (the older scripts read NW_DOY and fell back to 144, card GRA defect D8).  The resolver lines (CLFRAC) are
read with runs.clfrac_lines.

The checks and their views:
  inside   legacy: user_fix_vs_float_full.py byte for byte.  A held pair of the fixing user is counted only when the
           network has an integer at that receiver for both satellites (its pins of logs/network_chain_edges.csv and
           its held and decided chords of logs/network_ring_fix.csv), and is judged on the narrow lane against the
           float user: a near-null (3, 4) error passes (card GRA, defect D2).  Its receivers are every network station
           with both user runs of the labels (38 on today's streams, 10 for an arm's users), so two labels can be
           counted over different sets; --sites restricts the count to a list (user_fix_vs_float_full_sites.py)
  outside  legacy: user_fix_vs_float_free.py byte for byte: every hold, both lanes against the float user
  both     the corrected check for any receiver, inside the network or not: every hold of the fixing user judged on
           both lanes against the float user; with --net, each hold also counted as known or not to the network
  paired   paired_users.py (one view): per hour the median over the receivers of each receiver's own difference
  hourly   legacy: users_hourly2.py byte for byte (the matrix the difference of medians is read from); exact: the
           same, saying so when --base is not one of the run sets (the legacy view prints no paired column then)
  medians  legacy: bar6_receivers.py, which takes the difference of medians from the matrix printed to 0.1 mm;
           exact: the same from the unrounded hourly medians
  refch    ref_changes_full.py byte for byte; --sums adds the totals per label and constellation over the receivers
"""
import argparse
import csv
import json
import os
import re
import statistics as st
import sys
from collections import defaultdict

import runs

FREQ = {"G": (1575.42e6, 1227.60e6), "E": (1575.42e6, 1176.45e6)}
SD = re.compile(r"^\[\s*\d+,\s*([\d.]+) .*CLFRAC sat_i=(\w+) sat_ref=(\w+) sig=(\w+) sd_cyc=(-?[\d.]+)")
REF = re.compile(r"^\[\s*\d+,\s*([\d.]+) .*CLFRAC sat_i=(\w+) sat_ref=(\w+) sig=(\w+)")
NETWORK_STATES = ("held", "decided")
SETS = {
    "net28": "bogt,bor1,cas1,cpvg,cusv,ffmj,gamb,gcgo,gope,kokv,krgg,lck4,leij,lmmf,mbar,mro1,pots,pove,sctb,sthl,"
             "svtl,syog,ulab,urum,voim,wark,wind,ykro",
    "ind11": "arht,ascg,badg,cedu,chti,cibg,quin,stpm,utqi,wtzz,wuh2",
    "test17": "arht,ascg,badg,bake,cedu,chti,cibg,dav1,harb,kokb,pohn,quin,scrz,stpm,utqi,wtzz,wuh2",
}


def user_run(label, doy, site, kind="obs"):
    return f"_rap_nw_{kind}{label}_{doy}_{site}"


def user_sd(run, sigs):
    """{epoch key: {(sat, ref): {signal: single difference, cycles}}} of a user run's CLFRAC lines."""
    out = defaultdict(lambda: defaultdict(dict))
    for line in runs.clfrac_lines(run):
        m = SD.match(line)
        if not m or m.group(4) not in sigs:
            continue
        out[int(round(float(m.group(1))))][(m.group(2), m.group(3))][m.group(4)] = float(m.group(5))
    return out


def network_pins(run, sy):
    """{band: {epoch key: {(station, sat): integer}}} of the chain's tree links with a pin target."""
    out = defaultdict(lambda: defaultdict(dict))
    for r in runs.rows(run, "network_chain_edges.csv"):
        if not r["target_cyc"] or r["edge_sat"][0] != sy:
            continue
        out[r["band"]][int(round(float(r["tow"])))][(r["edge_sta"], r["edge_sat"])] = int(round(float(r["target_cyc"])))
    return out


def network_holds(run, pair):
    """((band 1, band 2): {epoch key: {(station, sat): integer}}, held link-epochs, the trace's first epoch key) of the
    pair family's held and decided chords."""
    sy, sigs = pair.split()
    s1, s2 = sigs.split("/")
    h1, h2, n, t0 = defaultdict(dict), defaultdict(dict), 0, None
    for r in runs.rows(run, "network_ring_fix.csv"):
        if r.get("audit") is None or not r.get("tow"):
            continue
        t = int(round(float(r["tow"])))
        if t0 is None:
            t0 = t
        if r["state"] not in NETWORK_STATES or r["sat"][0] != sy or r["sig1"] != s1 or r["sig2"] != s2:
            continue
        if not r["n1"] or not r["n2"]:
            continue
        h1[t][(r["station"], r["sat"])] = int(r["n1"])
        h2[t][(r["station"], r["sat"])] = int(r["n2"])
        n += 1
    return (h1, h2), n, t0


def offsets(sd_t, sta, i1, i2, s1, s2, f1, f2):
    """sat -> (wide offset, narrow offset, held) against the network, relative to the epoch's reference satellite."""
    out, ref = {}, None
    for (si, sr), v in sd_t.items():
        if s1 not in v or s2 not in v:
            continue
        ka, kb = (sta, si), (sta, sr)
        if ka not in i1 or kb not in i1 or ka not in i2 or kb not in i2:
            continue
        d1 = v[s1] - (i1[ka] - i1[kb])
        d2 = v[s2] - (i2[ka] - i2[kb])
        held = abs(v[s1] - round(v[s1])) <= 0.01 and abs(v[s2] - round(v[s2])) <= 0.01
        out[si] = (d1 - d2, (f1 * d1 - f2 * d2) / (f1 - f2), held)
        ref = sr
    if ref is not None and out:
        out[ref] = (0.0, 0.0, True)
    return out, ref


def per_sat(sd_t, s1, s2):
    """sat -> (sd on s1, sd on s2) relative to the epoch's reference satellite, the reference at (0, 0)."""
    out, ref = {}, None
    for (si, sr), v in sd_t.items():
        if s1 in v and s2 in v:
            out[si] = (v[s1], v[s2])
            ref = sr
    if ref is not None:
        out[ref] = (0.0, 0.0)
    return out, ref


def cmd_inside(a):
    sy, sg = a.pair.split()
    s1, s2 = sg.split("/")
    f1, f2 = FREQ[sy]
    pins = network_pins(a.net, sy)
    (h1, h2), _, t0 = network_holds(a.net, a.pair)
    with open(os.path.join(runs.run_dir(a.net), "arm.json"), encoding="utf-8") as fh:
        ncfg = json.load(fh)

    def blank():
        return {"held": 0, "right": 0, "wrong": 0, "unv": 0, "wl_bad": 0, "net_eq": 0}

    hour, site_acc = defaultdict(blank), defaultdict(blank)
    wrong = defaultdict(lambda: [0, None, None])
    nsites = 0
    only = set(a.sites.split(",")) if a.sites else None
    for stn in ncfg["network"]["stations"]:
        sta = stn["name"]
        site = sta[:4].lower()
        if only is not None and site not in only:
            continue
        rf, rl = user_run(a.fix, a.doy, site), user_run(a.flt, a.doy, site)
        if not os.path.isdir(runs.run_dir(rf)) or not os.path.isdir(runs.run_dir(rl)):
            continue
        sdf, sdl = user_sd(rf, (s1, s2)), user_sd(rl, (s1, s2))
        used = False
        for t, pf in sdf.items():
            hb = int((t - t0) // 3600)
            if hb < 0 or hb >= a.hours:
                continue
            i1 = dict(pins[s1].get(t, {}))
            i1.update(h1.get(t, {}))
            i2 = dict(pins[s2].get(t, {}))
            i2.update(h2.get(t, {}))
            of, ref = offsets(pf, sta, i1, i2, s1, s2, f1, f2)
            if ref is None:
                continue
            ol, _ = offsets(sdl.get(t, {}), sta, i1, i2, s1, s2, f1, f2)
            for s, (dw, dn, held) in of.items():
                if s == ref or not held:
                    continue
                used = True
                for x in (hour[hb], site_acc[site]):
                    x["held"] += 1
                    x["wl_bad"] += round(dw) != 0
                    x["net_eq"] += round(dn) == 0 and round(dw) == 0
                cls = "unv"
                if s in ol and ref in ol:
                    fl = ol[s][1] - ol[ref][1]
                    if abs(fl - round(fl)) <= a.conv:
                        cls = "right" if round(fl) == round(dn) else "wrong"
                for x in (hour[hb], site_acc[site]):
                    x[cls] += 1
                if cls == "wrong":
                    rec = wrong[(site, s, ref, int(round(dn)), int(round(ol[s][1] - ol[ref][1])))]
                    rec[0] += 1
                    if rec[1] is None:
                        rec[1] = t
                    rec[2] = t
        nsites += used
    print(f"{a.pair}; network {a.net}; fixing users {a.fix} against float users {a.flt}; {nsites} receivers; "
          f"float counted as converged within {a.conv:g} cycle")
    print("  hour | held pair-epochs | equal to the network | right by the float | wrong by the float | unverified | wide lane off")
    tot = blank()
    for hb in range(a.hours):
        x = hour[hb]
        for k in tot:
            tot[k] += x[k]
        if not x["held"]:
            continue
        ver = x["right"] + x["wrong"]
        print(f"  {hb + 1:4d} | {x['held']:16d} | {100 * x['net_eq'] / x['held']:19.1f}% | {x['right']:8d} "
              f"({(100 * x['right'] / ver) if ver else float('nan'):5.1f}%) | {x['wrong']:18d} | {x['unv']:10d} | {x['wl_bad']}")
    ver = tot["right"] + tot["wrong"]
    if tot["held"]:
        print(f"   all | {tot['held']:16d} | {100 * tot['net_eq'] / tot['held']:19.1f}% | {tot['right']:8d} "
              f"({(100 * tot['right'] / ver) if ver else float('nan'):5.1f}%) | {tot['wrong']:18d} | {tot['unv']:10d} | {tot['wl_bad']}")
    if a.per_site:
        print("  per receiver: held, right, wrong, unverified")
        for site in sorted(site_acc):
            x = site_acc[site]
            if x["held"]:
                print(f"    {site}: held {x['held']:5d} | right {x['right']:5d} | wrong {x['wrong']:4d} | unverified {x['unv']:5d}"
                      + (f" | wide lane off {x['wl_bad']}" if x["wl_bad"] else ""))
    if a.list:
        for (site, s, ref, kf, kl), (c, ta, tb) in sorted(wrong.items(), key=lambda kv: -kv[1][0])[:a.list]:
            print(f"      {site} {s}-{ref}: fixing user at {kf:+d}, float user at {kl:+d} (against the network): {c} epochs, tow {ta} to {tb}")


def both_lanes(a, network=None):
    """The both-lanes check of every hold: per hour of the day and per receiver, held / known / right / wrong / unv."""
    sy, sg = a.pair.split()
    s1, s2 = sg.split("/")
    f1, f2 = FREQ[sy]
    hour, site_acc = defaultdict(lambda: defaultdict(int)), defaultdict(lambda: defaultdict(int))
    wrongs, missing = defaultdict(int), []
    for site in a.sites.split(","):
        rf, rl = user_run(a.fix, a.doy, site), user_run(a.flt, a.doy, site)
        if not os.path.isdir(runs.run_dir(rf)) or not os.path.isdir(runs.run_dir(rl)):
            missing.append(site)
            continue
        sdf, sdl = user_sd(rf, (s1, s2)), user_sd(rl, (s1, s2))
        if not sdf:
            missing.append(site)
            continue
        day0 = (min(sdf) // 86400) * 86400
        for t, pf in sdf.items():
            hb = int((t - day0) // 3600)
            if hb < 0 or hb >= a.hours:
                continue
            of, ref = per_sat(pf, s1, s2)
            if ref is None:
                continue
            ol, _ = per_sat(sdl.get(t, {}), s1, s2)
            known_sats = network(site, t) if network else None
            for s, (v1, v2) in of.items():
                if s == ref:
                    continue
                if abs(v1 - round(v1)) > 0.01 or abs(v2 - round(v2)) > 0.01:
                    continue
                cls = "unv"
                if s in ol and ref in ol:
                    d1 = (ol[s][0] - ol[ref][0]) - v1
                    d2 = (ol[s][1] - ol[ref][1]) - v2
                    dw, dn = d1 - d2, (f1 * d1 - f2 * d2) / (f1 - f2)
                    if abs(dw - round(dw)) <= a.conv and abs(dn - round(dn)) <= a.conv:
                        cls = "right" if round(dw) == 0 and round(dn) == 0 else "wrong"
                        if cls == "wrong":
                            wrongs[(site, s, ref, int(round(dw)), int(round(dn)))] += 1
                known = known_sats is not None and s in known_sats and ref in known_sats
                for x in (hour[hb], site_acc[site]):
                    x["held"] += 1
                    x[cls] += 1
                    x["known"] += known
    return hour, site_acc, wrongs, missing


def cmd_outside(a):
    """user_fix_vs_float_free.py, byte for byte."""
    sy, sg = a.pair.split()
    s1, s2 = sg.split("/")
    f1, f2 = FREQ[sy]
    hour, site_acc, wrongs = defaultdict(lambda: defaultdict(int)), defaultdict(lambda: defaultdict(int)), defaultdict(int)
    for site in a.sites.split(","):
        rf, rl = user_run(a.fix, a.doy, site), user_run(a.flt, a.doy, site)
        if not os.path.isdir(runs.run_dir(rf)) or not os.path.isdir(runs.run_dir(rl)):
            print(f"  {site}: missing")
            continue
        sdf, sdl = user_sd(rf, (s1, s2)), user_sd(rl, (s1, s2))
        if not sdf:
            continue
        day0 = (min(sdf) // 86400) * 86400
        for t, pf in sdf.items():
            hb = int((t - day0) // 3600)
            if hb < 0 or hb >= a.hours:
                continue
            of, ref = per_sat(pf, s1, s2)
            if ref is None:
                continue
            ol, _ = per_sat(sdl.get(t, {}), s1, s2)
            for s, (v1, v2) in of.items():
                if s == ref:
                    continue
                if abs(v1 - round(v1)) > 0.01 or abs(v2 - round(v2)) > 0.01:
                    continue
                cls = "unv"
                if s in ol and ref in ol:
                    d1 = (ol[s][0] - ol[ref][0]) - v1
                    d2 = (ol[s][1] - ol[ref][1]) - v2
                    dw = d1 - d2
                    dn = (f1 * d1 - f2 * d2) / (f1 - f2)
                    if abs(dw - round(dw)) <= a.conv and abs(dn - round(dn)) <= a.conv:
                        cls = "right" if round(dw) == 0 and round(dn) == 0 else "wrong"
                        if cls == "wrong":
                            wrongs[(site, s, ref, int(round(dw)), int(round(dn)))] += 1
                for x in (hour[hb], site_acc[site]):
                    x["held"] += 1
                    x[cls] += 1
    print(f"{a.pair}; fixing users {a.fix} against float users {a.flt}, DOY {a.doy}; float converged within {a.conv:g} "
          f"cycle on both lanes")
    print("  hour |   held | right | wrong | unverified")
    tot = defaultdict(int)
    for hb in range(a.hours):
        x = hour[hb]
        for k in ("held", "right", "wrong", "unv"):
            tot[k] += x[k]
        if x["held"]:
            print(f"  {hb + 1:4d} | {x['held']:6d} | {x['right']:5d} | {x['wrong']:5d} | {x['unv']:6d}")
    print(f"   all | {tot['held']:6d} | {tot['right']:5d} | {tot['wrong']:5d} | {tot['unv']:6d}")
    print("  per receiver: held, right, wrong, unverified")
    for site in sorted(site_acc):
        x = site_acc[site]
        print(f"    {site}: held {x['held']:6d} | right {x['right']:6d} | wrong {x['wrong']:4d} | unverified {x['unv']:6d}")
    for (site, s, ref, kw, kn), c in sorted(wrongs.items(), key=lambda kv: -kv[1])[:20]:
        print(f"      {site} {s}-{ref}: float user off by wide {kw:+d}, narrow {kn:+d}: {c} epochs")


def cmd_both(a):
    network = None
    if a.net:
        sy, sg = a.pair.split()
        s1, s2 = sg.split("/")
        pins = network_pins(a.net, sy)
        (h1, h2), _, _ = network_holds(a.net, a.pair)
        with open(os.path.join(runs.run_dir(a.net), "arm.json"), encoding="utf-8") as fh:
            names = {s["name"][:4].lower(): s["name"] for s in json.load(fh)["network"]["stations"]}

        def network(site, t):
            sta = names.get(site)
            if sta is None:
                return set()
            k1 = {k[1] for k in list(pins[s1].get(t, {})) + list(h1.get(t, {})) if k[0] == sta}
            k2 = {k[1] for k in list(pins[s2].get(t, {})) + list(h2.get(t, {})) if k[0] == sta}
            return k1 & k2
    hour, site_acc, wrongs, missing = both_lanes(a, network)
    print(f"{a.pair}; fixing users {a.fix} against float users {a.flt}, DOY {a.doy}; every hold, both lanes against the "
          f"float user (converged within {a.conv:g} cycle)" + (f"; known: both satellites held or pinned by {a.net} at the receiver" if a.net else ""))
    if missing:
        print(f"  missing or empty: {','.join(missing)}")
    print("  hour |   held |  known | right | wrong | unverified")
    tot = defaultdict(int)
    for hb in range(a.hours):
        x = hour[hb]
        for k in ("held", "known", "right", "wrong", "unv"):
            tot[k] += x[k]
        if x["held"]:
            print(f"  {hb + 1:4d} | {x['held']:6d} | {x['known']:6d} | {x['right']:5d} | {x['wrong']:5d} | {x['unv']:6d}")
    print(f"   all | {tot['held']:6d} | {tot['known']:6d} | {tot['right']:5d} | {tot['wrong']:5d} | {tot['unv']:6d}")
    print("  per receiver: held, known, right, wrong, unverified")
    for site in sorted(site_acc):
        x = site_acc[site]
        print(f"    {site}: held {x['held']:6d} | known {x['known']:6d} | right {x['right']:6d} | wrong {x['wrong']:4d} | "
              f"unverified {x['unv']:6d}")
    for (site, s, ref, kw, kn), c in sorted(wrongs.items(), key=lambda kv: -kv[1])[:20]:
        print(f"      {site} {s}-{ref}: float user off by wide {kw:+d}, narrow {kn:+d}: {c} epochs")


def replay_hourly(label, doy, site, kind="obs", shift=0, hours=None):
    """{hour index: 3D median, mm} of a user run's logs/consumer_replay_hourly.csv (shifted, cut to hours if given)."""
    p = os.path.join(runs.run_dir(user_run(label, doy, site, kind)), "logs", "consumer_replay_hourly.csv")
    if not os.path.exists(p):
        return None
    out = {}
    with open(p, newline="") as fh:
        for r in csv.DictReader(fh):
            h = int(r["hour"]) + shift
            if hours is None or 0 <= h < hours:
                out[h] = float(r["err3d_median_m"]) * 1000.0
    return out or None


def cmd_paired(a):
    """paired_users.py, byte for byte (hours 1-based)."""
    sites = a.sites.split(",")
    hours = list(range(a.first, a.last + 1))

    def hourly(label, s):
        d = replay_hourly(label, a.doy, s)
        return {h + 1: v for h, v in d.items()} if d else None
    da = {s: hourly(a.a, s) for s in sites}
    db = {s: hourly(a.b, s) for s in sites}
    both = [s for s in sites if da[s] and db[s]]
    missing = [s for s in sites if s not in both]
    print(f"{a.name} DOY {a.doy}: {a.a} minus {a.b}, {len(both)} receivers"
          + (f" (missing: {','.join(missing)})" if missing else "") + f", hours {a.first} to {a.last} (mm)")
    hmed = []
    print("  hour  median  better>5  worse>5")
    for h in hours:
        v = [da[s][h] - db[s][h] for s in both if h in da[s] and h in db[s]]
        if not v:
            continue
        m = st.median(v)
        hmed.append((h, m))
        print(f"  {h:4d}  {m:+6.1f}  {sum(x < -5 for x in v):8d}  {sum(x > 5 for x in v):7d}")
    print("  receiver  median    mean   worst hour")
    rmed = []
    for s in both:
        v = [(h, da[s][h] - db[s][h]) for h in hours if h in da[s] and h in db[s]]
        if not v:
            continue
        m = st.median(x for _, x in v)
        w = max(v, key=lambda t: t[1])
        rmed.append((s, m))
        print(f"  {s:8s}  {m:+6.1f}  {st.mean(x for _, x in v):+6.1f}   {w[1]:+7.1f} (h{w[0]})")
    if hmed:
        wh = max(hmed, key=lambda t: t[1])
        wr = max(rmed, key=lambda t: t[1])
        print(f"  SUMMARY worst hourly median {wh[1]:+.1f} (h{wh[0]}), mean of hourly medians "
              f"{st.mean(m for _, m in hmed):+.2f}, worst receiver median {wr[1]:+.1f} ({wr[0]}), "
              f"receivers with a negative median {sum(m < 0 for _, m in rmed)} of {len(rmed)}")


def cmd_hourly(a):
    """users_hourly2.py, byte for byte."""
    nh = a.hours
    sites = SETS.get(a.set, a.set).split(",")
    specs = []
    for s in a.specs:
        q = s.split(":")
        specs.append((q[0], q[1], int(q[2]) if len(q) > 2 else 0))
    names = [f"{k}:{lab}" for k, lab, _ in specs]
    data = {n: {s: replay_hourly(lab, a.doy, s, k, sh, nh) for s in sites} for n, (k, lab, sh) in zip(names, specs)}
    base = a.base or names[0]
    print(f"{len(sites)} receivers ({a.set}); median over the receivers of the hourly 3D median (mm), hours 1 to {nh} of the "
          f"day; base {base}")
    for n in names:
        d = data[n]
        have = [s for s in sites if d[s]]
        med = []
        for h in range(nh):
            v = [d[s][h] for s in have if h in d[s]]
            med.append(st.median(v) if v else float("nan"))
        line = f"  {n:12s} n={len(have):2d} | " + " ".join(f"{x:6.1f}" for x in med)
        if n != base and base in data:
            b = data[base]
            both = [s for s in have if b[s]]
            dif = []
            for h in range(nh):
                v = [d[s][h] - b[s][h] for s in both if h in d[s] and h in b[s]]
                dif.append(st.median(v) if v else float("nan"))
            better = worse = 0
            for s in both:
                hs = [h for h in (3, 4, 5) if h in d[s] and h in b[s]]
                if not hs:
                    continue
                x = st.mean(d[s][h] for h in hs)
                y = st.mean(b[s][h] for h in hs)
                if x < y - 5.0:
                    better += 1
                elif x > y + 5.0:
                    worse += 1
            line += " | paired: " + " ".join(f"{x:+6.1f}" for x in dif) + f" | better {better}, worse {worse} of {len(both)}"
        elif n != base and a.view == "exact":
            line += " | paired: none (the base is not one of the run sets)"
        print(line)
    if a.per_site:
        print("  per receiver, mean of hours 4 to 6 (mm):")
        print("  site  " + " ".join(f"{n:>12s}" for n in names))
        for s in sites:
            row = []
            for n in names:
                d = data[n][s]
                hs = [h for h in (3, 4, 5) if d and h in d]
                row.append(f"{st.mean(d[h] for h in hs):12.1f}" if hs else "           -")
            print(f"  {s}  " + " ".join(row))
    if a.matrix:
        for n in names:
            print(f"  {n}: per receiver, hourly 3D median (mm), hours 1 to {nh}:")
            for s in sites:
                d = data[n][s]
                if d:
                    print(f"  {s}  " + " ".join(f"{d[h]:6.1f}" if h in d else "     -" for h in range(nh)))


def cmd_medians(a):
    """bar6_receivers.py from the runs: legacy reads the hourly medians as the matrix prints them (0.1 mm)."""
    def mat(label):
        out = {}
        for s in a.sites.split(","):
            d = replay_hourly(label, a.doy, s, hours=24)
            if d:
                out[s] = [float(f"{d[h]:6.1f}") if a.view == "legacy" else d[h] for h in range(24)]
        return out
    sites = sorted(mat(a.base + "x"))          # bar6_receivers.py: the base's fixing users with data, for both pairs
    for new, old in ((a.arm + "x", a.base + "x"), (a.arm + "f", a.base + "f")):
        mn, mo = mat(new), mat(old)
        dm = [st.median(mn[s][h] for s in sites) - st.median(mo[s][h] for s in sites) for h in range(24)]
        print(f"== {new} against {old}, DOY {a.doy}: difference of the medians over {len(sites)} receivers, hours 3 to 24 (mm):")
        print("   " + " ".join(f"{d:+.1f}" for d in dm[2:]))
        h1, h2 = a.span
        per = {s: st.mean(mn[s][h] - mo[s][h] for h in range(h1 - 1, h2)) for s in sites}
        print(f"   per receiver, mean difference over hours {h1} to {h2} (mm): "
              + ", ".join(f"{s} {d:+.1f}" for s, d in sorted(per.items(), key=lambda x: -x[1])))


def cmd_refch(a):
    """ref_changes_full.py, byte for byte; --sums adds totals per label and constellation."""
    sums = defaultdict(lambda: [0, 0])
    for run in a.runs:
        ref = defaultdict(dict)
        for line in runs.clfrac_lines(run):
            m = REF.match(line)
            if m:
                ref[m.group(3)[0]][int(round(float(m.group(1))))] = m.group(3)
        parts = []
        lab = re.match(r"^_rap_nw_obs(\w+?)_\d+_\w+$", os.path.basename(run.rstrip("/\\")))
        for sy in sorted(ref):
            ts = sorted(ref[sy])
            ch = sum(1 for x, y in zip(ts, ts[1:]) if ref[sy][x] != ref[sy][y])
            parts.append(f"{sy}: {ch} changes in {len(ts)} epochs, {len(set(ref[sy].values()))} satellites")
            if lab:
                sums[(lab.group(1), sy)][0] += ch
                sums[(lab.group(1), sy)][1] += 1
        print(f"{run}: " + ("; ".join(parts) if parts else "no line"))
    if a.sums:
        for (lab, sy), (ch, n) in sorted(sums.items()):
            print(f"  sum {lab} {sy}: {ch} changes over {n} receivers")


def selftest():
    import tempfile
    import fixture
    import io
    import contextlib
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    def capture(fn, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            fn(build_parser().parse_args(argv))
        return buf.getvalue()

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g5_users.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        # the fixing user holds E05-E11 at (3, 4) cycles off the float user's branch: a near-null error
        ins = capture(cmd_inside, ["inside", "net", "--fix", "tx", "--flt", "tf", "--doy", "146", "--pair", "E L1C/L5Q",
                                   "--hours", "1"])
        check("   all |                2 |" in ins and "|                  0 |" in ins,
              "the inside check passes the near-null hold (narrow lane only)")
        bo = capture(cmd_both, ["both", "--fix", "tx", "--flt", "tf", "--doy", "146", "--sites", "aaaa", "--pair",
                                "E L1C/L5Q", "--hours", "1", "--net", "net"])
        check("   all |      3 |      2 |     2 |     1 |      0" in bo, "both lanes: the near-null hold is wrong; known 2 of 3")
        out = capture(cmd_outside, ["outside", "--fix", "tx", "--flt", "tf", "--doy", "146", "--sites", "aaaa,zzzz",
                                    "--pair", "E L1C/L5Q", "--hours", "1"])
        check("  zzzz: missing" in out and "wrong    1" in out, "outside: a missing receiver and the wrong hold")
        p = capture(cmd_paired, ["paired", "tx", "tf", "--doy", "146", "--sites", "aaaa", "--first", "1", "--last", "2"])
        check("     1    +2.0" in p and "     2    -1.0" in p, "paired difference by hour")
        r = capture(cmd_refch, ["refch", "_rap_nw_obstx_146_aaaa", "--sums"])
        check("E: 1 changes in 2 epochs, 2 satellites" in r and "sum tx E: 1 changes over 1 receivers" in r, "reference changes")
        h = capture(cmd_hourly, ["hourly", "obs:tx", "--base", "obs:other", "--doy", "146", "--set", "aaaa", "--hours", "2",
                                 "--view", "exact"])
        check("paired: none" in h, "hourly, exact view: a base that is not a run set is said, not silently left out")
    print("selftest:", "PASS" if ok else "FAIL")
    ok &= fixture.check_frozen("users.py")
    return 0 if ok else 1


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("inside")
    i.add_argument("net")
    for p in (i,):
        p.add_argument("--pair", default="E L1X/L5X")
        p.add_argument("--hours", type=int, default=6)
    o = sub.add_parser("outside")
    b = sub.add_parser("both")
    for p in (o, b):
        p.add_argument("--sites", required=True)
        p.add_argument("--pair", default="E L1X/L5X")
        p.add_argument("--hours", type=int, default=24)
    b.add_argument("--net", default="")
    for p in (i, o, b):
        p.add_argument("--fix", required=True)
        p.add_argument("--flt", required=True)
        p.add_argument("--doy", required=True)
        p.add_argument("--conv", type=float, default=0.20)
    i.add_argument("--per-site", action="store_true")
    i.add_argument("--list", type=int, default=0)
    i.add_argument("--sites", default="", help="only these receivers (default: every network receiver with both user "
                   "runs, the legacy set)")
    pa = sub.add_parser("paired")
    pa.add_argument("a")
    pa.add_argument("b")
    pa.add_argument("--doy", required=True)
    pa.add_argument("--sites", required=True)
    pa.add_argument("--first", type=int, default=3)
    pa.add_argument("--last", type=int, default=24)
    pa.add_argument("--name", default="")
    h = sub.add_parser("hourly")
    h.add_argument("specs", nargs="+")
    h.add_argument("--doy", required=True)
    h.add_argument("--set", default="net28")
    h.add_argument("--base", default=None)
    h.add_argument("--per-site", action="store_true")
    h.add_argument("--hours", type=int, default=6)
    h.add_argument("--matrix", action="store_true")
    h.add_argument("--view", choices=("legacy", "exact"), default="legacy")
    m = sub.add_parser("medians")
    m.add_argument("arm")
    m.add_argument("--doy", required=True)
    m.add_argument("--sites", required=True)
    m.add_argument("--base", default="a24")
    m.add_argument("--span", type=int, nargs=2, default=[14, 24])
    m.add_argument("--view", choices=("legacy", "exact"), default="legacy")
    r = sub.add_parser("refch")
    r.add_argument("runs", nargs="+")
    r.add_argument("--sums", action="store_true")
    return ap


def main():
    if "--selftest" in sys.argv:
        return selftest()
    a = build_parser().parse_args()
    {"inside": cmd_inside, "outside": cmd_outside, "both": cmd_both, "paired": cmd_paired, "hourly": cmd_hourly,
     "medians": cmd_medians, "refch": cmd_refch}[a.cmd](a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
