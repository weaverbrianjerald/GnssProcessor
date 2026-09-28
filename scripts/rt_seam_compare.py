#!/usr/bin/env python3
"""Masked byte-identity comparator for the RealTimeEngine seam gate.

    python scripts/rt_seam_compare.py pre inc2

Compares runs/rtseam_goldens/<golden>/ against runs/rtseam_goldens/<cand>/.

Two tiers, deliberately different in strictness:

  CSV artifacts    -> strict byte compare, no masking. Every per-epoch CSV the
                      engine writes carries GPS time only, so a difference here
                      is a real numerical or control-flow change. This is the gate.

  console/main log -> masked compare. Wall-clock-derived fields are
                      nondeterministic run to run and say nothing about the cut.

Mask specification (the RT analogue of the network INC-0 maskspec). Each entry
exists because the value is a function of when the run happened, not what it
computed:

  1. spdlog "[%E]" epoch prefix and any leading wall-clock timestamp.
  2. "is <N>s behind wall-clock GPS": lag_s, from nowContinuousGpsSeconds().
  3. "frame rx->process <N>s": steady_clock delta at the warn site.
  4. "held <N>/<N>s": obs/SSR hold age (pinned off in the arm configs, masked
     as a second layer of protection).
  5. "hold expired after <N>s".
  6. elapsed / ms-per-epoch / "<N> ms wall" timing fields.

Not masked, deliberately: satellite counts, reset/repair counts, AR ratios,
sigma0, discontinuity notices, gate decisions. Those are the signal.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
GOLD = ROOT / "runs" / "rtseam_goldens"

# --- maskspec ---------------------------------------------------------------
MASKS = [
    (re.compile(r"^\[[^\]]*\]\s*"),                       ""),   # 1 leading stamp
    (re.compile(r"is [-\d.]+s behind wall-clock GPS"),    "is <LAG>s behind wall-clock GPS"),
    (re.compile(r"frame rx.process [-\d.]+s"),            "frame rx->process <DT>s"),
    (re.compile(r"held [-\d.]+/[-\d.]+s"),                "held <T>/<T>s"),
    (re.compile(r"hold expired after [-\d.]+s"),          "hold expired after <T>s"),
    (re.compile(r"\b\d+(\.\d+)?\s*ms/epoch"),             "<MS>ms/epoch"),
    (re.compile(r"\b\d+(\.\d+)?\s*ms wall\b"),            "<MS> ms wall"),
    (re.compile(r"epoch-loop wall [\d.]+ ms"),            "epoch-loop wall <MS> ms"),
    (re.compile(r"\b(elapsed|took|in)\s+\d+(\.\d+)?\s*(s|ms)\b"), r"\1 <T>"),
]

# --- CSV column masks -------------------------------------------------------
# Ported from the network decomposition's frozen INC-0 maskspec, which already
# established these exact fields as wall-clock-derived. Masking is scoped by
# column (header name): blanket-masking a whole file would hide a real
# numerical regression sitting in a neighbouring column.
CSV_COL_MASKS = {
    "network_service_health.csv": {"ms_per_epoch_window", "last_emit_age_s"},
}

# JSON/JSONL keys whose values are wall-clock or build stamps (same source).
JSON_KEY_MASKS = {
    "network_run_manifest.json": ("written_utc", "compiled", "loop_ms", "ms_per_epoch"),
}

# Capture subdirectories that are not run arms.
NOT_ARMS = {"src"}


def masked_csv(path: pathlib.Path, cols: set):
    """Return CSV rows with the named columns blanked, by header position."""
    rows = path.read_text(errors="replace").splitlines()
    if not rows:
        return rows
    header = rows[0].split(",")
    idx = [i for i, h in enumerate(header) if h.strip() in cols]
    if not idx:
        return rows
    out = [rows[0]]
    for r in rows[1:]:
        f = r.split(",")
        for i in idx:
            if i < len(f):
                f[i] = "<MASKED>"
        out.append(",".join(f))
    return out


def masked_json(path: pathlib.Path, keys):
    """Blank the named keys textually: the manifest is JSONL, not one object."""
    text = path.read_text(errors="replace")
    for k in keys:
        text = re.sub(r'("' + re.escape(k) + r'"\s*:\s*)("?[^,}\]"]*"?)',
                      r'\1<MASKED>', text)
    return text.splitlines()


def mask(line: str) -> str:
    for pat, rep in MASKS:
        line = pat.sub(rep, line)
    return line


def read_masked(p: pathlib.Path):
    return [mask(l) for l in p.read_text(errors="replace").splitlines()]


def compare_arm(a: pathlib.Path, b: pathlib.Path, arm: str):
    """Return (verdict, [detail lines])."""
    detail, ok = [], True

    # A run that failed or produced nothing must fail, never silently pass as
    # "0 files differ" (a vacuous-green result).
    for side, d in (("golden", a), ("cand", b)):
        st = d / "arm_status.json"
        if not st.exists():
            return "NO-RUN", [f"  {side}: no arm_status.json (arm never captured)"]
        s = json.loads(st.read_text())
        if s.get("exit_code", 1) != 0:
            ok = False
            detail.append(f"  {side}: exit code {s['exit_code']}")
        csvs = [x for x in s.get("artifacts", []) if x.endswith(".csv")]
        if not csvs:
            ok = False
            detail.append(f"  {side}: produced ZERO csv artifacts -- nothing to certify")
    if not ok:
        return "FAIL", detail

    names = lambda d: {p.name for p in d.iterdir()
                       if p.is_file() and p.name not in ("arm_status.json",)}
    only_a, only_b = names(a) - names(b), names(b) - names(a)
    for n in sorted(only_a):
        ok = False; detail.append(f"  MISSING in candidate: {n}")
    for n in sorted(only_b):
        ok = False; detail.append(f"  EXTRA in candidate:   {n}")

    for n in sorted(names(a) & names(b)):
        pa, pb = a / n, b / n
        if n.endswith(".csv"):
            cols = CSV_COL_MASKS.get(n)
            if cols:
                la, lb = masked_csv(pa, cols), masked_csv(pb, cols)
                tag = f" (cols {'/'.join(sorted(cols))} masked)"
            else:
                la = pa.read_text(errors="replace").splitlines()
                lb = pb.read_text(errors="replace").splitlines()
                tag = ""
            if la == lb:
                detail.append(f"  IDENTICAL {n} ({pa.stat().st_size:,} B){tag}")
            else:
                ok = False
                first = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y),
                             min(len(la), len(lb)))
                detail.append(f"  DIFFERS   {n}{tag}: {len(la)} vs {len(lb)} lines, "
                              f"first diff line {first + 1}")
                if first < len(la) and first < len(lb):
                    detail.append(f"      golden: {la[first][:160]}")
                    detail.append(f"      cand  : {lb[first][:160]}")
        elif n in JSON_KEY_MASKS:
            keys = JSON_KEY_MASKS[n]
            la, lb = masked_json(pa, keys), masked_json(pb, keys)
            tag = f" (keys {'/'.join(keys)} masked)"
            if la == lb:
                detail.append(f"  IDENTICAL {n}{tag}")
            else:
                ok = False
                first = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y),
                             min(len(la), len(lb)))
                detail.append(f"  DIFFERS   {n}{tag}: first diff line {first + 1}")
                if first < len(la) and first < len(lb):
                    detail.append(f"      golden: {la[first][:200]}")
                    detail.append(f"      cand  : {lb[first][:200]}")
        else:
            ma, mb = read_masked(pa), read_masked(pb)
            if ma == mb:
                detail.append(f"  IDENTICAL {n} (masked, {len(ma)} lines)")
            else:
                ok = False
                first = next((i for i, (x, y) in enumerate(zip(ma, mb)) if x != y),
                             min(len(ma), len(mb)))
                detail.append(f"  DIFFERS   {n} (masked): {len(ma)} vs {len(mb)} lines, "
                              f"first diff line {first + 1}")
                if first < len(ma) and first < len(mb):
                    detail.append(f"      golden: {ma[first][:160]}")
                    detail.append(f"      cand  : {mb[first][:160]}")
    return ("PASS" if ok else "FAIL"), detail


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    gold, cand = GOLD / sys.argv[1], GOLD / sys.argv[2]
    for d in (gold, cand):
        if not d.is_dir():
            sys.exit(f"missing capture: {d}")

    arms = sorted(({p.name for p in gold.iterdir() if p.is_dir()} |
                   {p.name for p in cand.iterdir() if p.is_dir()}) - NOT_ARMS)
    verdicts, worst = {}, "PASS"
    for arm in arms:
        v, detail = compare_arm(gold / arm, cand / arm, arm)
        verdicts[arm] = v
        print(f"\n=== arm {arm}: {v} ===")
        for line in detail:
            print(line)
        if v != "PASS":
            worst = "FAIL"

    print("\n" + "=" * 60)
    for arm, v in verdicts.items():
        print(f"  {arm:12s} {v}")
    print(f"  OVERALL      {worst}")
    sys.exit(0 if worst == "PASS" else 1)


if __name__ == "__main__":
    main()
