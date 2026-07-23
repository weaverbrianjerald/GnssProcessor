# SPEC — Network slip-QC hardening (post IF-TD ↔ MW agreement audit, 2026-07-21)

**Handoff**: written 2026-07-21 by the auditing session for an implementing session.
Self-contained — do not assume access to the audit conversation. Read `CLAUDE.md`
(build pattern, layout, conventions, **Standing invariants**) before starting.

**Intent**: the 2026-07-21 slip-detector cross-check (150 sta, GPS-only,
`frequency_mode=all` + `channel_mode=all`, em10, 11 epochs, both
`network.cycle_slip.{mw_detect,if_phase_td}` armed, full MW-AR arm live) found the
IF-phase-TD pool detector **clean** (0.37 % flag rate, decisive margins, no bunching)
but exposed four limitations around it. This spec defines the fixes. You have
**mechanism latitude**: each card states MUST acceptance criteria (binding) and
CANDIDATE mechanisms (non-binding — deviate freely with a stated rationale). If a
MUST itself seems wrong, stop and ask the user; do not silently reinterpret it.

---

## 0. Evidence base (what the audit measured)

Run config (in tree, reusable): `config/test_suite/_wlver_150sta_10ep_d10_allfreq_slipcmp.json`.
Artifacts land in `build/x64-Release/logs/` (`network_events.csv`,
`network_if_slip.csv`, `network_slip_raw.csv`, `main_log.log`) and are overwritten
per run — rerun the config to regenerate. ~50 s/epoch, ~8.5 min total.

Headline numbers (2026-07-21 run, 11 epochs emitted / 0 skipped of 12 synced):

| Quantity | Value |
|---|---|
| IF-TD pool samples → flags | 8 717 → 32 (0.367 %); margins `abs_dev/thr` min 1.19, med 74, max 2066 |
| IF-TD refsat-reset events | 17 (JPLM00USA/G10 ×9, MKEA00USA/G10 ×5, OWMG00NZL/G31 ×3) |
| Raw-confirmed flags (ΔMW ≥ 0.5 WL cyc, or ΔGF ≥ 4 cm, or LLI) | 30 of 49 |
| Unsupported flags (raw series flat) | 14 — **all** the JPLM/MKEA G10 refsat family |
| LLI-set-but-no-jump rows | 352 over 10 epochs, dominated by STK200JPN (99), ISHI00JPN (90), MCIL00JPN (83), CCJ200JPN (80) |
| Card-4 carry slip refloats vs real events | 2 318 vs ≈ 50 (LLI chatter feeding the C3c funnel) |
| D5 innovation re-taps | 1 560 (σ̂₀ = 1.78) |
| Reverse check, raw \|ΔMW\| > 3 cyc | 57 candidates → 39 IF-flagged; misses are gap-coincident re-acquisition arcs (DARW00AUS/G28 ±405 cyc, YKRO00CIV/G06 +47/−60/−17 cyc, MAC100AUS/G04) or code-multipath MW steps with flat phase (NLIB00USA/G05, dev 0.02–0.05 m vs thr 0.107 every epoch) |
| MW/LLI event rows in `network_events.csv` | 0 — structural (see L3), not a null MW result |

The DARW00AUS/G28 excursion **was** caught (dev = 179.47 m vs thr = 2.55 m, 70×) at
the first epoch where one signal pair survived two consecutive epochs — the earlier
misses are the TD chain breaking during re-acquisition, not threshold failures.

The four limitations:

- **L1 — refsat gate false-positive family.** `IfPhaseSlipDetector.cpp:415`: the
  reference-satellite slip check fires on `|median(non-ref DDs)| > 0.5·λ_IF ≈ 5.3 cm`
  with only `n ≥ 2` — fixed threshold, no elevation normalization, no empirical
  scale, no persistence semantics. On a thin signal family (L1C_L2L: census ≈ 3.7
  arcs/station) a persistent sub-slip drift re-fires it **every epoch** (9
  consecutive at JPLM, 5 at MKEA; raw WL/GF flat at ±0.1 cyc, no LLI). Each fire
  refloats both band ambiguities and marks carry chords slipped.
- **L2 — LLI chatter thrashes the carry funnel.** `NetworkSlipQc.cpp:175-187`: the
  MW-drain LLI arm acts on raw RINEX LLI bit-0 unconditionally. Four stations
  assert LLI on ~9 sats every epoch with zero observable jump; via the pre-warm
  `carry_slipped_ambs` insert (`NetworkSlipQc.cpp:146`, active from epoch 1) this
  produced 2 318 carry slip refloats against ≈ 50 real events. In a warm run the
  legacy pending-reseed path would thrash the same stations' ambiguities too.
