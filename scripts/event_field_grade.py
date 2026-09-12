#!/usr/bin/env python3
"""FIELD EVENT-HARNESS grader — truth-free, everything is EVENT minus NULL.

    python scripts/event_field_grade.py runs/evleg_fld_e1_off \
        --null runs/evleg_fld_null_off [--ref-null runs/evleg_fld_null_off] \
        --station GOPE00CZE --sat G07 [--pre 5] [--post 50] [--json out.json]

The field has NO truth (SPEC_event_harness §20): every channel here is the event
leg MINUS its null leg at the SAME tow, on datum-invariant or de-biased
quantities.  The two legs resume from the SAME snapshot with the SAME config
except ``network.inject``, so the warm-resume seam transient, the bed, the
posture and (for the consumer) the station's static bias all cancel in the
difference.  NOTHING here is absolute and nothing here is a verdict.

Epoch axis ``k = (tow - event_tow) / 30`` with ``event_tow`` from the LEG's
``leg_meta.json``.  A null-vs-null comparison has no event of its own, so pass
``--event-tow`` explicitly there (the event leg's tow, so both floors are read on
the same window).

Channels
  A IDENTITY   who fired, on what evidence, and WHICH BANDS WERE FREED —
               network_events.csv, network_if_slip.csv, mw_slip_detector.log,
               plus the T4 attribution-erase counts at the station and
               network-wide (event - null).  Invariant #4: counters, not the
               absence of crashes.
  B FRAME      the station's own states: receiver clock (network_rx_states.csv
               kind CLK), ZWD (network_zwd.csv), and the injected LOS's slant
               ionosphere (network_ionosphere.csv, iono_m = the CORRECTION x_r
               and iono_total_m = x_lin + x_r; §Amendment 4 — x_r alone is not
               absolute slant iono, which is why both are printed).
  C WIRE       products.txt per (sat, kind, signal), event - null: the injected
               satellite's CLK and per-signal PB, and the worst OTHER satellite
               (the blast radius).
  D CONSUMER   closed_loop_trace.csv ENU, event - null — de-biased BY
               CONSTRUCTION (the null carries the static bias), plus the
               consumer's own reset counts from rt_engine.log.
  E FLOOR      with --ref-null: the same B/C/D for (null - ref-null), i.e. what
               the knob costs on a QUIET leg with no event at all, plus each
               null's event census and withhold count.

BANDS FREED — where the identity comes from (formats read from source, and the
lines are cited so a format drift is findable):
  * ``T0_mw_detect``/``mw_wl_slip``  (src/core/slip/NetworkSlipQc.cpp:964)
    action = refloat | diagnostic | oracle_repair | oracle_keep
    detail = "<sig1>;<sig2>" [";band_scope=implicated;kept=<sig>"]
             [";src=evidence;ratio=<r>;n=<n>;int=known|unknown"]
    The MW row does NOT name the common-band keeps; they come from the epoch's
    ONE verdict row (below).  A band of the pair is freed iff the action frees
    (``refloat``), the band is not the §Z ``kept=`` pin, and the common-band
    verdict did not exonerate it (NetworkSlipQc.cpp:915-921).
  * ``T0_mw_detect``/``band_scope`` (NetworkSlipQc.cpp:906) action = keep,
    detail = "<sig1>;<sig2>;band_scope=implicated;kept=<sig>" — the second-pair
    keep that would otherwise be silent.
  * ``T1_if_slip``/``if_slip`` (NetworkSlipQc.cpp:1493) action = refloat |
    diagnostic | oracle_*, detail = "<sig1>;<sig2>"[";<oracle detail>"]
    [";cb_kept=<sigA>[+<sigB>]"] — here the common-band keeps ARE named
    explicitly (NetworkSlipQc.cpp:1515).
  * ``T0_band_scope``/``common_band`` (src/core/slip/NetworkCommonBandScope.cpp
    :477-503) — the epoch's ONE verdict for a (station, satellite):
      action "keep" : ["rule=lli_epoch;"] "implicated=<set>" [";exonerated=<set>"]
                      [";narrowed=0|1"] ";basis=<pair+pair...>"
      action "all"  : ["rule=lli_epoch;"] "reason=inconsistent|anchor_colocate|
                      no_exoneration;implicated=<set>;basis=<...>"
    Sets are rendered with ``+`` separators (a comma would break the one-field
    CSV detail — NetworkCommonBandScope.cpp:52).

mw_slip_detector.log is a TEXT spdlog file, not a CSV, and its per-detection line
carries no station (station_label_ appears only on the DEBUG ``MWDEC`` line,
silent at the shipped level).  The LANE is recovered from ``lambda_wl``:
GPS 0.862 m = (L1,L2), 0.751 m = (L1,L5), 5.861 m = (L2,L5).  ``bia=yes|no`` on
that line is the arc's ``bia_corrected`` flag (MwSlipDetector.cpp:2248-2261).
"""
import argparse
import csv
import json
import math
import os
import re
import sys
from collections import defaultdict, Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
from event_grade import read_products      # noqa: E402  (one products parser)

