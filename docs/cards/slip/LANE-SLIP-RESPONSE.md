# lane slip response: the lane-integer slip cost is 100 % response-inflicted

**STATUS**

- **id:** lane slip response
- **topic:** slip
- **status:** closed
- **summary:** The GPS lane-integer slip cost is 100 % response-inflicted (response-virgin EWL chords 98.9 % at their own integer); blanket keep and the corroborated retap (rung 1′) are FALSIFIED as armed.
- **dates in the text:** 2026-08-21 → 2026-08-22
- **knobs / posture:** `network.cycle_slip.relevel_retap_corroborate`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/camp60wu_f1`, `runs/camp60wu_f3`, `runs/camp60wu_f4c`
- **source:** `docs/archive/SPEC_lane_slip_response.md`: (preamble), §1, §2, §3, §4, §5

**Outcome.** The question was where the ~25-point slip loss at both lane rungs comes from: every refloat-class response re-randomizes the lane level, and T4 `attrib_arc` is the largest source (7 648 of 12 356 events). Keeping the flagged level loses to refloat-always (settled GPS EWL 70.2 %): blanket keep reaches 54.0 %, and the corroborated keep 59.6 %, since the raw MW/GF evidence refuses only 1.5 % (T4 catches drifts, not steps). For T4-flagged arcs the refloat is the better response. Both retap knobs are KEEP_OFF in the [ledger](../../CONFIG_promotion_ledger.md). Rung 2 (band scope) was later KEEP_OFF in [Study 3](STUDY-3-band-scoped-responses.md); rung 3 continues, open, in [rung 3](LANE-SNAP-RUNG3.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_lane_slip_response.md -->
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

1. **T4 keep+retap instead of refloat — RAN, and the gate fired the OTHER way
   (FALSIFIED as a blanket policy).**  `runs/camp60wu_f3` (`_camp60wu_f3.json`,
   retap live: 24 870 kept / 462 escalated, 720/720 emitted; the bridge applied
   33 times — negligible, the retap is the mover):

   | settled (last 25 %) | F1 refloat-always | F3 keep-always |
   |---|---|---|
   | GPS EWL(2,5) at own integer | 70.2 % | **54.0 %** |
   | GPS WL(1,2) | 21.1 % | 13.9 % |
   | ZWD hourly err rms | 1.85–3.08 flat | 2.20–3.28 flat |

   Blanket keep is 16 points WORSE than blanket refloat on the lane instrument
   while staying ZWD-NEUTRAL — the §8 "truth-neutral" grade was
   instrument-limited, not wrong.  The age decomposition explains it: under
   keep, rows within 5 ep of a response fall to EWL 47.7 % (refloat: 66.6 %)
   with med|e| 0.544 — **at flag time the flagged arc's level really is wrong
   more often than a code re-level**, and var 0.25 anchors it.  Both blanket
   policies lose; the population is heterogeneous and only per-arc EVIDENCE
   separates it.

   **Rung 1′ — the corroborated retap (BUILT 2026-08-22, default-OFF):**
   `network.cycle_slip.relevel_retap_corroborate` gates the keep on POSITIVE
   pre-solve raw-observable continuity — the slip-QC QUALIFY verdicts (measured
   AND not stepped), the same tri-state the D5-v3 corroboration gate consumes.
   Unmeasured / stepped / no-evidence arcs take the full erase (invariant #1).
   Wiring: `NetworkRaim.cpp` keep-branch gate fed by `RawSlipEpoch*` through
   the seam signature; the knob registers as a slip-QC DECISION card
   (`SlipQcParams::retap_corr` in `anyConsumer` + `cardsArmed` — without that
   the qualify pass never fills the verdicts and the gate silently degenerates
   to refloat-always, the exact trap the D5-v3 auto-arm comment names).
   Counters both ways (`corr_kept` / `corr_stepped` / `corr_unmeasured`),
   teardown line prints all three; loader interlocks: corroborate without
   retap_statistical warns + disarms, and dies with its base under
   carry_invariant.
   **Validation arm pre-registered (`_camp60wu_f4c.json` = free_epoch +
   retap_statistical + corroborate, NO bridge): corr counters nonzero BOTH
   ways; settled GPS EWL ≥ F1's 70.2 % (must not lose to refloat-always),
   target toward the 85 %+ virgin band; ZWD flat in F1's band; census
   unchanged.**
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

## §5 Rung 1′ VERDICT (2026-08-22, `runs/camp60wu_f4c`) — FALSIFIED AS ARMED; the evidence lacks resolution

720/720 emitted; counters both ways (24 315 kept-with-evidence, 360
refused-STEPPED, 0 refused-UNMEASURED — the wiring is live and the qualify
pass ran).  But the refusal rate is **1.5 %**: the raw MW/GF empirical step
gates call almost every T4-flagged arc FLAT, so the corroborated keep
degenerates to near-blanket keep and inherits its harm:

| settled GPS lane | F1 refloat-always | F4C corroborated keep | F3 keep-always |
|---|---|---|---|
| EWL(2,5) at own integer | **70.2 %** | 59.6 % | 54.0 % |
| WL(1,2) | **21.1 %** | 17.0 % | 13.9 % |

**Root cause — the evidence measures the wrong failure mode.**  A raw MW/GF
step gate detects DISCRETE jumps at multi-tenths-of-a-cycle thresholds; the
levels T4 catches are DRIFTS (sub-threshold accumulation) that the network
solve sees at > kσ̂₀ while the per-arc band-pair observables see nothing.
"No step" and "level wrong vs the network" are simultaneously true, and the
T4 flag IS the higher-resolution evidence — corroborating it against a
coarser instrument and keeping on the coarser instrument's silence was
backwards.  The knob stays built, default-OFF, counters intact (it is the
correct SHAPE for any future finer evidence source), but do NOT arm it with
the raw-step verdicts as the evidence.

**Standing conclusion of this card:** for T4-flagged arcs the refloat is the
better response (the code re-level is EWL-good at λ 5.86 m); the recoverable
losses are (a) rung 2 — vertex-wide responses hitting truth-continuous bands
(10–15 pts, band-scoped refloat where the detector localizes), and (b) rung 3
— making the refloat LANE-PRESERVING: after the per-band code re-level,
integer-snap the EWL lane to its EWL-MW-measured delta (σ ≈ 0.05 cyc
single-epoch ⇒ a confident snap), bridge-v2 corroboration shape.  Rung 3 is
the next build; this decomposition (F1/F3/F4C + the age tables) is its brief.