- **L3 — MW/LLI events invisible when cold.** `NetworkSlipQc.cpp:134-152`:
  `refloat_signal` sets `any = true` only under `carry_warm`
  (`ApplicationNetwork.cpp:1248`: `pl_on && n_emitted ≥ pl_warmup`), and both
  `emitEvent` calls require it — so cold runs write **zero** `mw_wl_slip`/`lli_slip`
  rows even though detections fed the C3c funnel. Asymmetric with the IF-TD driver,
  which emits `action=diagnostic` when cold. MW ↔ IF agreement is unmeasurable from
  the event trace on any cold/short run.
- **L4 — gap-coincident slips are detector-invisible while the TD chain is broken.**
  The estimation side behaved correctly in the audit (broken arcs → chord death /
  fresh float, "0 gap re-pins"; D5 fired), but nothing *measures* whether carry- or
  lane-bridged state ever rides across a break whose raw level shifted. This is an
  instrument-first card, not a mechanism card.

Out of scope (do **not** bundle): the `dd-build` blowup at all-freq/all-channel
(35–43 s/ep vs sub-second at triple/single — DD chord population; belongs to the
LF-1 perf lineage), and root-causing the physical L2L drift at JPLM/MKEA
(characterize opportunistically from `network_if_slip.csv` if it informs Card R
thresholds, but it is not a deliverable).

---

## 1. Ground rules (binding, project-wide)

From `CLAUDE.md` and standing user directives; restated so this spec stands alone:

1. **Default-OFF + byte-identity.** Every behavior change sits behind a new config
   toggle following the `GnssConfig` conventions (private member + getter +
   `jval<>` parse in the matching section helper of
   `src/core/config/GnssConfigLoadNetwork.cpp`, summary-log line). With all new
   toggles OFF, artifacts must be **byte-identical to the pre-change binary on the
   same build config** (same-build golden comparison; never Debug-vs-Release).
   Emission/logging-only changes (Card E) must not alter any numerical artifact.
