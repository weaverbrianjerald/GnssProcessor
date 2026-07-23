# SPEC — Slip-QC follow-ups: warm validation, D5 attribution, residuals (2026-07-22)

**Handoff**: written 2026-07-22 by the session that lean-reviewed the Cards E/R/L/G
implementation (review verdict: PASS, 0 defects — see §0.2). Self-contained — do
not assume access to either prior conversation. Read `CLAUDE.md` (build pattern,
**Standing invariants**), `docs/SPEC_slip_qc_hardening.md` (the parent spec; its
§1 ground rules are **incorporated by reference and binding here unchanged**) and
`docs/slip_qc_hardening_report_2026-07-21.md` (the implementation report — its
Findings F1–F5 are the evidence base below) before starting.

**Intent**: the 2026-07-21/22 hardening closed the four audit limitations
(refsat FP family 14→4, raw-confirmed 71%→90%, cold MW/LLI emission, LLI
qualification, gap instrumentation). It also *opened* one first-class lead and
left four bounded residuals. This spec defines the remaining work. Cards state
MUST criteria (binding) and CANDIDATE mechanisms (non-binding — deviate with a
stated rationale). If a MUST seems wrong, stop and ask the user.

---

## 0. Evidence base

### 0.1 What the hardening measured (armed 10-ep smoke, cold)

| Quantity | Value |
|---|---|
| Refsat FP family (JPLM/MKEA G10, raw flat) | 14 → **4**, longest consecutive run 1; residuals are the *physical-floor* arm firing while the empirical dispersion EWMA is still cold (`ref_shrink_kappa=20` samples ≈ 7–10 ep on the 2–3-DD thin family) |
| Raw-confirmed IF-tier rate (grader) | 71% → **90%**; the entire unsupported set is still the refsat side-channel |
| Card L (cold, GPS-only) | 125 assertions: 34 MW-corr, 3 GF-corr, 49 unmeasurable, **2 off-primary-pair**, 22 suppressed; real bit-0 chatterers = OWMG00NZL, MAC100AUS (the JPN "chatter" was LLI **bit 2** — antispoofing — never acted on; do not re-derive, see report F1) |
| Card G (cold) | 16 resumptions / 11 shifted / **0 shifted-bridged** / 22 shifted-fresh (arc-level); cold ⇒ almost nothing bridges by construction — necessary, not sufficient for G3 |
| **D5 attribution (the lead)** | **1 689 re-taps, 50 (3.0%) coincident with a raw MW/GF step** — and this is *with* `carry_invariant_innov_calibrate=true` live (σ̂₀ = 1.78, window 15: the gate was already an empirically-calibrated ≈7·σ_formal test). D5 contributed 1 560 of the 2 318 OFF-run carry refloats; the coarse detectors ≈ 154 |

### 0.2 Lean-review verdict on the implementation (2026-07-22)

PASS — build clean, suite 864/863/1, every parent-spec MUST met or its premise
measured false and handled (F1/F4/F5), OFF byte-identity documented per-artifact.
Five minor notes, none a defect, folded into this spec's cards:

- **N1** (→ W6, H3): an incoherent *station event*'s large dispersion is folded
  into the refsat scale EWMA (`updateRefDispersion` runs before the verdict is
  known), inflating the refsat threshold for ~1/α ≈ 7 epochs afterward. A
  genuine ref slip just above the floor could be suppressed in that window
  (pool arm + D5 backstop it).
- **N2** (→ H1): `IfPhaseSlipDetector::RefGateParams::max_consecutive` C++
  default is 2; the config default (the only armed path) is 1. Cosmetic drift.
- **N3** (→ H2): Card G counters mix units — `gap_shifted` counts *resumptions*
  (per sat-pair), `gap_shifted_bridged/fresh` count *arcs* (per band), so the
  summary line reads "11 shifted … 22 fresh".
- **N4** (→ W4): an **unmeasurable** uncorroborated assertion at a demoted
  station is suppressed (single-band arc at a chatty station has no MW/GF/IF
  witness; only D5 backstops a real slip there). Intended, counted, but
  unvalidated warm.
