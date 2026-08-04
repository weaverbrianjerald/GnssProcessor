#!/usr/bin/env python3
"""Build the validation arm configs for docs/SPEC_measurement_admission.md.

Every arm derives from ONE base so the estimation configuration is identical
across arms and only this card's knobs differ.  The spec's §7 interaction
discipline is enforced here: **no arm carries a deweight-release-hygiene, prefit
fuse, or redundancy-escalation knob**, because each of those is either
solve-affecting or a competing responder for the same event, and an arm carrying
two responders cannot attribute what it sees to either.

Two families:

``smoke``  -- gate 1.  Cold start, 10 epochs, 63 stations.  Three arms:

  ``off``     nothing armed -- the same-build control.
  ``census``  ``adm_trace`` only.  MEASURE-ONLY: its ``network_solution.csv``
              must be byte-identical to ``off``.  That comparison is the whole
              point of the arm -- it is what makes the census legal to leave
              armed on any leg.
  ``screen``  ``adm_screen`` + ``adm_trace``.  The one solve-affecting arm.

``leg``    -- gates 2/3/4.  Replays from the FT63 24 h archive.  ``birth`` is the
             known-answer gate (``snap_19`` + offset 18.1125 contains BOTH BSHM
             arc births); ``break`` runs through 19:26 to see whether the era-2
             discharge survives with the two channels never admitted; the three
             ``b300`` / ``b1k`` / ``b3k`` arms are the §7 gate-4 bound sweep,
             whose claim is that the reject SET is identical across two decades
             of threshold.

NOTE the snapshot index is NOT the hour: the archive's ``snap_N`` is written
~30 s before offset ``(N-1).1125``, which is why the main leg pairs ``snap_20``
with 19.1125.  Pairing ``snap_18`` with 18.1125 makes the snapshot 3690 s stale
and, with ``state_snapshot_require`` on, hard-aborts the leg (measured, and
recorded in the prefit-census card's §17).

Each arm gets its OWN run directory and its own copy of the seed snapshot: the
producer OVERWRITES ``state_snapshot_path`` at its first hour boundary, so
sharing one file between arms would silently re-seed the later arms from the
earlier arm's output.

Usage::

    python scripts/adm_make_arms.py                  # smoke arms (gate 1)
    python scripts/adm_make_arms.py --family leg     # replay arms (gates 2-4)
    python scripts/adm_make_arms.py --list
"""
from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SMOKE_BASE = ROOT / "config" / "test_suite" / "_ft63_dwr_smoke.json"
LEG_BASE = ROOT / "config" / "test_suite" / "_ft63_leg19_base.json"
SNAP_DIR = ROOT / "runs" / "ft63_24h_allon" / "snap_archive"

# Knobs stripped from EVERY arm (spec §7 interaction discipline).  A competing
# responder for the same event makes the arm unattributable; a second
# solve-affecting knob makes the byte-identity claim meaningless.
STRIP = (
    "iono_dw_release_reseed",
    "iono_dw_park_bound_tecu",
    "iono_dw_release_validate_k",
    "iono_dw_admit_validate_k",
    "prefit_fuse_sigma",
    "prefit_fuse_classes",
    "redundancy_escalate_sigma",
    "redundancy_census_stride",
    "rhs_attrib_tow",
    "rhs_attrib_n_ep",
)

# (arm tag, network-knob overrides).  ``None`` DELETES an inherited key.
SMOKE_ARMS = [
    ("off", {}),
    ("census", {"adm_trace": True}),
    ("screen", {"adm_screen": True, "adm_trace": True}),
    # INC-4 on a COLD 10-epoch window, where nothing has had time to leave the
    # screened set and come back.  The claim is INERTNESS: a run with zero
    # re-entries is a run in which the vet has nothing to do, so it must be
    # byte-identical to its control — and it fails loudly if the re-entry
    # predicate ever fires on a continuously-present channel.
    #
    # `rvsmoke` carries the SHIPPED defaults; `rvsmokelbl` pins
    # `adm_revet_bound_scale` to 1.0 for the label-only vet.  The tag `on`/no-suffix
    # always means "what a user gets", because an arm named for the default that
    # does not carry the default is exactly the trap this repo keeps paying for
    # (pcarry silently forced off, innov_refold parsed-inert).
    ("rvsmoke", {"adm_screen": True, "adm_trace": True, "adm_revet": True,
                 "adm_revet_bound_scale": 0.2, "trace_prefit_census": True}),
    ("rvsmokelbl", {"adm_screen": True, "adm_trace": True, "adm_revet": True,
                    "adm_revet_bound_scale": 1.0, "trace_prefit_census": True}),
    # …against a control that differs ONLY in `adm_revet`.  `screen` is NOT that
    # control: it lacks `trace_prefit_census`, whose own epoch-status columns would
    # show up in the diff and be read as an INC-4 effect.  The census has to be on
    # BOTH sides or the comparison is not about this increment.
    ("rvsmokeoff", {"adm_screen": True, "adm_trace": True,
                    "trace_prefit_census": True}),
]

