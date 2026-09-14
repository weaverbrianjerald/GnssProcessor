# slip-QC — the slip-QC hardening spec — evidence base, the binding ground rules, cards R / L / E / G, the shared harness, and what is still owed

**STATUS**

- **id:** slip-QC
- **topic:** slip
- **status:** reference
- **summary:** the slip-QC hardening spec — evidence base, the binding ground rules, cards R / L / E / G, the shared harness, and what is still owed
- **dates in the text:** 2026-07-21 → 2026-09-09
- **knobs / posture:** `network.cycle_slip`, `network.trace_events`, `network.cycle_slip.mw_lli_corroborate`, `network.cycle_slip.gap_instrument`, `network.cycle_slip.if_phase_td_ref_gate`, `network.cycle_slip.gap_reentry_relevel`, `network.mw_wl.d5_dump`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/fldf3_pl2`, `runs/fldf3_pl2_gapfree2`
- **source:** `docs/archive/SPEC_slip_qc_hardening.md` — (preamble), 0, 1, 2, 3, 4, 5, 6, 7, 11

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_slip_qc_hardening.md -->
# SPEC — Network slip-QC hardening (post IF-TD ↔ MW agreement audit, 2026-07-21)

**Intent**: the 2026-07-21 slip-detector cross-check (150 sta, GPS-only, `frequency_mode=all` +
`channel_mode=all`, em10, 11 epochs, both `network.cycle_slip.{mw_detect,if_phase_td}` armed, full
MW-AR arm live) found the IF-phase-TD pool detector **clean** (0.37 % flag rate, decisive margins, no
bunching) and exposed four limitations around it (L1–L4, §0). Cards state MUST criteria (binding) and
CANDIDATE mechanisms (non-binding — deviate with a stated rationale; if a MUST itself seems wrong, stop
and ask the user). Read `CLAUDE.md` (build, layout, **Standing invariants**) first.

> **Structure (consolidated 2026-08-14).** §0–§7 are the original spec, **section-pinned**:
> `ApplicationNetworkAssembly.cpp:1402` cites §1, `ApplicationNetworkSetup.cpp:760` §2,
> `ApplicationNetwork.cpp:1881` §5, `tests/test_if_phase_slip_detector.cpp:6` "§2, MUST R7",
> `tests/test_network_slip_qc.cpp:5` §3, and `SPEC_beidou_w6_datum.md` (ground-rules paragraph) /
> `SPEC_warm_resume_event_card.md` (status header) cite §1 as the project-wide ground-rules block —
> **cited without line numbers deliberately: both docs are under active compression and the pin has
> already gone stale three times (`:9` → `:12` → line 3).** **Never renumber or retitle.** §8–§10 absorb three siblings (now `docs/attic/`); §11 is the status ledger.
>
> **Absorbed-source section map** (old § → here). `slip_qc_hardening_report_2026-07-21.md` §X → §8.X,
> no exceptions (§4.1→§8.4.1, §4.2→§8.4.2, §4.4→§8.4.4, §5→§8.5). `SPEC_slip_qc_followups.md` §N →
> §9.N. `slip_qc_d5a_report_2026-07-22.md` §X → §10.X (§2.2→§10.2.2, §2.3→§10.2.3, §2.4 folded into
> §10.2.1, §5.1→§10.5.1, §5.2→§10.5.2, §5.4→§10.5.4, §5.5→§10.5.5) **with two exceptions**: d5a §6
> (Card-W run mechanics) → **§9.2**, d5a §8 (commit logistics) → **§9.4**. Hence no §10.6 / §10.8.
>
> **Provenance.** Every `runs/<name>` and `build/x64-Release/logs/` path below is absent from the
> working tree: `runs/` was archived to `D:\GnssProcessor_archive\2026-08-12\runs\` on 2026-08-12 and
> the build trees were regenerated under the VS 2026 migration the same day. **The 2026-07-21/22
> goldens are gone**; a re-run also comes from a different compiler (MSVC 19.51.36256) and invariant #6
> forbids reading a cross-build delta as a treatment effect — take a fresh same-build baseline before
> using any table here as an A/B reference. Build: `CLAUDE.md` Build section / `scripts/vs2026_build.ps1`.
>
> **Anchor-drift map.** Anchors below are HEAD, re-grepped 2026-08-15; the 2026-07-21/22 text cited
> pre-decomposition lines. Use this to resolve an old citation.
>
> | file | cited 2026-07-21/22 → live at HEAD |
> |---|---|
> | `NetworkSlipQc.cpp` | `NetworkSlipQc.cpp:100-193` → `:100-391` (`networkSlipDrainMwRefloat`); `NetworkSlipQc.cpp:134-152` / `NetworkSlipQc.cpp:134-193` → `:141-391` (Card E/L drain body, `refloat_signal` `:147-166`); `NetworkSlipQc.cpp:146` / `NetworkSlipQc.cpp:146-150` → `:160` (pre-warm C3c `carry_slipped_ambs` insert); `NetworkSlipQc.cpp:175-187` → `:276-379` (MW-drain LLI arm); `NetworkSlipQc.cpp:249-330` → `:447-573` (IF-TD pool arm, empirical-Bayes shrink `:515-521`); `NetworkSlipQc.cpp:338` → `:534-537` (IF-TD cold `action=diagnostic` emit); `NetworkSlipQc.cpp:378-393` → `:577-599` (refsat fold) |
> | `IfPhaseSlipDetector.cpp` | `IfPhaseSlipDetector.cpp:415` / `IfPhaseSlipDetector.cpp:401-448` → `:617` / `:598-687` (legacy ref-slip candidate gate + block) |
> | `ApplicationNetwork.cpp` | `ApplicationNetwork.cpp:1248` / `ApplicationNetwork.cpp:1248-1250` → `:1428-1430` (`carry_warm`); `ApplicationNetwork.cpp:5255-5290` → `NetworkInstallCarry.cpp:745`–`:840` (`d5_dump` writer) |
> | `ApplicationNetworkAssembly.cpp` | `ApplicationNetworkAssembly.cpp:629-637` / `ApplicationNetworkAssembly.cpp:648-652` → `:1366` / `:1381` (`sta_mw_refloat` / `sta_if_resets`); `ApplicationNetworkAssembly.cpp:689-760` → `:1481`+ (`trace_slip_raw` writer) |
> | `NetworkRunTraces.cpp` | `NetworkRunTraces.cpp:279-291` → `:643` header, `:953` emit (`network_events.csv` schema) |

---

## 0. Evidence base (what the audit measured)

Config (in tree, reusable): `config/test_suite/_wlver_150sta_10ep_d10_allfreq_slipcmp.json`.
Artifacts in `build/x64-Release/logs/` (`network_events.csv`, `network_if_slip.csv`,
`network_slip_raw.csv`, `main_log.log`), overwritten per run. ~50 s/epoch, ~8.5 min total.
2026-07-21 run, 11 epochs emitted / 0 skipped of 12 synced:

| Quantity | Value |
|---|---|
| IF-TD pool samples → flags | 8 717 → 32 (0.367 %); margins `abs_dev/thr` min 1.19, med 74, max 2066 |
| IF-TD refsat-reset events | 17 (JPLM00USA/G10 ×9, MKEA00USA/G10 ×5, OWMG00NZL/G31 ×3) |
| Raw-confirmed flags (ΔMW ≥ 0.5 WL cyc, or ΔGF ≥ 4 cm, or LLI) | 30 of 49 |
| Unsupported flags (raw series flat) | 14 — **all** the JPLM/MKEA G10 refsat family |
| LLI-set-but-no-jump rows | 352 over 10 epochs (STK200JPN 99, ISHI00JPN 90, MCIL00JPN 83, CCJ200JPN 80) — ⛔ **FALSIFIED, §8.2 F1**: LLI **bit 2** (antispoofing), never tested by the drain's `lli & 1`. Bit-0 census **122 rows**: OWMG00NZL 33, MAC100AUS 24, MCIL00JPN 11, WARK00NZL 9, ASCG00SHN 9 |
| Card-4 carry slip refloats vs real events | 2 318 vs ≈ 50 — count right; ⛔ attribution "(LLI chatter feeding the C3c funnel)" **FALSIFIED, §8.2 F2**: D5 contributed 1 560, the coarse layer ≈ 154 |
| D5 innovation re-taps | 1 560 (σ̂₀ = 1.78) |
| Reverse check, raw \|ΔMW\| > 3 cyc | 57 → 39 IF-flagged; misses are gap-coincident re-acquisition arcs (DARW00AUS/G28 ±405 cyc, YKRO00CIV/G06 +47/−60/−17 cyc, MAC100AUS/G04) or code-multipath MW steps with flat phase (NLIB00USA/G05, dev 0.02–0.05 m vs thr 0.107 every epoch) |
| MW/LLI event rows in `network_events.csv` | 0 — structural (L3), not a null MW result |

DARW00AUS/G28 **was** caught (dev 179.47 m vs thr 2.55 m, **70×**) at the first epoch where one signal
pair survived two consecutive epochs — the earlier misses are the TD chain breaking during
re-acquisition, not threshold failures. NLIB00USA/G05's 2–4 cyc MW-only steps with flat phase are code
multipath, correctly unflagged.

| id | limitation |
|---|---|
| 1 | **L1 — refsat gate false-positive family.** `IfPhaseSlipDetector.cpp:617` fires on `\|median(non-ref DDs)\| > 0.5·λ_IF ≈ 5.3 cm` at only `n ≥ 2` (`:609`): fixed threshold, no elevation normalization, no empirical scale, no persistence. On a thin family (L1C_L2L census ≈ **3.7 arcs/station**) a persistent sub-slip drift re-fires it every epoch — 9 consecutive at JPLM, 5 at MKEA, raw WL/GF flat at ±0.1 cyc, no LLI. Each fire refloats both band ambiguities and marks carry chords slipped. |
| 2 | **L2 — LLI chatter thrashes the carry funnel.** The MW-drain LLI arm (`NetworkSlipQc.cpp:276-379`) acts on raw RINEX LLI bit-0 unconditionally, and via the pre-warm `carry_slipped_ambs` insert (`:160`, active from epoch 1) that reaches the DD carry cold or warm. ⛔ The audit's causal story (four JPN stations, ~9 sats/epoch, 2 318 refloats) is **FALSIFIED by §8.2 F1/F2**. The DEFECT CLASS survives — real bit-0 chatterers OWMG00NZL / MAC100AUS — but sized at ~122 rows, and Card L cannot move the 2 318. |
| 3 | **L3 — MW/LLI events invisible when cold.** `refloat_signal` (`NetworkSlipQc.cpp:147-166`) set `refloated` only under `carry_warm` (`ApplicationNetwork.cpp:1428`: `pl_on && n_emitted ≥ pl_warmup`) and both `emitEvent` calls required it, so cold runs wrote **zero** `mw_wl_slip`/`lli_slip` rows even though detections fed the C3c funnel — asymmetric with the IF-TD driver, which emits `action=diagnostic` when cold (`:534-537`). MW ↔ IF agreement was unmeasurable from the event trace on any cold/short run. |
| 4 | **L4 — gap-coincident slips are detector-invisible while the TD chain is broken.** Estimation behaved correctly (broken arcs → chord death / fresh float, "0 gap re-pins"; D5 fired), but nothing *measures* whether carry- or lane-bridged state rides across a break whose raw level shifted. Instrument-first card, not a mechanism card. |

Out of scope (do **not** bundle): the `dd-build` blowup at all-freq/all-channel (35–43 s/ep vs
sub-second at triple/single — DD chord population; LF-1 perf lineage), and root-causing the physical L2L
drift at JPLM/MKEA (characterize opportunistically from `network_if_slip.csv` if it informs Card R
thresholds; not a deliverable).

---

## 1. Ground rules (binding, project-wide)

From `CLAUDE.md` and standing user directives; restated so this spec stands alone.

| # | rule |
|---|---|
| 1 | **Default-OFF + byte-identity.** Every behavior change sits behind a new toggle following the `GnssConfig` conventions (private member + getter + `jval<>` parse in the matching section helper of `src/core/config/GnssConfigLoadNetwork.cpp`, summary-log line). All toggles OFF ⇒ artifacts **byte-identical to the pre-change binary on the same build config**; never Debug-vs-Release. Emission/logging-only changes (Card E) must not alter any numerical artifact. |
| 2 | **Release-only data runs.** Debug is for compile checks and `gnss_tests`. Renamed exe copy (`gnss_processor_<card>_run.exe`); pre-kill zombies **name-scoped to your own renamed exe only** (a `gnss_processor*` wildcard kills concurrent sessions' runs); on LNK1104 assume a build lock, retry. |
| 3 | **Staged test cadence.** 10-epoch smoke first, always; ≥ 1 h graded runs only after a clean smoke **and on the user's command** — report readiness, never launch unprompted. |
| 4 | **Empirical thresholds only.** Any new gate on raw observables (MW/GF steps, LLI corroboration, refsat scale) derives from the data stream itself (MAD / Welford / EWMA, cross-satellite or per-(station,pair)), never from the stochastic-model σ. Follow the pool-arm pattern (`NetworkSlipQc.cpp:447-573`: pooled MAD prior → per-(station,pair) EWMA with empirical-Bayes shrinkage at `:515-521`, physical integer floor kept in raw metres). |
| 5 | **Never hard-exclude measurements.** Fixes qualify, corroborate, deweight, or downgrade to diagnostic — never drop stations, satellites, or observations. Reset-over-repair for bias-less signals: the refloat mechanism is already reset-semantics; keep it that way. |
| 6 | **Run-end counters are the ON-path telemetry** (invariant #4); "no crash + clean log" is not evidence. |
| 7 | **Unit tests are additive.** The slip-detector chain had **zero** dedicated coverage — these cards add the first (TestAccess shim where private state must be probed); suite stays green. |
| 8 | **Doxygen + CMake.** Headers keep the project's Doxygen style; new `.cpp` files go in the explicit `CMakeLists.txt` source list. |
| 9 | **Commits**: no Claude co-author trailer. |

Shared plumbing note (Cards L and G): the raw MW/GF observable computation lived inside the
`trace_slip_raw` writer (`ApplicationNetworkAssembly.cpp:1481`+, gated on the trace stream). Refactor into
a shared helper — computed once per epoch when *any* consumer needs it, trace output byte-unchanged when
only the trace consumes it — rather than duplicating the math in the drains. *(Built as
`include/slip/NetworkRawObservables.hpp` + `src/core/slip/NetworkRawObservables.cpp`; driver call site
`ApplicationNetworkAssembly.cpp:1402`, which cites this section; qualifier entry `:1416`.)*

---

## 2. Card R — refsat slip-gate hardening (fixes L1)

**Where**: `IfPhaseSlipDetector.cpp:598-687` (ref-slip block; legacy candidate filter `:617`, `n ≥ 2`
`:609`, median rebase `:669-671`), `:296-345` (ref selection/retention; `kRefElevThreshDeg = 45°`
declared at `IfPhaseSlipDetector.hpp:314`, tested `:308`), `:690-694` (deferred-mode branch — on the
network path `defer_flagging_ = true` and **only** the ref-slip branch pushes reset keys, which flow
through `sta_if_resets` (`ApplicationNetworkAssembly.cpp:1381`) into the driver's refsat fold
(`NetworkSlipQc.cpp:577-599`) as `refsat_reset` events + `refloat_pair`).

**Problem**: the only non-empirical decision left in the IF-TD chain; 14/14 false positives while the
empirically-scaled pool arm fired ~0. Each fire rebases `prev_grad` by the median, so a re-fire next
epoch is *evidence of persistent drift, not a stale-state bug* — **the gate cannot distinguish a step
from a drift.**

**MUST**:

| id | criterion |
|---|---|
| **R1** | The refsat decision becomes scale-aware and empirical, consistent with the pool arm (driver-side `s_eff` per (station,pair) exists — reuse preferred, not mandatory). |
| **R2** | Step-vs-drift: a genuine ref slip (one-epoch common step across the non-ref DDs) fires **once**; a persistent sub-slip drift must not produce an unbounded per-epoch event/refloat stream on the same (station, sat, pair). Cap, de-dup, escalate to re-reference, or discriminate by dispersion — the 9-consecutive-epoch pattern must be **structurally impossible**. |
| **R3** | Thin-family honesty: below a minimum non-ref population (audit family 2–3 DDs) **the median is untrustworthy** — widen empirically or defer with a counted skip; no silent behavior. |
| **R4** | Genuine ref slips still fire: OWMG00NZL/G31 (LLI-corroborated station re-lock) must survive the rerun. The pool arm is untouched. |
| **R5** | Both detector modes keep working: deferred (network) and self-contained (user/kinematic — STAGED; do not change its defaults or wiring). |
| **R6** | New toggle(s) under `network.cycle_slip.if_phase_td_ref_*`; default OFF ⇒ byte-identical; armed in the audit config for validation. |
| **R7** | Counters: refsat fires, suppressed-by-{scale, persistence, thin-family}, ref re-selections. Unit tests: first `IfPhaseSlipDetector` coverage — synthetic gradient streams for step-fires-once, drift-does-not-refire, thin-family defer, ref handover offset correctness (`:546-557`). |

**CANDIDATES → outcomes** (as built, §8.3): driver-side adjudication on one empirical-scale authority —
**ADOPTED in spirit** (empirical-Bayes `s_eff`); dispersion discriminator (`MAD(dd_i − median)` small vs
`|median|`) — **ADOPTED** as the coherence test, but it needed the incoherent SPLIT to be safe, since
coherence alone destroys the very event R4 protects (§8.2 F5); post-fire escalation to a station-local
hand-off — **FALSIFIED, not built** (§8.2 F4), branch and counter REMOVED rather than shipped
permanently zero.

**Falsifier (10-ep smoke, audit config)**: JPLM/MKEA G10 families (14 events, flat raw) vanish;
OWMG00NZL/G31 persists; pool flags ≈ 32 ± noise; sample population unchanged (8 717 ± sync noise).
**Result: §8.4.4 — PARTIAL, 14 → 4.**

---

## 3. Card L — LLI corroboration in the MW drain (fixes L2)

**Where**: `NetworkSlipQc.cpp:100-391` (`networkSlipDrainMwRefloat`; LLI loop `:276-379`,
qualification/suppression `:321-348`; `refloat_signal` `:147-166`, C3c insert `:160`); shared
raw-observable helper (§1).

**Problem**: LLI bit-0 is treated as ground truth. It *is* the only witness for some real slips (the
G23@GAMB precedent preserved in the code comment at `:130-135`), but some receivers assert it
continuously with no phase discontinuity, and through the C3c funnel that fiction reaches the DD carry
from epoch 1. ⛔ The audit's named offenders (STK2/ISHI/MCIL/CCJ2) are the wrong population — §8.2 F1.

**MUST**:

| id | criterion |
|---|---|
| **M1** | An LLI assertion alone no longer drives refloat/funnel actions at a station whose LLI behavior is demonstrably non-credible; at credible stations LLI stays decisive — corroboration everywhere would forfeit the G23@GAMB class where MW/IF cannot see the slip. |
| **M2** | Credibility/corroboration is empirical (rule 4); no station or satellite is excluded from estimation (rule 5) — this card qualifies only the *slip action*. |
| **M3** | Real multi-sat station events survive: the OWMG00NZL re-lock epochs (LLI **with** WL jumps, 3 separate epochs) must still refloat every affected satellite; station-level credibility must not veto jump-corroborated LLI — hence corroboration-per-signal is the preferred primitive and rate-masks are at most a prefilter. |
| **M4** | Suppressed assertions are visible: a diagnostic event row (`lli_chatter`, or `lli_slip` with `action=suppressed`), run-end counters (LLI accepted / suppressed / corroborated-by-{MW step, GF step}), and a per-run top-offenders line reproducible from the log alone. |
| **M5** | The C3c funnel insert obeys the same qualification as the pending-reseed path — the 2 318 refloat thrash was *cold*, so gating only the warm path is insufficient. |
| **M6** | Toggle under `network.cycle_slip.` (e.g. `mw_lli_corroborate`); default OFF ⇒ byte-identical; armed in the audit config. Unit tests: chatty-station stream (LLI every epoch, flat MW/GF) → suppressed + counted; quiet-station LLI with flat observables → still acts; LLI + WL step → acts. |

**CANDIDATES → outcomes**: per-(station, sat, signal) corroboration from an MW/GF step above an
empirical gate — **ADOPTED** (per-station robust EWMA of that station's own step magnitudes; audit saw
0.05–0.2 cyc benign vs 2–200 cyc real); two-state credibility EWMA of the station's LLI-set fraction,
hysteresis, never "ignore" — **ADOPTED** verbatim; placement in the drain vs PASS-A next to
`sta_mw_refloat` (`ApplicationNetworkAssembly.cpp:1366`) — built in the **drain**, preserving its
serial station-ordered determinism.

**Falsifier (10-ep smoke)**: ⛔ two of three clauses are DEAD (results §8.4.4). "Carry refloats 2 318 →
(real events × affected chords)" assumed a population that does not exist: **NOT MET, NOT ACHIEVABLE BY
THIS CARD** — the armed run moved it 2 318 → **2 399**, the *wrong* way, for a correct reason (more chords
survive to be judged by D5). "JPN stations move to suppressed" is **VOID** — never in the arm. What
stands: OWMG re-lock epochs unchanged (**MET**), plus the warm checks (≥ 1 h, **on command**) — no
ambiguity-variance sawtooth at the demoted stations, D5 re-tap count there drops — which became Card W
gate W3 (§9.2) and were **never run**.

---

## 4. Card E — diagnostic MW/LLI event emission when cold (fixes L3)

**Where**: `NetworkSlipQc.cpp:141-391`; `carry_warm` at `ApplicationNetwork.cpp:1428-1430`; events
schema `NetworkRunTraces.cpp:643` (opt-in via `network.trace_events`, emit helper `:953`).

**MUST**:

| id | criterion |
|---|---|
| **E1** | MW-detect and LLI detections emit event rows when the carry is cold, `action=diagnostic` (warm keeps `refloat`) — mirroring the IF-TD driver (`NetworkSlipQc.cpp:536`). Schema unchanged (the `action` column already carries the distinction). |
| **E2** | Restructure `refloat_signal`'s return so "matched ambiguities for this (sat, signal)" and "refloated" are separate facts; emission and the `n_mw`/`n_lli` log counters key on *detections*, refloat accounting stays warm-only (separate counter). The `Network[MW detect]` summary line reports cold detections instead of printing nothing. |
| **E3** | Numerical no-op, strictly: state mutations (`markAmbiguityPendingReseed`, `carry_slipped_ambs`) untouched. With `trace_events` off, artifacts byte-identical; with it on, only `network_events.csv` rows and log text may differ. |
| **E4** | Makes MW ↔ IF agreement measurable from `network_events.csv` alone on short runs — the rerun must show nonzero MW-side rows wherever PASS-A detected. No new toggle if E3 holds (emission rides the existing `trace_events` opt-in); if you find any numerical coupling, stop and gate it. |

Sequencing: land Card E **first** — Cards R and L validation reads the event rows it unlocks.

---

## 5. Card G — gap-coincident slip instrumentation (addresses L4, instrument-first)

**Framing**: invariant #3 — a mechanism here is *not yet justified by data*. The audit showed correct
reset semantics on broken arcs (chord death/rebirth, 0 gap re-pins) and a live D5 backstop. The
unmeasured residual risk is state that *bridges* a tracking break — the Card-4 DD carry across
`phase_leveling_max_gap_s`-tolerated gaps, and the WL-3 lane-carry bridge re-key — riding over a level
shift. Instrument, measure on a warm run, then decide.

**MUST**:

| id | criterion |
|---|---|
| **G1** | Counters (run-end + per-event rows via `trace_events`): (a) arcs re-established after ≥ 1 missed epoch whose raw MW (and GF) level shifted across the break by more than an empirical gate (shared helper, §1) — total and shifted resumptions counted separately; (b) among shifted resumptions, how many re-entered with bridged carry/lane state vs a fresh float (poison vs benign); (c) D5 re-taps whose (station, sat, epoch) coincides with a raw step — making the D5 counter attributable (the audit's 1 560 was un-decomposable). |
| **G2** | Zero mechanism changes. Default-OFF toggle only if the instrumentation touches a hot path; pure counter/trace additions under existing trace flags need no toggle but still obey E3-style byte-identity with traces off. |
| **G3** | Decision rule, recorded in the card's report: shifted-resumption-with-bridged-state ≈ 0 on the warm graded run ⇒ mechanism **deferred with data** (write the number down — the required triage artifact). Nonzero ⇒ a follow-up card marks such arcs slipped at re-entry (C3c funnel) or blocks the lane bridge — spec it then, with the measurements. |

⚠ G3's rule is **weaker than it reads** (§10.4 H2): only as strong as the *unmeasurable*-and-bridged
count is small, and that number was invisible until the arc ledger was split three ways. The warm arm
that would have executed G3 (gate W1, §9.2) was never launched.

---

## 6. Validation harness (shared)

| # | item |
|---|---|
| 1 | **Grader**: port the audit's analysis into `scripts/grade_slip_agreement.py` (scratchpad scripts do not persist). Input a log dir; output (a) event counts by tier/type from `network_events.csv`; (b) IF ↔ MW/LLI matching on (station, sat) with ± 1-epoch tolerance; (c) per-epoch flag histogram (bunch check) + top stations/sats; (d) sample stats from `network_if_slip.csv` (opportunities, flags, margin distribution vs `thr_m`); (e) raw adjudication from `network_slip_raw.csv` (support = \|ΔMW\| > 0.5 WL cyc or \|ΔGF\| > 4 cm or LLI; classify supported / unsupported / no-series); (f) reverse sweep at \|ΔMW\| gates 1.5/2/3 cyc with hit/miss lists. Cross-check totals once against §0. *(Card D5-A later added section (g), D5 re-tap attribution — §10.2.2; the run-end-counter echo moved to (h).)* |
| 2 | **Gates per card**: Release build; `gnss_tests` green (additive cases); OFF byte-identity vs a pre-change same-build golden on the audit config with new toggles stripped; ON 10-ep smoke meeting the card's falsifier; counters nonzero where the card claims wiring (invariant #4). |
| 3 | **Warm validation** (Cards L and G need it; R benefits): ≥ 1 h leg on the same station set — prepared, reported ready, launched **only on the user's command**. *(Became Card W, §9.2 — PREPARED 2026-07-22, NEVER LAUNCHED.)* |
| 4 | **Run mechanics**: `build/x64-Release`, renamed exe per card, name-scoped zombie pre-kill, lock-retry on LNK1104. The audit config already carries `parallel_carry=true / parallel_reduce=false` — leave that as-is. |

## 7. Acceptance summary

| Card | Toggle (default OFF) | OFF gate | ON falsifier (10-ep smoke) | LANDED 2026-07-21/22 |
|---|---|---|---|---|
| E (first) | none if numerically inert, else gate it | byte-identical, traces off | MW/LLI rows appear with `action=diagnostic`; counters count detections | **MET, no toggle needed** — 67 WL + 87 LLI detections that previously wrote nothing; `slip refloats` unchanged at 2 318 |
| R | `network.cycle_slip.if_phase_td_ref_*` | byte-identical | JPLM/MKEA G10 family gone; OWMG G31 survives; pool arm unchanged | **PARTIAL 14 → 4** (−71 %), longest run 1; OWMG survives on the station-event path; pool arm 32 → 33 on 8 717 → 8 727 |
| L | `network.cycle_slip.mw_lli_corroborate` (name latitude) | byte-identical | carry refloats 2 318 → ≈ real-event order; JPN stations move to suppressed counters; OWMG re-locks still act | **VOID / NOT ACHIEVABLE** on the first two (§8.2 F1/F2); OWMG **MET**; the card demoted the real bit-0 chatterers instead |
| G | counters/trace only (`network.cycle_slip.gap_instrument`) | byte-identical, traces off | counters populate; decision rule executed on warm run data | counters **MET**; decision rule **NOT executed** — Card W (§9.2) never launched |

---

## 11. Current status and what is still owed (2026-08-14)

**Landed and live** (all committed; every knob default-OFF and fingerprint-SAFE):

| knob | default | status |
|---|---|---|
| `network.cycle_slip.if_phase_td_ref_gate` (+ `_k` 6.0, `_min_n` 4, `_thin_widen` 3.0, `_coherence` 0.5, `_max_consecutive` **1**, `_ewma_alpha` 0.15, `_shrink_kappa` **20.0**, `_floor_frac` 0.5) | **OFF** | Card R — validated 10-ep cold; warm takeover (W2) unmeasured |
| `network.cycle_slip.mw_lli_corroborate` (+ `mw_lli_min_epochs` **1**, `_chatter_frac_hi/lo`, `_ewma_alpha`, `_k_mw`, …) | **OFF** | Card L — validated 10-ep cold; sawtooth check (W3) unmeasured |
| `network.cycle_slip.gap_instrument` | **OFF** | Card G — counters live; G3 decision rule (W1) **not executed** |
| `network.cycle_slip.gap_reentry_relevel` (+ `_lli` **true**, `_unmeasurable` **false**) | **OFF** | Card G **RESPONSE** — §G3/W1, built 2026-09-09 once the decision rule came back nonzero (`runs/fldf3_pl2`: 88 shifted-AND-bridged arcs + 24 unmeasurable-bridged over 12 h at 63 stations).  SOLVE-AFFECTING, unlike every other row in this table: it marks both bands of a slipped re-entry, freeing the zero-q leveling level for one epoch.  Arms the resumption collection itself (does NOT need `gap_instrument`); on the leveling vehicle it needs `cycle_slip.relevel_free_epoch` to ACT, and the loader WARNs rather than disarming so the loss is counted (`gap_reentry_no_relevel`) instead of hidden.  ⚠ **Its first cut walked the LIVE ambiguity map (the MW/LLI drain's `refloat_signal` shape) and was therefore INERT on 100 % of its population — item 4 of "Still OWED" below, reproduced inside the very card that was meant to answer it.**  Fixed 2026-09-09: the ambiguity key is SYNTHESISED (`StateKeys::makeAmbiguityKey`, the inject site's own format) and the "anything to protect?" test reads the leveling CARRY (`LocalCarry` inside `phase_leveling_max_gap_s`) or the `carry_invariant` vehicle — never the state.  **GRADED 2026-09-09 — see §12**: round 2 (`runs/fldf3_pl2_gapfree2`) is proven live (SHIFTED→bridged **88 → 0**, **129 arcs freed**, 129/129 state-absent) and every level/DD instrument improves, but the float consumer pays **+56 % settled rms** (3.178 → 4.946 m) and WROC00POL's settled ZWD span goes 3.6 → 15.6 m — the bare refloat's re-coupling edge costs more than the 88 poisons.  Ledger: **KEEP_OFF** on field lineages (regime unchanged); the instrument half is kept. |
| `network.mw_wl.d5_dump` | pre-existing | D2's `raw_stepped` column rides it — **no new toggle** |
| Card E | **no toggle** | numerically inert by construction; emission rides `network.trace_events` |

⚠ The C++ header default `RefGateParams::max_consecutive` is **1** as of H1 (it was 2);
`HeaderDefaultsMatchTheShippedConfigDefaults` pins the whole struct against its `SlipQcConfig` twin.

**Still OWED.**

| # | item |
|---|---|
| 1 | **Card W — PREPARED 2026-07-22, NEVER LAUNCHED** (§9.2): three arm configs in tree, W1–W7 unmeasured, `docs/slip_qc_warm_report_<date>.md` never created. **The cluster's single largest open item** — ~4.5–7 h wall, and since the 2026-07-21/22 goldens are gone a fresh same-build OFF baseline must be taken first (invariant #6). |
| 2 | **Card H3 — deferred, W6 unmeasured**; undecidable without Card W. |
| 3 | **Card OP — untriggered.** |
| 4 | **Detector-side hole handed over by `docs/SPEC_flicker_gap_integrity.md` §8.3 finding 3, explicitly owed by THIS card**: the MW/LLI drain matches assertions against `StateManager::getAmbiguities()` (`src/core/slip/NetworkSlipQc.cpp:151-153`, verified 2026-08-15), but a **re-entering arc's ambiguity has just been pruned** (`pruneAmbiguitiesExcept`, `MeasurementModel.cpp:748`, unconditional, no grace epoch), so `matched` stays false — **2 of 2 LLI assertions raised on a re-entry epoch reached NO refloat funnel**, while any carried invariant-DD chord for that arc, keyed independently of the StateManager, is NOT re-tapped. The flicker card measured the hole and deliberately did not close it. ⚠ **2026-09-09: this hole then ATE the Card-G RESPONSE.** Its drain was written in the same shape (walk `getAmbiguities()`, match satellite+signal), and on a 12 h / 63-station field arm it reported **396 re-entries judged SLIPPED, 775 signals with "no live ambiguity", 0 freed**, and the whole run came back **byte-identical to its OFF control** — a card whose entire population is re-entries cannot use a live-state match. It is closed **for the re-entry drain only**: the key is synthesised in the inject site's format and the protectability test reads the CARRY (`local_carry` within `phase_leveling_max_gap_s`) instead of the state, with `gap_reentry_state_absent` counting how often the column was in fact missing (expected: nearly always). **The MW/LLI drain itself still has the hole** — same fix shape available, not applied here because that drain's population is dominated by measured arcs and the change would not be byte-identical. Prune site: `MeasurementModel.cpp:784`, `pruneAmbiguitiesExcept` inside `ensureRequiredStates`, which runs in PASS B — i.e. AFTER every PASS-A slip drain.  The re-entry drain's fixed shape and its graded outcome are **§12**; this row stays OPEN for the MW/LLI drain. |

**CLOSED elsewhere — do not relitigate.** **The headline D5 lead** ("~97 % of D5 re-taps have no raw
observable step — over-firing, or an invisible slip class?", §8.2 F3 / §9.0 / §10.5.4) **is ANSWERED
from a different direction by `docs/SPEC_d5_churn_limit_cycle.md` §2b: 95.3 % of D5 flags are on chords
BORN that epoch at √var_v = 1000.000 m — the diffuse fresh-chord prior. A born chord has no carried
prior, so there is no H0 and the ratio was never a hypothesis test.** That card is a genuinely different
mechanism (four MECHANISM knobs on 63-sta FT63 DOY-129, different code, different graders) and its
numbers are independent measurements, not duplicates — its §2b closes this lead and its §7 falsifies the
"remove the churn and integers appear" hypothesis four ways over. The D3 cross-experiment (§10.3) was
never run and is largely superseded; if it is ever run, read it against §2b's born/carried split. Also
closed: **the 352-row LLI figure is bit 2 (antispoofing), not a defect** (§8.6 item 3), and **Card R's
station-local hand-off candidate is structurally unimplementable at this seam** (§8.2 F4).

---