2. **Release-only data runs.** Debug is for compile checks and `gnss_tests` only.
   Run a renamed exe copy (e.g. `gnss_processor_<card>_run.exe`); pre-kill zombies
   **name-scoped to your own renamed exe only** (a `gnss_processor*` wildcard kills
   concurrent sessions' runs); on LNK1104 assume a concurrent build lock and retry.
3. **Staged test cadence.** 10-epoch smoke first, always. Longer graded runs
   (≥ 1 h warm validation) only after a clean smoke **and on the user's command** —
   report readiness, do not launch them unprompted.
4. **Empirical thresholds only.** Any new gate on raw observables (MW/GF steps,
   LLI corroboration, refsat scale) derives from the data stream itself
   (MAD / Welford / EWMA, cross-satellite or per-(station,pair)) — never from the
   stochastic-model σ. Follow the existing pool-arm pattern
   (`NetworkSlipQc.cpp:249-330`: pooled MAD prior → per-(station,pair) EWMA with
   empirical-Bayes shrinkage, physical integer floor kept in raw metres).
5. **Never hard-exclude measurements.** Fixes qualify, corroborate, deweight, or
   downgrade to diagnostic — they do not drop stations, satellites, or
   observations. (Also: reset-over-repair for bias-less signals — the refloat
   mechanism is already reset-semantics; keep it that way.)
6. **Run-end counters are the ON-path telemetry** (Standing invariant 4). Every
   card adds counters proving its wiring is live; "no crash + clean log" is not
   evidence. Wire them into the existing end-of-run summary block.
7. **Unit tests are additive.** The slip-detector chain currently has **zero**
   dedicated unit coverage — these cards add the first. Follow the existing
   GoogleTest patterns in `tests/` (TestAccess shim where private state must be
   probed); suite must stay green.
8. **Doxygen + CMake.** Public headers keep the project's thorough Doxygen style.
   New `.cpp` files are added to the explicit source list in `CMakeLists.txt`.
9. **Commits**: no Claude co-author trailer.

Shared plumbing note (used by Cards L and G): the raw MW/GF observable computation
exists today inside the `trace_slip_raw` writer
(`ApplicationNetworkAssembly.cpp:689-760`, gated on the trace stream). Refactor it
into a shared helper (compute once per epoch when *any* consumer needs it; trace
output byte-unchanged when only the trace consumes it) rather than duplicating the
math in the drains.

---

## 2. Card R — refsat slip-gate hardening (fixes L1)

**Where**: `IfPhaseSlipDetector.cpp:401-448` (ref-slip block; gate at :415),
`:296-320` (ref selection/retention, `kRefElevThreshDeg = 45°`), `:451-496`
(deferred-mode branch — on the network path `defer_flagging_ = true` and **only**
the ref-slip branch pushes reset keys; those flow through `sta_if_resets`
(`ApplicationNetworkAssembly.cpp:648-652`) into the driver's refsat fold
(`NetworkSlipQc.cpp:378-393`) as `refsat_reset` events + `refloat_pair`).

**Problem restated**: the gate is the only non-empirical decision left in the IF-TD
chain, and it fired 14/14 false positives in the audit while the empirically-scaled
pool arm fired ~0. After each fire the code rebases `prev_grad` by the median
(`:433-434`), so a re-fire next epoch is *evidence of persistent drift, not a
stale-state bug* — the gate cannot distinguish a step (slip) from a drift.

**MUST**:
- R1. The refsat decision becomes scale-aware and empirical, consistent in spirit
  with the pool arm (driver-side `s_eff` per (station,pair) already exists —
  reusing it is the architecturally preferred candidate, but not mandatory).
- R2. Step-vs-drift semantics: a genuine ref slip (a one-epoch common step across
  the non-ref DDs) fires **once**; a persistent sub-slip drift must not produce an
  unbounded per-epoch event/refloat stream on the same (station, sat, pair). Cap,
  de-dup, escalate to re-reference, or discriminate by dispersion — your choice —
  but the audit's 9-consecutive-epoch pattern must be structurally impossible.
- R3. Thin-family honesty: below a minimum non-ref population (audit family was
  2–3 DDs), the median is not a trustworthy common-mode estimate — the gate must
  either widen empirically or defer with a counted skip (no silent behavior).
- R4. Genuine ref slips still fire: the OWMG00NZL/G31 events (LLI-corroborated,
  station re-lock) must survive the rerun. The pool arm is untouched.
- R5. Both detector modes keep working: deferred (network) and self-contained
  (user/kinematic — currently STAGED; do not change its defaults or wiring).
- R6. New toggle(s) under `network.cycle_slip.if_phase_td_ref_*`; default OFF ⇒
  byte-identical; armed in the audit config for validation.
- R7. Counters: refsat fires, refsat suppressed-by-{scale, persistence, thin-family},
  ref re-selections. Unit tests: first coverage of `IfPhaseSlipDetector` —
  synthetic gradient streams exercising step-fires-once, drift-does-not-refire,
  thin-family defer, ref handover offset correctness (`:349-363`).

**CANDIDATES** (non-binding):
- Move refsat adjudication driver-side: detector returns the median + dispersion
  of non-ref DDs per (station, pair); the driver gates against `k · s_eff ·
  elev-map` with its existing empirical scale, and applies persistence rules where
  it already owns event emission. Keeps one empirical-scale authority.
- Dispersion discriminator: fire only when the non-ref DDs agree on a common step
  (`MAD(dd_i − median)` small relative to `|median|`) — a drift shows scatter and
  repeated small exceedance; a slip shows one coherent jump.
- Post-fire escalation: after a fire on (station, pair), force reference
  re-selection next epoch; if the *new* reference immediately "slips" too, the
  common mode is station-local (receiver event) — hand it to the pool/RAIM layer
  rather than re-firing the refsat path.

**Falsifier (10-ep smoke, audit config)**: JPLM00USA/G10 + MKEA00USA/G10 families
(14 events, flat raw) vanish; OWMG00NZL/G31 events persist; pool flags ≈ 32 ± noise;
`network_if_slip.csv` sample population unchanged (8 717 ± sync noise).

---

## 3. Card L — LLI corroboration in the MW drain (fixes L2)

**Where**: `NetworkSlipQc.cpp:100-193` (`networkSlipDrainMwRefloat`; LLI loop
`:175-187`; pre-warm C3c insert `:146`); shared raw-observable helper (§1 note).

**Problem restated**: LLI bit-0 is treated as ground truth. It *is* the only
witness for some real slips (the G23@GAMB precedent in the code comment at
`:120-132` — preserve that), but four receivers assert it continuously without any
phase discontinuity, and through the C3c funnel that fiction reaches the DD carry
from epoch 1, cold or warm.

**MUST**:
- M1. An LLI assertion alone no longer drives refloat/funnel actions at a station
  whose LLI behavior is demonstrably non-credible; at credible stations LLI stays
  decisive (do not require corroboration everywhere — that would forfeit the
  G23@GAMB class of catches where MW/IF genuinely cannot see the slip).
- M2. Credibility/corroboration is empirical (rule 4). No station or satellite is
  excluded from estimation (rule 5) — this card only qualifies the *slip action*.
- M3. Real multi-sat station events survive: the OWMG00NZL re-lock epochs
  (LLI **with** WL jumps, 3 separate epochs in the audit) must still refloat every
  affected satellite. Any station-level credibility state must not veto
  jump-corroborated LLI. (This is why corroboration-per-signal is the preferred
  primitive and rate-masks at most a prefilter.)
- M4. Suppressed LLI assertions are visible, not silent: a diagnostic event row
  (e.g. `lli_chatter` or `lli_slip` with `action=suppressed`) and run-end counters
  — LLI accepted / suppressed / corroborated-by-{MW step, GF step}; plus a one-line
  per-run top-offenders summary (the audit's STK2/ISHI/MCIL/CCJ2 list must be
  reproducible from the log alone).
- M5. The C3c funnel insert (`:146`) obeys the same qualification as the
  pending-reseed path — the audit's 2 318-refloat thrash was *cold*, so gating only
  the warm path is insufficient.
- M6. Toggle under `network.cycle_slip.` (e.g. `mw_lli_corroborate`); default OFF ⇒
  byte-identical; armed in the audit config. Unit tests: chatty-station stream
  (LLI every epoch, flat MW/GF) → suppressed + counted; quiet-station LLI with
  flat observables → still acts (credibility intact); LLI + WL step → acts.

**CANDIDATES**:
- Corroboration primitive: per (station, sat, signal), an LLI assertion is
  confirmed if the shared raw-observable helper shows an MW or GF step above an
  empirical gate (per-station MAD/EWMA of its own epoch-to-epoch steps — the 30 s
  ΔMW noise floor differs per station; the audit saw 0.05–0.2 cyc benign vs 2–200
  cyc real).
- Credibility state: EWMA of the station's LLI-set fraction across its arcs; a
  station sustaining an implausible fraction (audit: ~100 % of tracked sats,
  every epoch) demotes LLI from "decisive" to "corroboration-required". Two-state,
  hysteresis, never "ignore".
- Placement latitude: qualification may live in the drain (reading the helper) or
  in PASS-A next to where `sta_mw_refloat` is filled
  (`ApplicationNetworkAssembly.cpp:629-637`) — keep the serial, station-ordered
  determinism of the drain either way.

**Falsifier (10-ep smoke)**: Card-4 slip-refloat counter collapses from 2 318 to
the same order as (real events × affected chords); JPN stations disappear from the
refloat population but appear in the suppressed counters; OWMG re-lock epochs
unchanged. Warm validation (≥ 1 h, **on command**): no ambiguity-variance sawtooth
at the four JPN stations, D5 re-tap count at those stations drops accordingly.

---

## 4. Card E — diagnostic MW/LLI event emission when cold (fixes L3)

**Where**: `NetworkSlipQc.cpp:134-193`; `carry_warm` at
`ApplicationNetwork.cpp:1248-1250`; events trace schema
`NetworkRunTraces.cpp:279-291` (opt-in via `network.trace_events`).

**MUST**:
- E1. MW-detect and LLI detections emit event rows when the carry is cold, with
  `action=diagnostic` (warm keeps `refloat`) — mirroring the IF-TD driver's
  existing cold behavior (`NetworkSlipQc.cpp:338`). Schema unchanged (the `action`
  column already carries the distinction).
- E2. Restructure `refloat_signal`'s return so "matched ambiguities for this
  (sat, signal)" and "refloated" are separate facts; emission and the
  `n_mw`/`n_lli` log counters key on *detections*, refloat accounting stays
  warm-only (separate counter). The `Network[MW detect]` summary line reports cold
  detections instead of printing nothing.
- E3. Numerical no-op, strictly: state mutations (`markAmbiguityPendingReseed`,
  `carry_slipped_ambs`) are untouched by this card. With `trace_events` off,
  artifacts byte-identical; with it on, only `network_events.csv` rows and log
  text may differ from pre-change.
- E4. This card is what makes the MW ↔ IF agreement measurable from
  `network_events.csv` alone on short runs — the rerun must show nonzero MW-side
  rows wherever PASS-A detected (the audit could only infer them from raw traces).
  No new toggle needed if E3 holds (emission rides the existing `trace_events`
  opt-in); if you find any numerical coupling, stop and gate it.

Sequencing note: land Card E **first** — Cards R and L validation reads the event
rows it unlocks.

---

## 5. Card G — gap-coincident slip instrumentation (addresses L4, instrument-first)

**Framing**: Standing invariant 3 — a mechanism here is *not yet justified by
data*. The audit showed correct reset semantics on broken arcs (chord
death/rebirth, 0 gap re-pins) and a live D5 backstop. The unmeasured residual risk
is state that *bridges* a tracking break — the Card-4 DD carry across
`phase_leveling_max_gap_s`-tolerated gaps, and the WL-3 lane-carry bridge re-key —
riding over a level shift. Instrument, measure on a warm run, then decide.

**MUST**:
- G1. Counters (run-end + per-event diagnostic rows via `trace_events`):
  (a) arcs re-established after ≥ 1 missed epoch whose raw MW (and GF) level
  shifted across the break by more than an empirical gate (shared helper, §1) —
  count total resumptions and shifted resumptions separately;
  (b) among shifted resumptions, how many re-entered with bridged carry/lane state
  vs a fresh float (the poison case vs the benign case);
  (c) D5 re-taps whose (station, sat, epoch) coincides with a raw step — making
  the D5 counter attributable (the audit's 1 560 was un-decomposable).
- G2. Zero mechanism changes in this card. Default-OFF toggle for the
  instrumentation only if it touches a hot path; pure counter/trace additions
  under existing trace flags need no toggle but still obey E3-style byte-identity
  with traces off.
- G3. Decision rule, recorded in the card's report: if shifted-resumption-with-
  bridged-state ≈ 0 on the warm graded run, the mechanism is **deferred with
  data** (write the number down — that is the required triage artifact). If
  nonzero, a follow-up card marks such arcs slipped at re-entry (C3c funnel) or
  blocks the lane bridge — spec that card then, with the measurements in hand.

---

## 6. Validation harness (shared)

1. **Grader**: port the audit's analysis into `scripts/grade_slip_agreement.py`
   (session scratchpad scripts do not persist). Required behavior — inputs: a log
   dir; outputs: (a) event counts by tier/type from `network_events.csv`;
   (b) IF ↔ MW/LLI matching on (station, sat) with ± 1-epoch tolerance;
   (c) per-epoch flag histogram (bunch check) + top stations/sats;
   (d) sample stats from `network_if_slip.csv` (opportunities, flags, margin
   distribution vs `thr_m`); (e) raw adjudication from `network_slip_raw.csv`
   (support = |ΔMW| > 0.5 WL cyc or |ΔGF| > 4 cm or LLI; classify flags
   supported / unsupported / no-series); (f) reverse sweep at |ΔMW| gates 1.5/2/3
   cyc with hit/miss lists. Cross-check its totals once against the numbers in §0.
2. **Gates per card**: Release build; `gnss_tests` green (additive cases); OFF
   byte-identity vs a pre-change same-build golden on the audit config with new
   toggles stripped; ON 10-ep smoke meeting the card's falsifier; counters nonzero
   where the card claims wiring (invariant 4).
3. **Warm validation** (Cards L and G need it; R benefits): ≥ 1 h leg on the same
   station set — prepared, reported ready, launched **only on the user's command**.
4. **Run mechanics**: `build/x64-Release`, renamed exe per card, name-scoped
   zombie pre-kill, lock-retry on LNK1104. The audit config already carries
   `parallel_carry=true / parallel_reduce=false` — leave that as-is.

## 7. Acceptance summary

| Card | Toggle (default OFF) | OFF gate | ON falsifier (10-ep smoke) |
|---|---|---|---|
| E (first) | none if numerically inert, else gate it | byte-identical, traces off | MW/LLI rows appear with `action=diagnostic`; counters count detections |
| R | `network.cycle_slip.if_phase_td_ref_*` | byte-identical | JPLM/MKEA G10 family gone; OWMG G31 survives; pool arm unchanged |
| L | `network.cycle_slip.mw_lli_corroborate` (name latitude) | byte-identical | carry refloats 2 318 → ≈ real-event order; JPN stations move to suppressed counters; OWMG re-locks still act |
| G | counters/trace only | byte-identical, traces off | counters populate; decision rule executed on warm run data |