EPOCH_S = 30.0
K_FRAME = (1, 5, 10, 20, 45)               # §20 gate G-B reads +1/+10/+45; +5/+20 are free
SIG_RE = re.compile(r"^[LCS]\d[A-Z]$")
_TOWPFX = re.compile(r"^\[\s*(\d+),\s*([0-9.]+)")

# GPS wide-lane wavelengths, metres — the lane discriminator for the MW log.
_LANES = ((0.8619, "L1C/L2W"), (0.7514, "L1C/L5Q"), (5.8611, "L2W/L5Q"))


def lane_of_lambda(lam):
    for v, name in _LANES:
        if abs(lam - v) < 0.02:
            return name
    return f"lam={lam:.3f}"


# ------------------------------------------------------------------ utilities
def fnum(v, d=4, w=10):
    if v is None or (isinstance(v, float) and not math.isfinite(v)):
        return "n/a".rjust(w)
    return f"{v:{w}.{d}f}"


def rel_ep(tow, t0):
    return int(round((tow - t0) / EPOCH_S))


def leg_meta(legdir):
    p = os.path.join(legdir, "leg_meta.json")
    if not os.path.isfile(p):
        return {}
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def read_csv_series(path, t0, keyfn, valfn):
    """{k: value} from a CSV with a `tow` column; keyfn(row) selects, valfn reads."""
    out = {}
    if not os.path.isfile(path):
        return out, f"n/a ({os.path.basename(path)} absent)"
    try:
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            for row in csv.DictReader(f):
                if not keyfn(row):
                    continue
                try:
                    k = rel_ep(float(row["tow"]), t0)
                except (KeyError, TypeError, ValueError):
                    continue
                v = valfn(row)
                if v is not None:
                    out[k] = v
    except OSError as e:
        return {}, f"n/a ({e})"
    return out, None


def f_or_none(row, col):
    try:
        return float(row[col])
    except (KeyError, TypeError, ValueError):
        return None


def diff_series(a, b):
    return {k: a[k] - b[k] for k in a if k in b}


def max_abs(d, lo, hi):
    v = [(abs(x), k) for k, x in d.items() if lo <= k <= hi]
    return max(v) if v else (None, None)


def mean_abs(d, lo, hi):
    v = [abs(x) for k, x in d.items() if lo <= k <= hi]
    return (sum(v) / len(v)) if v else None


# --------------------------------------------------------------- events / A
def read_events(legdir):
    p = os.path.join(legdir, "logs", "network_events.csv")
    if not os.path.isfile(p):
        return None
    rows = []
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def parse_detail(detail):
    """-> dict with pair, kept, cb_kept, implicated, exonerated, basis, reason, rule, src."""
    out = {"pair": [], "kept": set(), "cb_kept": set(), "implicated": set(),
           "exonerated": set(), "basis": "", "reason": "", "rule": "", "src": ""}
    for tok in (detail or "").split(";"):
        tok = tok.strip()
        if not tok:
            continue
        if "=" not in tok:
            if SIG_RE.match(tok):
                out["pair"].append(tok)
            continue
        k, _, v = tok.partition("=")
        if k == "kept":
            out["kept"].update(x for x in v.split("+") if x)
        elif k == "cb_kept":
            out["cb_kept"].update(x for x in v.split("+") if x)
        elif k == "implicated":
            out["implicated"].update(x for x in v.split("+") if x and x != "-")
        elif k == "exonerated":
            out["exonerated"].update(x for x in v.split("+") if x and x != "-")
        elif k in ("basis", "reason", "rule", "src"):
            out[k] = v
    return out