- **N5**: the drain-level act/suppress path has no direct unit test (the
  primitives and the field falsifiers cover it). Acceptable; no card.

### 0.3 Why the D5 lead outranks everything else here

The D5 gate re-taps Card-4 carry chords — each re-tap discards accumulated DD
information. 1 560–1 689 re-taps per 11 epochs at 150 sta, ~97% with no raw
observable step, is either (a) a gate firing on **structured model tension** a
*global scalar* σ̂₀ cannot absorb, or (b) a real slip class invisible to MW/GF.
The prime suspect for (a) is per-LOS iono representation error: the T4/Jul-17
A/B showed ~70–80% of LOS residuals > 3σ at ANY SH degree, and the Iono-2 card
(`docs/HANDOFF_iono2_card.md`, built, uncommitted) exists precisely to carry
that misfit as a per-(station,sat) GM deviation. If Iono-2 absorbs the tension,
D5 re-taps collapse — which simultaneously validates Iono-2's mechanism and
un-churns the carry that the WL/NL lane cards sit on. This is the cheapest
decisive measurement available on the NL critical path.

Out of scope (unchanged from parent): the all-freq `dd-build` perf blowup
(LF-1 lineage); root-causing the physical L2L drift at JPLM/MKEA.

---

## 1. Card D5-A — D5 re-tap attribution (instrument-first; land BEFORE Card W)

**Framing**: Standing invariant 3 — no gate mechanism changes until the warm
decomposition exists. This card makes the warm run (Card W) produce the
attribution dataset in one pass.

**Where**: `scripts/grade_slip_agreement.py` (extend);
`src/core/app/ApplicationNetwork.cpp:5255-5290` (the `d5_dump` writer — columns
`week,tow,group,chord_u,chord_v,station,amb_key,std_innov,ewl_stasat,hold_chord,injected_prev`,
gated on `network.mw_wl.d5_dump` && `carry_invariant_innov_gate`);
`slip_qc.epoch.stepped` is in scope at the writer.

**MUST**:
- D1. Grader gains a `--d5` section: join `network_d5_flags.csv` against
  `network_slip_raw.csv` on (station, satellite, epoch ± 1) and report, for the
  re-tap population: raw-step-coincident fraction (cross-check ≈ the run-end
  counter pair), distribution of `std_innov` (min/med/p90/max, vs the k·σ̂₀
  gate), top (station, sat) re-tap families with counts and consecutive-run
  lengths, and the warm/cold split (epoch index vs `pl_warmup`). Zero producer
  changes required for this — it must work on the existing CSVs.
- D2. Per-row attribution column: append `raw_stepped` (0/1 from
  `slip_qc.epoch.stepped`) to the `d5_dump` row. Header-compatible change
  (append-only, header written fresh per run); `d5_dump` is log-only so this is
  numerically inert by construction, but it rides the existing
  `network.mw_wl.d5_dump` opt-in — no new toggle. The offline join (D1) remains
  the source of truth for ±1-epoch tolerance; the column is the cheap exact-epoch
  tag.
