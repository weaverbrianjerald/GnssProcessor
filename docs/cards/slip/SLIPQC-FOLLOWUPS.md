# slip-QC follow-ups — the 2026-07-22 follow-up cards D5-A / W / OP / H, and why the D5 lead outranked everything else

**STATUS**

- **id:** slip-QC follow-ups
- **topic:** slip
- **status:** closed
- **summary:** the 2026-07-22 follow-up cards D5-A / W / OP / H, and why the D5 lead outranked everything else
- **dates in the text:** 2026-07-22 → 2026-08-15
- **knobs / posture:** `network.mw_wl.d5_dump`, `estimation.global_vtec_sh.rep_error_tau_s`, `processing.max_duration_s`, `network.max_duration_s`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_slip_qc_hardening.md` — 9, 9.0, 9.1, 9.2, 9.3, 9.4, 9.5

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_slip_qc_hardening.md -->
## 9. Follow-up cards, 2026-07-22 (absorbed `SPEC_slip_qc_followups.md`)

Written by the session that lean-reviewed the Cards E/R/L/G implementation. Its header declared §1 of
this document "incorporated by reference and binding here unchanged" — it still is.

### 9.0 Evidence base, lean-review notes N1–N5, and why the D5 lead outranks everything else

**Lean-review verdict (2026-07-22): PASS, 0 defects.** Build clean, suite 864/863/1, every parent MUST
met or its premise measured false and handled (F1/F4/F5), OFF byte-identity documented per-artifact.
Five minor notes, none a defect:

| note | content |
|---|---|
| 1 | **N1** (→ W6, H3): `updateRefDispersion` runs *before the verdict is known*, so an incoherent **STATION EVENT**'s large dispersion is folded into the refsat scale EWMA, inflating the refsat threshold for **~1/α ≈ 7 epochs** afterward. A genuine ref slip just above the floor could be suppressed in that window (pool arm + D5 backstop it). |
| 2 | **N2** (→ H1): `IfPhaseSlipDetector::RefGateParams::max_consecutive` C++ default is **2**; the config default (the only armed path) is **1**. Cosmetic drift. |
| 3 | **N3** (→ H2): Card-G counters mix units — `gap_shifted` counts **RESUMPTIONS** (per sat-pair), `gap_shifted_bridged`/`fresh` count **ARCS** (per band), hence "11 shifted … 22 fresh". |
| 4 | **N4** (→ W4): an **UNMEASURABLE** uncorroborated assertion at a demoted station is suppressed (a single-band arc at a chatty station has no MW/GF/IF witness; only D5 backstops a real slip there). Intended, counted, unvalidated warm. |
| 5 | **N5**: the drain-level act/suppress path has no direct unit test (primitives + field falsifiers cover it). Acceptable; **no card**. |

Two reframings of §8's numbers. First, the **4 residual refsat fires are the PHYSICAL-FLOOR arm** firing
while the empirical dispersion EWMA is still cold — `ref_shrink_kappa = 20` samples ≈ **7–10 epochs** to
warm on the 2–3-DD thin L2L family. Second, **why the D5 lead is first-class**: each re-tap discards
accumulated DD information, and 1 560–1 689 re-taps per 11 epochs at 150 sta, ~97 % with no raw step, is
***with* `carry_invariant_innov_calibrate = true` live** (σ̂₀ = 1.78, window 15 — an empirically-calibrated
≈ 7·σ_formal gate), so **the tension is STRUCTURED and no global scalar can absorb it.** Either (a) the
gate fires on structured model tension or (b) a real slip class is invisible to MW/GF. Prime suspect for
(a): **per-LOS iono representation error** — the T4/Jul-17 A/B showed **~70–80 % of LOS residuals > 3σ at
ANY SH degree**, and the Iono-2 card (`docs/SPEC_iono2_mechB_v3.md §6-§10`) carries that misfit as a
per-(station,sat) GM deviation. If Iono-2 absorbs the tension, D5 re-taps collapse — the cheapest decisive
measurement on the NL critical path. *(Out of scope, unchanged from §0: the all-freq `dd-build` perf
blowup, LF-1 lineage; the physical L2L drift at JPLM/MKEA.)*

### 9.1 Card D5-A — D5 re-tap attribution (instrument-first; land BEFORE Card W)

*FORMERLY cited as `SPEC_slip_qc_followups.md §1` by `src/core/filter/NetworkInstallCarry.cpp:745` and
`scripts/grade_slip_agreement.py:68`; the 2026-08-14 merge re-pointed both to
`docs/SPEC_slip_qc_hardening.md §9.1` (read and verified 2026-08-15). Do not hunt for the old string.*
**Framing**: invariant #3 — no gate mechanism changes until the warm decomposition exists; this card
makes the warm run produce the attribution dataset in one pass.

**Where** (as spec'd 2026-07-22): `scripts/grade_slip_agreement.py` (extend); the `d5_dump` writer,
gated on `network.mw_wl.d5_dump && carry_invariant_innov_gate` with `slip_qc.epoch.stepped` in scope
(columns in §10.2.1). ⚠ That writer **RELOCATED** with the network decomposition (INC-0..12) — see the
anchor-drift map; live at `NetworkInstallCarry.cpp:745`+ (verified 2026-08-15).

**MUST.** **D1** — grader gains a `--d5` section joining `network_d5_flags.csv` against
`network_slip_raw.csv` on (station, satellite, epoch ± 1), reporting for the re-tap population the
raw-step-coincident fraction (cross-checked ≈ the run-end counter pair), the `std_innov` distribution
vs the k·σ̂₀ gate, top (station, sat) families with consecutive-run lengths, and the warm/cold split —
**zero producer changes**, working on existing CSVs *(delivered contract: §10.2.2)*. **D2** — append
`raw_stepped` (0/1 from `slip_qc.epoch.stepped`) to the `d5_dump` row; header-compatible, log-only,
inert by construction, riding the existing `d5_dump` opt-in — **no new toggle**; the offline join stays
the source of truth for ±1-epoch tolerance, the column is the cheap exact-epoch tag *(landed
TRI-STATE, §10.2.1)*. **D3** — the cross-experiment protocol is written into the card's report
**before** Card W runs *(ruled and extended in §10.3, which also carries the v3 CANDIDATES this
decomposition is collecting for — reset-over-repair for bias-less signals,
`[[feedback-reset-over-repair-biasless]]`)*. **D4** — no change to D5 gating, flags, funnel, or
counters beyond the D2 column; suite green; `d5_dump` OFF runs byte-identical.

**Falsifier (10-ep smoke, sliphard + `d5_dump` armed)**: grader `--d5` totals match the run-end
`d5_retaps`/`d5_retaps_stepped` counters; `raw_stepped` populated; OFF-toggle runs unchanged. **Result:
§10.5.3 — MET on both units.**

### 9.2 Card W — warm validation leg (≥ 1 h) — ⛔ PREPARED 2026-07-22, NEVER LAUNCHED

*FORMERLY cited as `SPEC_slip_qc_followups.md sec.2` by the `_comment_card_w` field of all three
`config/test_suite/_wlver_150sta_1h_d10_allfreq_{slipcmp,sliphard,sliphard_iono2}.json`; the same merge
re-pointed all three to `docs/SPEC_slip_qc_hardening.md §9.2` (verified 2026-08-15).* §6.3's obligation
with the full grading protocol so the run is turnkey. **LAUNCH ONLY ON THE USER'S COMMAND.**

| item | content |
|---|---|
| **arms** (150-sta GPS-only all-freq/all-channel, same span and station set, all in tree) | **(i) ON** `_wlver_150sta_1h_d10_allfreq_sliphard.json` — hardening armed (Cards R/L/G), Iono-2 OFF, needs `network.mw_wl.d5_dump = true`; **(ii) Iono-2** `_wlver_150sta_1h_d10_allfreq_sliphard_iono2.json` — (i) **plus** `estimation.global_vtec_sh.rep_error_tau_s = 900`; **(iii) OFF** `_wlver_150sta_1h_d10_allfreq_slipcmp.json` — the three slip-QC masters absent |
| **span** | 1 h = **121 epochs at 30 s**. The followups card's instruction was to raise `processing.max_duration_s` / `network.max_duration_s` to **≥ 3 600 s (≥ 120 ep @ 30 s)** together with `network.mw_wl.d5_dump=true` (D5-A data). **Discharged in fact**: all three configs carry `max_duration_s: 3600.0` and `d5_dump: true` |
| **cost — two measurements, kept apart** (never average them) | followups §2 estimate ~35–50 s/ep ⇒ ~100–140 min/arm ⇒ ≈ 5–7 h wall for three arms; d5a §6, re-measured from the 2026-07-22 gate run's section timers (`dd-build` alone 37–55 s/ep), 42–60 s/ep ⇒ ~85–120 min/arm ⇒ ≈ 4.5–6 h. Plus ~2 min of 150-station RINEX ingestion per arm. ⚠ The three configs' own `_comment_card_w` carries a **third, unreconciled** figure ("~35-50 s/ep … => ~70-100 min per arm"); it predates the d5a re-measurement |
| **order** | **Sequence ON → Iono-2 → OFF** so the user can stop after any arm and still hold a complete result: (i) alone answers W1–W6, (i)+(ii) answer W7/D3, (iii) is only needed for W3's ON-vs-OFF comparison. State this when reporting readiness. Release-only, renamed exe (`gnss_processor_sliphard_run.exe`), name-scoped zombie pre-kill, LNK1104 ⇒ retry |
| **run mechanics beyond §6.4** (from the readiness pass) | **rebuild Release FIRST** (the exe on disk is only as current as the last build), then copy to the renamed exe, then run; **move `logs/network_d5_flags.csv` and `logs/network_wl_lane_trace.csv` aside before each arm** (both append-mode — the grader can segment an appended file, §10.2.2, but a fresh file makes the arm-to-arm comparison trivial); **copy the whole `logs/` dir per arm immediately** (the next arm overwrites it); **smoke arm (ii) for 10 epochs before committing to its 1-hour leg** — the only arm whose knob combination has never been run here (arms (i)/(iii) have 10-epoch twins), ~10 min, arming line `Network (Iono-2/T5v2): … GM deviation ON`. Grading per arm: `python scripts/grade_slip_agreement.py <arm_logdir> --json <arm>.json` |
| **reporting** | one report file (`docs/slip_qc_warm_report_<date>.md`) with the W1–W7 numbers, each MET / NOT-MET / DEFERRED-WITH-DATA; update the relevant memory files. ⛔ **Never created; it will not exist unless Card W is launched** |

**MUST — per-card warm gates** (record every number):

| gate | source | expectation |
|---|---|---|
| **W1 (G3 decision rule — the parent's required triage artifact)** | run-end Card-G line, warm arm: `shifted-AND-bridged` **arcs** | ≈ 0 ⇒ the re-entry mechanism is **DEFERRED WITH DATA** — write the number into the report and close L4. > 0 ⇒ spec the follow-up card (mark such arcs slipped at re-entry via the C3c funnel, or block the lane bridge) with the measured rate in hand |
| **W2 (Card R warm takeover)** | grader §(e) unsupported refsat count | **0** once the dispersion EWMA is warm (the 4 cold residuals were the physical-floor arm; `s_eff` should exceed the floor on the thin L2L family within ~10 ep). OWMG-class station events still fire on the station-event path. If unsupported fires persist warm, H3's fold-exemption and/or a floor-arm de-dup become a real card — spec then, with the rate |
| **W3 (Card L sawtooth)** | ON vs OFF at OWMG00NZL / MAC100AUS | no ambiguity-variance sawtooth; per-station D5 re-taps do not increase (a drop is expected); credibility transitions bounded — hysteresis working, not oscillating |
| **W4 (N4 watch)** | suppressed LLI population vs a later raw step / IF flag on the same (station, sat) within +2 ep | ≈ 0 expected; nonzero ⇒ the unmeasurable-at-demoted-station suppression is costing real events — **feeds Card OP** |
| **W5 (agreement + E4 sustained)** | grader §(e) raw-confirmed rate; §(b) IF↔MW from `network_events.csv` alone; §(c) histogram | ≥ 90 %; no new bunching at scale |
| **W6 (N1 observational)** | the log's ref-gate DEFER lines after any station-event epoch | note whether a genuine candidate was suppressed inside the ~7-ep inflation window. Observational only; feeds H3's **go/no-go** |
| **W7 (D5-A D3)** | grader §(g) per arm | the §10.3 decision rule, with the numbers |

### 9.3 Card OP — off-pair / unmeasurable corroboration widening (CONDITIONAL) — trigger never fired

**Trigger** (do not build otherwise): `lli_offpair` material on a warm or multi-constellation run (cold
GPS-only measured **2 of 125**) **or** W4 > 0. **MUST** (if triggered): **O1** an LLI'd band outside the
primary pair gains a witness — either MW/GF formed on (LLI'd band × nearest other band), or the
per-band phase-minus-code step (`l1_c1_m` generalized) with its own per-station empirical scale, same
pre-update-scale / clean-survivor discipline as the existing seam; **O2** `lli_offpair` splits into
corroborated/uncorroborated once witnessable, while `lli_unmeasurable` keeps meaning "structurally no
witness"; **O3** default-OFF sub-toggle under `network.cycle_slip.mw_lli_*`, byte-identity, additive
unit tests. **CANDIDATE**: prefer the **phase-minus-code witness** — it exists for every band with a
matching code and needs no pairing logic; its noise is code-dominated, so the empirical per-station
scale (not a fixed floor) carries the gate.

### 9.4 Card H — housekeeping (fold into the next slip-QC commit; no field gate)

**H1 (N2)**: `RefGateParams::max_consecutive` default 2 → **1**, matching the config default; update the
Doxygen line *(LANDED — §10.4)*. **H2 (N3)**: make Card G units explicit; counters may be renamed freely
(no consumer besides the log line and grader) *(LANDED, and exposed a second defect — §10.4)*. **H3 (N1,
W6-gated)**: exempt the refsat dispersion EWMA fold on epochs whose verdict lands `FireNoRebase` (compute
the verdict before the fold, or un-fold after — keep the pre-update-gate property either way); **build
ONLY if W6 shows a real suppression, otherwise record "deferred, W6 = 0"** *(deferred; W6 unmeasured)*.

**Commit logistics (HISTORICAL — discharged).** The slip-QC work shared `ApplicationNetwork.cpp` /
`GnssConfigTypes.hpp` / `GnssConfigLoadNetwork.cpp` / `CMakeLists.txt` with the then-uncommitted CB-ADM,
Iono-2 and UWL/WL-3 workstreams, so a slip-QC-only commit needed **hunk-level staging** on those four
(the `slip/` TUs, tests, grader and configs are cleanly separable) — keep that as a pattern. All three
are now in HEAD: `if_phase_td_ref_*`, `mw_lli_*`, and Iono-2's `rep_error_tau_s` / `rep_error_d5_keep` /
`rep_error_demean` resolve in `GnssConfigLoadNetwork.cpp` and `GnssConfigTypes.hpp:2622`.

### 9.5 Acceptance summary + sequencing

| Card | Type | Gate | Outcome |
|---|---|---|---|
| D5-A (first) | grader + 1 log-only column | `--d5` totals ≡ run-end counters; OFF byte-identical; suite green | **LANDED 2026-07-22** (§10) |
| W | run + grading (on command) | W1–W7 recorded; each MET / NOT-MET / DEFERRED-WITH-DATA | ⛔ **NEVER LAUNCHED** |
| OP | conditional mechanism | trigger fires first; then O1–O3 + byte-identity | **UNTRIGGERED** |
| H | hygiene | H1/H2 with the next commit; H3 only if W6 > 0 | **H1/H2 LANDED; H3 deferred, W6 unmeasured** |

Sequencing: **D5-A → (report readiness) → W on command → OP/H3 as the data rules → any v3 mechanism
card spec'd fresh with the W numbers.** Execution stopped after D5-A.

---