FREE_ACTIONS = ("refloat", "oracle_repair")


def bands_freed(rows, sta, sat, t0, lo, hi):
    """Union of bands actually freed for (sta, sat) in [lo, hi], + the verdicts."""
    freed, verdicts, drains = set(), [], []
    # the epoch's ONE common-band verdict per (sta, sat), keyed by k
    exon_at = {}
    for r in rows or []:
        if r.get("station") != sta or r.get("sat") != sat:
            continue
        try:
            k = rel_ep(float(r["tow"]), t0)
        except (KeyError, ValueError):
            continue
        if not (lo <= k <= hi):
            continue
        if r.get("tier") == "T0_band_scope" and r.get("event") == "common_band":
            d = parse_detail(r.get("detail", ""))
            verdicts.append((k, r.get("action", ""), d))
            if r.get("action") == "keep":
                exon_at[k] = d["exonerated"]
    for r in rows or []:
        if r.get("station") != sta or r.get("sat") != sat:
            continue
        try:
            k = rel_ep(float(r["tow"]), t0)
        except (KeyError, ValueError):
            continue
        if not (lo <= k <= hi):
            continue
        tier, ev, act = r.get("tier", ""), r.get("event", ""), r.get("action", "")
        if not ((tier == "T0_mw_detect" and ev in ("mw_wl_slip", "band_scope"))
                or (tier == "T1_if_slip" and ev == "if_slip")):
            continue
        d = parse_detail(r.get("detail", ""))
        pair = set(d["pair"])
        this = set()
        if act in FREE_ACTIONS and pair:
            this = pair - d["kept"] - d["cb_kept"] - exon_at.get(k, set())
        drains.append((k, tier, ev, act, sorted(pair), sorted(d["kept"] | d["cb_kept"]),
                       sorted(this), d["src"]))
        freed |= this
    return freed, verdicts, drains


def t4_counts(rows, sta, t0, lo, hi):
    """(at station, network-wide) T4_attrib/attrib_arc rows in [lo, hi]."""
    n_sta = n_net = 0
    for r in rows or []:
        if r.get("tier") != "T4_attrib" or r.get("event") != "attrib_arc":
            continue
        try:
            k = rel_ep(float(r["tow"]), t0)
        except (KeyError, ValueError):
            continue
        if not (lo <= k <= hi):
            continue
        n_net += 1
        if r.get("station") == sta:
            n_sta += 1
    return n_sta, n_net


def if_slip_rows(legdir, sta, sat, t0, lo, hi):
    p = os.path.join(legdir, "logs", "network_if_slip.csv")
    if not os.path.isfile(p):
        return None
    out = []
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r.get("station") != sta or r.get("sat") != sat:
                continue
            try:
                k = rel_ep(float(r["tow"]), t0)
            except (KeyError, ValueError):
                continue
            if lo <= k <= hi:
                out.append((k, r.get("sig1", ""), r.get("sig2", ""),
                            r.get("abs_dev_m", ""), r.get("thr_m", ""),
                            r.get("flagged", ""), r.get("carry_warm", "")))
    return out


_MW_DET = re.compile(
    r"MW slip detected for (\S+) \(([^)]*)\): \|dev\|=([-\d.]+) m \(([-\d.]+) WL\) "
    r"> thresh=([-\d.]+) m \(([-\d.]+) WL\).*?arc_n=(\d+), lambda_wl=([\d.]+) m, "
    r"bia=(yes|no), locked=(\w+), delta_wl=(.*)$")
_MW_KEY = re.compile(r"\b([GREC]\d\d)_([LCS]\d[A-Z])_([LCS]\d[A-Z])\b")
_MW_STA = re.compile(r"sta=(\S+)")