- D3. The **cross-experiment protocol** is written into the card's report
  before Card W runs: on the warm config, three arms —
  (i) hardening ON, Iono-2 OFF (the Card W ON arm does double duty);
  (ii) hardening ON, Iono-2 armed (`sh_iono.rep_error_tau_s=900`, per its
  handoff); (iii) the OFF baseline (Card W's other arm). Decision rule,
  recorded with the numbers:
  * re-taps(ii) ≪ re-taps(i) (order-of-magnitude class) ⇒ tension **is** iono
    representation — no D5 gate change; the D5 re-tap count becomes a standing
    grading metric for Iono-2, and the residual population is re-examined.
  * re-taps(ii) ≈ re-taps(i) ⇒ the global σ̂₀ hypothesis is dead AND the iono
    hypothesis is dead for this population ⇒ spec a D5-v3 card from the D1
    decomposition (see CANDIDATES) — do not build it inside this card.
- D4. No change to D5 gating, flags, funnel, or counters beyond the D2 column.
  Suite green; `d5_dump` OFF runs byte-identical.

**CANDIDATES** (for the *eventual* v3 card, only if D3 rules that way —
recorded here so the decomposition collects what they need):
- Per-(station) or per-elevation-bin empirical σ̂ (the pool-arm pattern) instead
  of one global σ̂₀ — D1's top-family + elevation columns decide if the tension
  is station-structured.
- Innovation-persistence semantics (re-tap only on K consecutive exceedances) —
  D1's consecutive-run lengths decide if tension is impulsive or sustained.
- If a genuinely invisible slip class emerges (raw-flat, iono-clean, sustained):
  that is a *finding*, not a gate defect — reset-over-repair stands
  ([[feedback-reset-over-repair-biasless]]); the card would then justify the
  re-taps and close.

**Falsifier (10-ep smoke, sliphard config + `d5_dump` armed)**: grader `--d5`
totals match the run-end `d5_retaps`/`d5_retaps_stepped` counters; `raw_stepped`
column populated; OFF-toggle runs unchanged.

---

## 2. Card W — warm validation leg (≥ 1 h; LAUNCH ONLY ON THE USER'S COMMAND)

The parent spec's §6.3 obligation, now with the full grading protocol so the
run is turnkey. **Prep and report readiness; do not launch unprompted.**

**Config**: `config/test_suite/_wlver_150sta_10ep_d10_allfreq_sliphard.json`
(all three masters + `if_phase_td` + `mw_detect` + full trace set already
armed), with: `processing.max_duration_s` / `network.max_duration_s` raised to
≥ 3 600 s (≥ 120 ep @ 30 s), and `network.mw_wl.d5_dump=true` (D5-A data).
Baseline arm: `_wlver_150sta_10ep_d10_allfreq_slipcmp.json` at the same span.
Iono-2 arm (D3): sliphard + `rep_error_tau_s` per the Iono-2 handoff.

**Cost honesty**: the all-freq/all-chan dd-build blowup means ~35–50 s/ep ⇒
**~100–140 min per arm**. Three arms ≈ 5–7 h wall. Recommend sequencing ON →
Iono-2 → OFF so the user can stop after any arm; state this when reporting
readiness. Release-only, renamed exe (`gnss_processor_sliphard_run.exe`),
name-scoped zombie pre-kill, LNK1104 = concurrent build lock ⇒ retry.

**MUST — per-card warm gates** (grader + counters; record every number):
- W1 (**G3 decision rule** — the parent's required triage artifact): warm
  `gap_shifted_bridged`. ≈ 0 ⇒ the re-entry mechanism is **deferred with
  data** — write the number into the report and close L4. > 0 ⇒ spec the
  follow-up card (mark such arcs slipped at re-entry via the C3c funnel, or
  block the lane bridge) with the measured rate in hand.
- W2 (**Card R warm takeover**): grader unsupported refsat count → expected
  **0** once the dispersion EWMA is warm (the 4 cold residuals were the
  physical-floor arm; `s_eff` should exceed the floor on the thin L2L family
  within ~10 ep). OWMG-class station events (if any occur) still fire on the
  station-event path. If unsupported fires persist warm, H3's fold-exemption
  and/or a floor-arm de-dup become a real card — spec then, with the rate.
- W3 (**Card L sawtooth check**): at the demoted stations (OWMG00NZL,
  MAC100AUS class), compare ON vs OFF arms: no ambiguity-variance sawtooth,
  and per-station D5 re-tap counts at those stations do not increase (drop
  expected). Credibility state transitions (demotions/promotions per station)
  bounded — hysteresis working, not oscillating.
- W4 (**N4 watch**): from the suppressed population, count assertions that a
  *later* epoch proved real (a raw step or IF flag on the same (station, sat)
  within +2 ep). Expected ≈ 0; nonzero ⇒ the unmeasurable-at-demoted-station
  suppression is costing real events — feeds Card OP.
- W5 (**agreement + E4 sustained**): grader raw-confirmed rate ≥ 90%,
  IF↔MW match-rate reported from `network_events.csv` alone; per-epoch flag
  histogram shows no new bunching at scale.
- W6 (**N1 observational**): from the log's ref-gate DEFER lines after any
  station-event epoch, note whether a genuine candidate was suppressed inside
  the ~7-ep inflation window. Observational only; feeds H3's go/no-go.
- W7 (**D5-A D3**): the cross-experiment numbers and verdict, per Card D5-A.

**Reporting**: one report file (`docs/slip_qc_warm_report_<date>.md`) with the
W1–W7 numbers, each gate marked MET / NOT-MET / DEFERRED-WITH-DATA. Update the
relevant memory files.

---

## 3. Card OP — off-pair / unmeasurable corroboration widening (CONDITIONAL)

**Trigger** (do not build otherwise): warm or multi-constellation runs show
`lli_offpair` material (cold GPS-only: 2 of 125) **or** W4 > 0.

**MUST** (if triggered):
- O1. An LLI'd band outside the primary pair gains a witness: either MW/GF
  formed on (LLI'd band × nearest other band), or the per-band phase-minus-code
  step (`l1_c1_m` generalized) with its own per-station empirical scale —
  same pre-update-scale/clean-survivor discipline as the existing seam.
- O2. `lli_offpair` splits into corroborated/uncorroborated once witnessable;
  `lli_unmeasurable` keeps meaning "structurally no witness".
- O3. Default-OFF sub-toggle under `network.cycle_slip.mw_lli_*`; byte-identity;
  additive unit tests (off-pair stream corroborated / suppressed).

**CANDIDATE**: prefer the phase-minus-code witness — it exists for every band
with a matching code and needs no pairing logic; its noise is code-dominated,
so the empirical per-station scale (not a fixed floor) carries the gate.

---

## 4. Card H — housekeeping (fold into the next slip-QC commit; no field gate)

- H1 (N2): `RefGateParams::max_consecutive` default 2 → **1**, matching the
  config default and its measured rationale; update the Doxygen line.
- H2 (N3): make Card G units explicit — rename the summary-line labels to
  "resumptions(sat-pair)" vs "arcs(band)" (or count both levels); a reader must
  not need the report to parse "11 shifted / 22 fresh". Counters may be
  renamed freely (uncommitted; no consumer besides the log line and grader).
- H3 (N1, W6-gated): exempt the refsat dispersion EWMA fold on epochs whose
  verdict lands FireNoRebase (requires computing the verdict before the fold or
  un-folding after — keep the pre-update-gate property either way). Build ONLY
  if W6 shows a real suppression; otherwise record "deferred, W6 = 0".

**Commit logistics** (flag to the user, not a card): the slip-QC work shares
`ApplicationNetwork.cpp` / `GnssConfigTypes.hpp` / `GnssConfigLoadNetwork.cpp` /
`CMakeLists.txt` with the uncommitted CB-ADM, Iono-2 and UWL/WL-3 workstreams.
A slip-QC-only commit needs hunk-level staging on those four files (the
`slip/` TUs, tests, grader and the two configs are cleanly separable). Do not
commit without the user's go; no co-author trailer.

## 5. Acceptance summary

| Card | Type | Gate |
|---|---|---|
| D5-A (first) | grader + 1 log-only column | `--d5` totals ≡ run-end counters; OFF byte-identical; suite green |
| W | run + grading (on command) | W1–W7 recorded; each MET / NOT-MET / DEFERRED-WITH-DATA |
| OP | conditional mechanism | trigger fires first; then O1–O3 + byte-identity |
| H | hygiene | H1/H2 with the next commit; H3 only if W6 > 0 |

Sequencing: **D5-A → (report readiness) → W on command → OP/H3 as the data
rules → any v3 mechanism card spec'd fresh with the W numbers.**