# (arm tag, snapshot index, start_offset_hours, duration_s, overrides)
LEG_ARMS = [
    # Gate 2 -- the KNOWN-ANSWER birth gate.  snap_19 + 18.1125 contains both
    # BSHM arc births (G07 C1C at TOW 327030, G16 at 327210).
    ("birth", 19, 18.1125, 3600.0, {"adm_screen": True, "adm_trace": True}),
    ("birthoff", 19, 18.1125, 3600.0, {}),
    # Gate 3 -- the break gate + the free experiment at 329190.
    ("break", 20, 19.1125, 3600.0, {"adm_screen": True, "adm_trace": True}),
    ("breakoff", 20, 19.1125, 3600.0, {}),
    # Gate 4 -- the bound sweep.  The claim is that the reject SET is IDENTICAL
    # across two decades, which is what makes the separation falsifiable.
    ("b300", 19, 18.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "adm_bound_m": 300.0}),
    ("b1k", 19, 18.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "adm_bound_m": 1000.0}),
    ("b3k", 19, 18.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "adm_bound_m": 3000.0}),
]

# INC-4, the arc RE-ENTRY vet (card §10).  ON/OFF pairs on ONE shared snapshot --
# legal because the whole adm family is deliberately absent from the resume
# fingerprint (a forward-only admission policy re-references no carried quantity).
#
# The leg windows are chosen to CONTAIN the exposed re-entries measured on
# runs/ft63_24h_conv, i.e. the sub-class where the channel was re-admitted and only
# caught 2-8 epochs later.  Those are the epochs that cost a00; the twelve that
# fired at the resume epoch itself cost nothing and only carried the wrong label.
#
#   snap_04 (offset 3.1125 -> TOW 270405) contains BOTH
#       G13@BSHM  resume 273240 (gap 1) -> onset 273390, prefit flag 273330
#       G22@BSHM  resume 273450 (gap 9) -> onset 273690, prefit flag 273570
#   snap_14 (offset 13.1125 -> TOW 306405) contains
#       G16@BSHM  resume 307740 (gap 3) -> onset 307800, prefit flag 307770
#
# Every arm carries `trace_prefit_census` -- without the prefit walk there is no
# census flag to consult and the vet silently degrades to the |z| bound, which is
# exactly the configuration these re-entries survived.  It is trace-class and
# fingerprint-SAFE, so it is legal on both halves of a warm-resume pair.
REVET_ARMS = [
    # `lbl` = the LABEL-ONLY vet (`adm_revet_bound_scale` 1.0): the relabel plus the
    # census trigger, with no tighter bound in the way.  These are what §10.7 grades,
    # and they must keep existing — with the 200 m bound armed the census made 0
    # refusals of 616 consultations (§10.11), so the `lbl` arms are the ONLY place
    # the census trigger's contribution is still separately falsifiable.
    #
    # `on` = the SHIPPED defaults (0.2 -> 200 m), the §10.11 gate.  Named `on`
    # rather than `vb` deliberately: an arm named for the default has to carry the
    # default, or the next session grades the wrong configuration and never knows.
    ("rv04lbl", 4,  3.1125, 4500.0,
     {"adm_screen": True, "adm_trace": True, "adm_revet": True,
      "adm_revet_bound_scale": 1.0, "trace_prefit_census": True}),
    ("rv04off", 4,  3.1125, 4500.0,
     {"adm_screen": True, "adm_trace": True, "trace_prefit_census": True}),
    ("rv14lbl", 14, 13.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "adm_revet": True,
      "adm_revet_bound_scale": 1.0, "trace_prefit_census": True}),
    ("rv14off", 14, 13.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "trace_prefit_census": True}),
    # §10.11 — the tighter vet bound at its shipped default.
    #   rv14vb: G16 arrives at 670.3 m -> must be refused at 307740 (return epoch),
    #           which is the +19.2 TECU excursion the census cannot reach.
    #   rv04vb: G22 arrives at 347.6 m -> refused at 273450; G13 arrives at 81.5 m,
    #           BELOW the 200 m bound by design, so it must still fall to the
    #           census at 273360 exactly as in rv04on.  That contrast is the
    #           disjoint-coverage claim made falsifiable.
    ("rv14on",  14, 13.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "adm_revet": True,
      "adm_revet_bound_scale": 0.2, "trace_prefit_census": True}),
    ("rv04on",  4,  3.1125, 4500.0,
     {"adm_screen": True, "adm_trace": True, "adm_revet": True,
      "adm_revet_bound_scale": 0.2, "trace_prefit_census": True}),
    # The decisive posture arm.  The default (provisional-admit) vet cannot help
    # G16@BSHM: it returns INSIDE the bound at 307740 and the bound itself fires at
    # 307800, one epoch after the census flag becomes readable -- a tie, so the
    # exposure window 307740-307770 is folded either way.  `adm_revet_hold` is the
    # only shipped posture that refuses on arrival regardless, so it is the only one
    # that can answer "would the +19 TECU excursion have happened?".  Its COST is
    # what this arm measures alongside: it withholds every re-entry in the leg.
    ("rv14hold", 14, 13.1125, 3600.0,
     {"adm_screen": True, "adm_trace": True, "adm_revet": True,
      "adm_revet_hold": True, "trace_prefit_census": True}),
    # --- the §10.4 owed measurement -------------------------------------
    # MEASURE-ONLY (`adm_screen` off) with the bound walked down to 50 m, to
    # enumerate the |z| population at RE-ENTRY epochs -- the one distribution
    # nobody has measured and the one a tighter vet bound would have to respect.
    #
    # Legal to read as though it were a screened arm because the statistic is
    # STATE-FREE: `d = observed - modeled`, and `modeled` carries the geometric
    # range, the product satellite clock and the deterministic corrections with no
    # filter state in it.  So the |z| this arm reports is bit-for-bit the |z| the
    # armed arm computes -- and §9.11 already proved measure-only is solve
    # byte-identical, so it costs nothing to run.
    #
    # `adm_revet` is deliberately NOT armed (the loader would refuse it anyway
    # without the screen).  Re-entry attribution is done OFFLINE against the
    # `T0_gap arc_resume` event stream, which is the same evidence §10.1 used --
    # keeping the measurement independent of the mechanism it is meant to
    # parameterize.
    ("bz14", 14, 13.1125, 3600.0,
     {"adm_screen": False, "adm_trace": True, "adm_bound_m": 50.0,
      "trace_prefit_census": True}),
    ("bz04", 4, 3.1125, 4500.0,
     {"adm_screen": False, "adm_trace": True, "adm_bound_m": 50.0,
      "trace_prefit_census": True}),
]