def mw_log_rows(path):
    """Parse mw_slip_detector.log -> list of dicts (tow, sat, lane, ...).

    Two row families carry what §20 gate G-F asks for:
      DET  'MW slip detected for G07 (GPS): ... lambda_wl=0.862 m, bia=no, ...'
           is_slip = 1 by construction; lane from lambda_wl; bia_corrected from bia=.
      KEY  any line naming a '<sat>_<sig1>_<sig2>' arc key (WL locked / init
           suspicious / scheduling reset) — lane EXPLICIT, no bia field except on
           'MW init suspicious ... BIA-corrected MW=' which is bia_corrected by
           its own wording.
    """
    if not os.path.isfile(path):
        return None
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = _TOWPFX.match(ln)
            tow = float(m.group(2)) if m else None
            body = ln.rstrip("\n")
            d = _MW_DET.search(body)
            if d:
                lam = float(d.group(8))
                out.append({"kind": "DET", "tow": tow, "sat": d.group(1),
                            "lane": lane_of_lambda(lam), "lambda_wl": lam,
                            "dev_wl": float(d.group(4)), "thr_wl": float(d.group(6)),
                            "ratio": (float(d.group(4)) / float(d.group(6))
                                      if float(d.group(6)) else None),
                            "arc_n": int(d.group(7)), "bia": d.group(9),
                            "locked": d.group(10), "delta_wl": d.group(11),
                            "is_slip": 1, "line": body})
                continue
            k = _MW_KEY.search(body)
            if k:
                bia = ("yes" if "BIA-corrected" in body else "?")
                out.append({"kind": "KEY", "tow": tow, "sat": k.group(1),
                            "lane": f"{k.group(2)}/{k.group(3)}", "bia": bia,
                            "is_slip": 1 if "suspicious" in body else 0,
                            "sta": (_MW_STA.search(body).group(1)
                                    if _MW_STA.search(body) else ""),
                            "line": body})
    return out


# ---------------------------------------------------------------- channels B
def frame_series(legdir, sta, sat, t0):
    clk, e1 = read_csv_series(
        os.path.join(legdir, "logs", "network_rx_states.csv"), t0,
        lambda r: r.get("station") == sta and r.get("kind") == "CLK",
        lambda r: f_or_none(r, "value_m"))
    zwd, e2 = read_csv_series(
        os.path.join(legdir, "logs", "network_zwd.csv"), t0,
        lambda r: r.get("station") == sta, lambda r: f_or_none(r, "zwd_m"))
    ion, e3 = read_csv_series(
        os.path.join(legdir, "logs", "network_ionosphere.csv"), t0,
        lambda r: r.get("station") == sta and r.get("sat") == sat,
        lambda r: f_or_none(r, "iono_m"))
    iot, _ = read_csv_series(
        os.path.join(legdir, "logs", "network_ionosphere.csv"), t0,
        lambda r: r.get("station") == sta and r.get("sat") == sat,
        lambda r: f_or_none(r, "iono_total_m"))
    return {"clk": (clk, e1), "zwd": (zwd, e2), "iono": (ion, e3), "iono_tot": (iot, e3)}


def consumer_series(legdir, t0):
    p = os.path.join(legdir, "logs", "closed_loop_trace.csv")
    out = {}
    if not os.path.isfile(p):
        return out, f"n/a ({os.path.basename(p)} absent)"
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                k = rel_ep(float(r["tow"]), t0)
            except (KeyError, ValueError):
                continue
            e, n, u = (f_or_none(r, "east_m"), f_or_none(r, "north_m"),
                       f_or_none(r, "up_m"))
            if None not in (e, n, u):
                out[k] = (e, n, u)
    return out, None


_RT_KIND = (("pb_reset", "SSR phase-bias discontinuity"),
            ("cb_disc", "SSR code-bias discontinuity"),
            ("clk_disc", "SSR clock discontinuity"))


