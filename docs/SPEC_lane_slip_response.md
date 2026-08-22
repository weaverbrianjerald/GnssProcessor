# SPEC — the lane-integer slip cost is RESPONSE-inflicted (decomposition + fix ladder)

**Status: measured 2026-08-21/22 on `runs/camp60wu_f1`; the F3 hypothesis arm is
the open validation.  Follow-up to `SPEC_slip_relevel_walk.md` §9's finding that
live slips cost ~25 points of at-own-true-integer rate at BOTH lane rungs
(EWL(2,5) 95.1 → 70.2 %, WL(1,2) 46.2 → 21.1 %, matched slipped-vs-quiet).**

Instrument: `scripts/simnet_lane_slip_age.py` — joins
`network_dd_fractional.csv` to the fixture's true integers exactly as
`simnet_amb_integer.py` does (python-int truth, cumulative `truth_slips`
deltas), forms lane errors `e_lane = e_band1 − e_band2` on matched
`(tow, sat, ref, sta_a, sta_b)` quadruples, then buckets them THREE ways:

1. by TRUTH slip age of the quadruple's legs (in-lane-band vs any-band),
2. by ESTIMATOR response age (`network_events.csv`, refloat-class events
   `{if_slip, mw_wl_slip, lli_slip, attrib_arc}` — false alarms included),
3. by the CLASS of the nearest response (≤ 20 ep).

Method caveats carried from the O2 card: rows are heavily repeated per key —
only medians and rates over large n are quoted, never p-values; the trace pairs
against one ref PRN per (const, signal), so quadruples share ref legs; the
"quadruple" is a truth-side identity, blind to the estimator's internal chord
lifecycle (which is the point — the response events supply that side).

## §1 The headline decomposition (GPS EWL(2,5), slipped arm `camp60wu_f1`)

| population | n | med\|e\| (cyc) | at own integer |
|---|---|---|---|
| **RESPONSE-VIRGIN** (no refloat-class event on any leg, ever) | 16 036 | **0.065** | **98.9 %** |
| within 20 ep of `mw_wl_slip` | 45 744 | 0.224 | 75.4 % |
| within 20 ep of `lli_slip` | 9 876 | 0.292 | 70.3 % |
| within 20 ep of `if_slip` | 6 240 | 0.305 | 65.6 % |
| **within 20 ep of `attrib_arc` (T4)** | **178 965** | 0.326 | **65.4 %** |
| response age 61–200 ep | 49 677 | 0.199 | 83.8 % |

Galileo EWL(5,7) response-virgin: **100.0 %** (n 15 435, med|e| 0.063).
GPS WL(1,2) response-virgin: 67.7 % vs 17–21 % near-response.

**Reading:** on 2 486-slip data, chords no response ever touched are
essentially perfect at the EWL rung — better than the quiet twin's pooled
95.1 % (which pools its own young arcs).  The entire lane-integer slip cost is
**response-adjacent**: each refloat-class event re-randomizes the lane level,
recovery takes 60–200 ep, and at 12 356 events / 720 ep the network never gets
that much quiet time on a leg.  T4 `attrib_arc` alone is 7 648 of the 12 356
events and ~74 % of the near-response row exposure at the worst per-event
quality — the same T4 storm `SPEC_slip_relevel_walk.md` §5 called a CO-SYMPTOM
is, on the lane-integer instrument, the single largest destroyer of integer
structure.

## §2 Truth-side corroboration (independent of the event log)

- **In-band truth-slip age shows NO healing under the refloat response**:
  GPS EWL 65.3 % at age 1–5 ep → 64.9 % at 21–60 → 72.6 % at 201+ — the
  re-level bakes in a wrong lane level for the life of the arc.
- **Other-band-only leg slips** (truth continuous in BOTH lane bands, vertex
  refloated anyway): EWL 85.1 → 74.8 %, WL 34.6 → 19.4 % — 10–15 points of
  pure response-inflicted damage, no in-band information loss at all.
- **Galileo EWL(5,7) is robust everywhere** (94.1 % virgin → 87–93 % aged):
  λ_EWL(5,7) ≈ 9.8 m makes a code-derived re-level good to ~0.03 cyc, so the
  response barely dents it.  GPS EWL(2,5) at λ 5.86 m absorbs ~2× the damage;
  per-band (λ 0.19–0.25 m) absorbs all of it.  The damage scales exactly as
  1/λ_lane — consistent with metre-class level error injected per response.

## §3 The fix ladder (leverage order), and what each must pre-flight

1. **T4 keep+retap instead of refloat — ALREADY BUILT** (`relevel_retap_statistical`,
   default-OFF, `SPEC_slip_relevel_walk.md` §8): keeps the arc's level, re-taps
   variance to 0.25.  It was graded TRUTH-NEUTRAL on ZWD/storm/census (F3≈F2)
   — but the lane-integer instrument did not exist then, and this SPEC's
   decomposition predicts it should recover most of the T4 share.
   **Hypothesis arm running: `runs/camp60wu_f3` (`_camp60wu_f3.json`).
   Pre-registered: GPS EWL at-own-integer ≥ +10 points over F1's 70.2 % (the
   T4 share of the deficit); ZWD hourly walk stays flat (F1's 1.85–3.08 band);
   census unchanged; if EWL moves < +3 the retap's kept level is NOT what the
   lane needs and rung 2 is the card.**
2. **Band-scoped response**: LLI detections are band-specific and the §2
   other-band class shows vertex-wide refloat costs 10–15 points on bands
   whose truth is continuous.  Refloat only the detected band's level where
   the detector localizes (LLI always; MW/GF pairs localize under 3 bands).
3. **Lane-preserving re-level**: when band b must refloat, seed its new level
   to keep the MEASURED lane delta integer: the EWL(2,5) MW-analogue measures
   the lane jump at σ ≈ 0.05 cyc single-epoch (0.3 m code on λ 5.86 m) — a
   confident integer snap for EWL, marginal for WL (σ ≈ 0.35), impossible
   per-band (the 6.66° geometry, `project_why_non_integer_rootcause`).  Use
   the bridge-v2 pend-and-corroborate shape (`SPEC_slip_relevel_walk.md`
   §8.2), NOT the killed mw_only direct write (§8.1: 0/49 correct — that
   repaired the PER-BAND level, a different and impossible object).

Invariant pre-flight (standing invariants #1/#4/#7):
- #1: a KEPT arc past a real slip carries a stale level — retap's variance
  re-tap is the wired response; the blind spot is a slip equal-in-cycles in
  ALL bands (lane-silent), which GF still sees (equal cycles ⇒ ΔGF =
  (λ₁−λ₂)·n ≠ 0) — any keep-gate must consult GF before keeping.
- #4: keep/refloat decisions need counters with identities on BOTH outcomes.
- #7: a keep-gate is a new edge at both ends — event rows at decide time,
  and the kept arc re-validates (or re-taps) at the next MW confirm.

## §4 What this does NOT claim

- Nothing here credits or involves the wind-up sign fix — the sim is
  structurally blind to it (`SPEC_phase_windup_sign.md` §2).
- The quiet twin's own EWL ceiling (95.1 %) and the response-virgin 98.9 %
  bound what lane-fixing can deliver on THIS posture; the per-band rung stays
  owned by iono representation regardless.
- Field transfer is not automatic: the field's T4 population and false-alarm
  mix differ (the campaign's field arm hit an A4-withhold deadlock era on an
  unrelated axis) — a field leg re-grades this before any default flips.