def snapshot_for(idx: int) -> Path:
    hits = sorted(SNAP_DIR.glob(f"snap_{idx:02d}_*.bin"))
    if not hits:
        raise SystemExit(f"no snapshot snap_{idx:02d}_*.bin under {SNAP_DIR}")
    return hits[0]


def write_arm(base: dict, tag: str, overrides: dict, run_dir: Path,
              cfg_path: Path, seed: Path | None, offset: float | None,
              duration: float | None) -> None:
    cfg = json.loads(json.dumps(base))          # deep copy
    net = cfg["network"]
    for k in STRIP:
        net.pop(k, None)
    # Sinks: this card's whole claim is that it NAMES the channel, so every arm
    # carries the two sinks whether or not the base did.
    net["trace_events"] = True
    net["trace_epoch_status"] = True
    net["write_run_manifest"] = True
    if offset is not None:
        net["start_offset_hours"] = offset
        net["state_snapshot_path"] = str(run_dir / "state_snapshot.bin").replace("\\", "/")
        net["state_snapshot_require"] = True
    if duration is not None:
        net["max_duration_s"] = duration
        cfg.setdefault("processing", {})["max_duration_s"] = duration
    for k, v in overrides.items():
        if v is None:
            net.pop(k, None)
        else:
            net[k] = v
    cfg_path.write_text(json.dumps(cfg, indent=1) + "\n", encoding="utf-8")
    run_dir.mkdir(parents=True, exist_ok=True)
    if seed is not None:
        shutil.copyfile(seed, run_dir / "state_snapshot.bin")
    print(f"wrote {cfg_path.name}  -> {run_dir.name}"
          + (f"  (seed {seed.name})" if seed else "  (cold)"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--family", choices=("smoke", "leg", "revet"), default="smoke")
    ap.add_argument("--list", action="store_true", help="print the plan and exit")
    args = ap.parse_args()

    if args.family == "smoke":
        base = json.loads(SMOKE_BASE.read_text(encoding="utf-8"))
        for tag, overrides in SMOKE_ARMS:
            run_dir = ROOT / "runs" / f"ft63_adm_{tag}"
            cfg_path = ROOT / "config" / "test_suite" / f"_ft63_adm_{tag}.json"
            if args.list:
                print(f"{tag:8s} cold 10 ep -> {run_dir.name}")
                continue
            write_arm(base, tag, overrides, run_dir, cfg_path,
                      seed=None, offset=None, duration=300.0)
        return 0

    base = json.loads(LEG_BASE.read_text(encoding="utf-8"))
    for tag, snap_idx, offset, duration, overrides in (
            LEG_ARMS if args.family == "leg" else REVET_ARMS):
        run_dir = ROOT / "runs" / f"ft63_adm_{tag}"
        cfg_path = ROOT / "config" / "test_suite" / f"_ft63_adm_{tag}.json"
        if args.list:
            print(f"{tag:8s} snap_{snap_idx:02d} offset={offset} dur={duration}"
                  f" -> {run_dir.name}")
            continue
        write_arm(base, tag, overrides, run_dir, cfg_path,
                  seed=snapshot_for(snap_idx), offset=offset, duration=duration)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