def rt_events(legdir, t0, lo, hi):
    p = os.path.join(legdir, "logs", "rt_engine.log")
    if not os.path.isfile(p):
        return None
    out = Counter()
    with open(p, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = _TOWPFX.match(ln)
            if not m:
                continue
            k = rel_ep(float(m.group(2)), t0)
            if not (lo <= k <= hi):
                continue
            for kind, pat in _RT_KIND:
                if pat in ln:
                    out[kind] += 1
                    break
    return out


def withheld(legdir):
    p = os.path.join(legdir, "logs", "network_run_summary.csv")
    if not os.path.isfile(p):
        return None
    try:
        with open(p, newline="", encoding="utf-8", errors="replace") as f:
            rows = list(csv.DictReader(f))
        if not rows:
            return None
        r = rows[-1]
        return {c: r.get(c) for c in ("n_synced", "n_emitted", "n_gate_withheld",
                                      "n_sigma0_withheld", "ms_per_epoch")}
    except OSError:
        return None


# ------------------------------------------------------------------ printing
def hdr(t):
    print("\n" + t)
    print("-" * max(len(t), 60))


def print_frame_table(title, legdir, nulldir, sta, sat, t0, post):
    hdr(title)
    A = frame_series(legdir, sta, sat, t0)
    B = frame_series(nulldir, sta, sat, t0)
    res = {}
    print(f"{'channel':<12}" + "".join(f"{('k=+%d' % k):>11}" for k in K_FRAME)
          + f"{'max|d|[0,45]':>14}{'at k':>6}")
    for name in ("clk", "zwd", "iono", "iono_tot"):
        a, ea = A[name]
        b, _ = B[name]
        if ea or not a or not b:
            print(f"{name:<12}" + "".join(f"{'n/a':>11}" for _ in K_FRAME)
                  + f"{'n/a':>14}{'-':>6}"
                  + (f"   [{ea}]" if ea else "   [no overlapping rows]"))
            res[name] = None
            continue
        d = diff_series(a, b)
        mx, at = max_abs(d, 0, post)
        print(f"{name:<12}" + "".join(fnum(d.get(k), 4, 11) for k in K_FRAME)
              + fnum(mx, 4, 14) + f"{('+%d' % at) if at is not None else '-':>6}")
        res[name] = {"at": {k: d.get(k) for k in K_FRAME}, "max": mx, "at_k": at}
    return res


def print_wire_table(title, legdir, nulldir, sat, t0, post):
    hdr(title)
    res = {"clk": None, "pb": {}, "other": None}
    pa = os.path.join(legdir, "products.txt")
    pb_ = os.path.join(nulldir, "products.txt")
    if not (os.path.isfile(pa) and os.path.isfile(pb_)):
        print("n/a (products.txt missing in leg or null)")
        return res
    A, B = read_products(pa), read_products(pb_)
    tows = sorted(set(A) & set(B))
    tows = [t for t in tows if 0 <= rel_ep(t, t0) <= post]
    if not tows:
        print("n/a (no overlapping emitted epochs in [0,+%d])" % post)
        return res
    dmax = defaultdict(float)
    dat = {}
    for t in tows:
        k = rel_ep(t, t0)
        for key, v in A[t].items():
            if key in B[t]:
                dv = abs(v - B[t][key])
                if dv > dmax[key]:
                    dmax[key] = dv
                    dat[key] = k
    inj_c = [(k, v) for k, v in dmax.items() if k[0] == "C" and k[1] == sat]
    inj_p = sorted((k, v) for k, v in dmax.items() if k[0] == "P" and k[1] == sat)
    oth = [(v, k) for k, v in dmax.items() if k[1] != sat]
    print(f"{'quantity':<28}{'max|d| m':>12}{'at k':>7}   (epochs compared: %d)" % len(tows))
    if inj_c:
        res["clk"] = inj_c[0][1]
        print(f"{sat+' CLK':<28}{fnum(inj_c[0][1], 5, 12)}{('+%d' % dat[inj_c[0][0]]):>7}")
    else:
        print(f"{sat+' CLK':<28}{'n/a':>12}")
    for key, v in inj_p:
        res["pb"][key[2]] = v
        print(f"{sat+' PB '+key[2]:<28}{fnum(v, 5, 12)}{('+%d' % dat[key]):>7}")
    if not inj_p:
        print(f"{sat+' PB':<28}{'n/a':>12}")
    if oth:
        v, key = max(oth)
        res["other"] = {"v": v, "key": "/".join(key), "at": dat[key]}
        print(f"{'WORST OTHER: ' + '/'.join(key):<28}{fnum(v, 5, 12)}{('+%d' % dat[key]):>7}")
        top = sorted(oth, reverse=True)[:5]
        print("   next: " + ", ".join(f"{'/'.join(k)}={v:.5f}" for v, k in top[1:]))
    return res


def print_consumer_table(title, legdir, nulldir, t0, post):
    hdr(title)
    res = {}
    A, ea = consumer_series(legdir, t0)
    B, eb = consumer_series(nulldir, t0)
    if ea or eb or not A or not B:
        print(f"n/a ({ea or eb or 'no overlapping rows'})")
        return res
    d3, comp = {}, {}
    for k in A:
        if k not in B or not (0 <= k <= post):
            continue
        de = A[k][0] - B[k][0]
        dn = A[k][1] - B[k][1]
        du = A[k][2] - B[k][2]
        comp[k] = (de, dn, du)
        d3[k] = math.sqrt(de * de + dn * dn + du * du)
    if not d3:
        print("n/a (no common epochs)")
        return res
    mx = max((v, k) for k, v in d3.items())
    seg = [v for k, v in d3.items() if 30 <= k <= 50]
    res = {"max3d": mx[0], "max3d_at": mx[1],
           "mean3d_30_50": (sum(seg) / len(seg)) if seg else None,
           "at45": comp.get(45), "n": len(d3)}
    print(f"{'quantity':<28}{'value':>12}{'at k':>7}")
    print(f"{'max |d3D| [0,+%d]' % post:<28}{fnum(mx[0], 5, 12)}{('+%d' % mx[1]):>7}")
    print(f"{'mean |d3D| [+30,+50]':<28}"
          f"{fnum(res['mean3d_30_50'], 5, 12)}{'-':>7}   (n={len(seg)})")
    c = comp.get(45)
    print(f"{'dE/dN/dU at k=+45':<28}"
          + (f"{c[0]:+.5f} / {c[1]:+.5f} / {c[2]:+.5f}" if c else "n/a"))
    print(f"{'epochs compared':<28}{len(d3):>12}")
    ea_, eb_ = rt_events(legdir, t0, 0, post), rt_events(nulldir, t0, 0, post)
    if ea_ is None or eb_ is None:
        print(f"{'rt_engine resets':<28}{'n/a (rt_engine.log absent)':>12}")
    else:
        res["rt"] = {k: ea_[k] - eb_[k] for k, _ in _RT_KIND}
        print(f"{'rt_engine resets (ev-null)':<28}"
              + ", ".join(f"{k}={ea_[k]}-{eb_[k]}={ea_[k]-eb_[k]}" for k, _ in _RT_KIND))
    return res


# ---------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("legdir")
    ap.add_argument("--null", required=True)
    ap.add_argument("--ref-null", default=None,
                    help="the OFF null: prints (null - ref-null) = the knob's own "
                         "quiet-leg FLOOR on the same channels")
    ap.add_argument("--station", required=True)
    ap.add_argument("--sat", required=True)
    ap.add_argument("--pre", type=int, default=5)
    ap.add_argument("--post", type=int, default=50)
    ap.add_argument("--event-tow", type=float, default=None,
                    help="override the leg's event tow (required for null-vs-null)")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    sta, sat, post = a.station, a.sat, a.post
    meta = leg_meta(a.legdir)
    t0 = a.event_tow if a.event_tow is not None else meta.get("event_tow")
    if t0 is None:
        raise SystemExit("FATAL: no event_tow in leg_meta.json — pass --event-tow "
                         "(a null leg has no event of its own)")
    t0 = float(t0)
    print("=" * 78)
    print(f"FIELD EVENT GRADE   leg {os.path.basename(a.legdir)}  "
          f"vs null {os.path.basename(a.null)}"
          + (f"  ref-null {os.path.basename(a.ref_null)}" if a.ref_null else ""))
    print(f"  station {sta}   sat {sat}   event_tow {t0!r}   window [-{a.pre}, +{post}]")
    ev = meta.get("event")
    print(f"  injected  {json.dumps(ev) if ev else '(none / override)'}")
    print(f"  extra     {json.dumps(meta.get('extra', {}))}")
    ws = withheld(a.legdir)
    print(f"  run       {json.dumps(ws) if ws else 'n/a (no network_run_summary.csv)'}")
    print("=" * 78)

    out = {"leg": a.legdir, "null": a.null, "station": sta, "sat": sat,
           "event_tow": t0, "event": ev}

    # ---------------------------------------------------------------- A
    hdr("A. IDENTITY — network_events.csv rows at (%s, %s), k in [-1,+5]" % (sta, sat))
    rows_l, rows_n = read_events(a.legdir), read_events(a.null)
    for tag, rows in (("LEG ", rows_l), ("NULL", rows_n)):
        if rows is None:
            print(f"{tag}  n/a (network_events.csv absent)")
            continue
        n = 0
        for r in rows:
            if r.get("station") != sta or r.get("sat") != sat:
                continue
            try:
                k = rel_ep(float(r["tow"]), t0)
            except (KeyError, ValueError):
                continue
            if -1 <= k <= 5:
                n += 1
                print(f"{tag} k={k:+3d}  {r.get('tier',''):<14} {r.get('event',''):<14} "
                      f"{r.get('action',''):<13} stat={r.get('statistic','')!s:>10} "
                      f"thr={r.get('threshold','')!s:>10}  {r.get('detail','')}")
        if n == 0:
            print(f"{tag}  (no rows at this station/sat in k in [-1,+5])")

    hdr("A. IDENTITY — bands freed, and the common-band verdicts")
    fr_l, vd_l, dr_l = bands_freed(rows_l, sta, sat, t0, -1, 5)
    fr_n, vd_n, dr_n = bands_freed(rows_n, sta, sat, t0, -1, 5)
    for tag, fr, vd, dr in (("LEG ", fr_l, vd_l, dr_l), ("NULL", fr_n, vd_n, dr_n)):
        print(f"{tag} BANDS FREED: {sorted(fr) if fr else '(none)'}")
        for k, tier, evn, act, pair, kept, this, src in dr:
            print(f"{tag}   drain k={k:+3d} {tier}/{evn} action={act} pair={pair} "
                  f"kept={kept} -> freed={this}" + (f"  src={src}" if src else ""))
        for k, act, d in vd:
            print(f"{tag}   verdict k={k:+3d} action={act} rule={d['rule'] or '-'} "
                  f"reason={d['reason'] or '-'} implicated={sorted(d['implicated'])} "
                  f"exonerated={sorted(d['exonerated'])} basis={d['basis']}")
        if not dr and not vd:
            print(f"{tag}   (no drain or verdict rows)")
    out["bands_freed"] = sorted(fr_l)
    out["bands_freed_null"] = sorted(fr_n)

    hdr("A. IDENTITY — network_if_slip.csv rows at (%s, %s), k in [-1,+1]" % (sta, sat))
    for tag, d in (("LEG ", a.legdir), ("NULL", a.null)):
        rr = if_slip_rows(d, sta, sat, t0, -1, 1)
        if rr is None:
            print(f"{tag}  n/a (network_if_slip.csv absent — needs trace_slip_events)")
            continue
        if not rr:
            print(f"{tag}  (no rows)")
        for k, s1, s2, dev, thr, fl, cw in rr:
            print(f"{tag} k={k:+3d}  {s1}/{s2:<5} abs_dev_m={dev:>10} thr_m={thr:>10} "
                  f"flagged={fl} carry_warm={cw}")

    hdr("A. IDENTITY — mw_slip_detector.log at (%s) near the event" % sat)
    for tag, d in (("LEG ", a.legdir), ("NULL", a.null)):
        mw = mw_log_rows(os.path.join(d, "logs", "mw_slip_detector.log"))
        if mw is None:
            print(f"{tag}  n/a (mw_slip_detector.log absent)")
            continue
        n = 0
        for r in mw:
            if r["sat"] != sat or r["tow"] is None:
                continue
            k = rel_ep(r["tow"], t0)
            if not (-1 <= k <= 5):
                continue
            n += 1
            if r["kind"] == "DET":
                print(f"{tag} k={k:+3d}  DET lane={r['lane']:<8} "
                      f"dev/thr={r['ratio']:.2f} (dev {r['dev_wl']:+.2f} WL, thr "
                      f"{r['thr_wl']:.2f} WL) arc_n={r['arc_n']} bia={r['bia']} "
                      f"locked={r['locked']} delta_wl={r['delta_wl']}")
            else:
                print(f"{tag} k={k:+3d}  KEY lane={r['lane']:<8} bia={r['bia']}  "
                      f"{r['line'][:150]}")
        if n == 0:
            print(f"{tag}  (no {sat} rows in k in [-1,+5]; the log is station-blind "
                  f"on the DET line by design)")

    hdr("A. IDENTITY — T4_attrib/attrib_arc counts, k in [0,+45]")
    ts_l, tn_l = t4_counts(rows_l, sta, t0, 0, 45)
    ts_n, tn_n = t4_counts(rows_n, sta, t0, 0, 45)
    print(f"{'':<10}{'at ' + sta:>18}{'network-wide':>16}")
    print(f"{'LEG':<10}{ts_l:>18}{tn_l:>16}")
    print(f"{'NULL':<10}{ts_n:>18}{tn_n:>16}")
    print(f"{'EV-NULL':<10}{ts_l - ts_n:>+18}{tn_l - tn_n:>+16}")
    out["t4_sta"], out["t4_net"] = ts_l - ts_n, tn_l - tn_n

    # ------------------------------------------------------------ B / C / D
    out["frame"] = print_frame_table(
        "B. STATION FRAME — event MINUS null (m), station %s / sat %s" % (sta, sat),
        a.legdir, a.null, sta, sat, t0, post)
    out["wire"] = print_wire_table(
        "C. WIRE — products.txt event MINUS null", a.legdir, a.null, sat, t0, post)
    out["consumer"] = print_consumer_table(
        "D. CONSUMER — closed_loop_trace.csv ENU event MINUS null (de-biased)",
        a.legdir, a.null, t0, post)

    # ------------------------------------------------------------------- E
    if a.ref_null:
        print("\n" + "=" * 78)
        print("E. FLOOR — (this posture's NULL) MINUS (the OFF null): the knob's own")
        print("   quiet-leg effect, same channels and window.  Nothing here is an event.")
        print("=" * 78)
        out["floor"] = {
            "frame": print_frame_table(
                "E-B. FLOOR station frame — null MINUS ref-null (m)",
                a.null, a.ref_null, sta, sat, t0, post),
            "wire": print_wire_table(
                "E-C. FLOOR wire — null MINUS ref-null", a.null, a.ref_null, sat, t0, post),
            "consumer": print_consumer_table(
                "E-D. FLOOR consumer — null MINUS ref-null", a.null, a.ref_null, t0, post),
        }
        hdr("E-A. FLOOR event census over the leg (tier/event counts) + withhold")
        for tag, d in (("NULL    ", a.null), ("REF-NULL", a.ref_null)):
            rows = read_events(d)
            if rows is None:
                print(f"{tag}  n/a (network_events.csv absent)")
                continue
            c = Counter((r.get("tier", ""), r.get("event", "")) for r in rows)
            print(f"{tag}  {withheld(d)}")
            for (tier, evn), n in sorted(c.items(), key=lambda x: -x[1])[:14]:
                print(f"{tag}    {tier:<16} {evn:<18} {n:>6}")

    # -------------------------------------------------------------- summary
    fr = out["frame"] or {}
    wi = out["wire"] or {}
    co = out["consumer"] or {}

    def g(ch, key="at", k=45):
        v = fr.get(ch)
        return (v[key].get(k) if v and key == "at" else (v or {}).get(key)) if v else None
    pbmax = max(wi.get("pb", {}).values()) if wi.get("pb") else None
    oth = wi.get("other") or {}
    line = (f"SUMMARY {os.path.basename(a.legdir)} "
            f"A:{'+'.join(sorted(fr_l)) if fr_l else 'none'} "
            f"T4sta:{ts_l - ts_n:+d} T4net:{tn_l - tn_n:+d} "
            f"B:{fnum(g('clk'),4,1).strip()}/{fnum(g('zwd'),4,1).strip()}/"
            f"{fnum(g('iono'),4,1).strip()}@45 "
            f"C:{fnum(wi.get('clk'),4,1).strip()}/{fnum(pbmax,4,1).strip()}({sat}) "
            f"other({oth.get('key','n/a')}):{fnum(oth.get('v'),4,1).strip()} "
            f"D:{fnum(co.get('mean3d_30_50'),4,1).strip()}/"
            f"{fnum(co.get('max3d'),4,1).strip()}")
    print("\n" + line)
    out["summary"] = line
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2, default=str)
        print(f"json -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
