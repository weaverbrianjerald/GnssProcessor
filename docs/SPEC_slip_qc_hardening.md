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

## 8. Implementation record, 2026-07-21 (absorbed `slip_qc_hardening_report_2026-07-21.md`)

All default-OFF. **Read §8.2 before §8.3** — three premises did not survive measurement and one MUST
could not be met as literally written.

### 8.1 What landed

| Area | Files |
|---|---|
| Shared raw MW/GF/LLI seam (§1) | NEW `include/slip/NetworkRawObservables.hpp`, `src/core/slip/NetworkRawObservables.cpp` |
| Card E + Card L | `include/slip/NetworkSlipQc.hpp`, `src/core/slip/NetworkSlipQc.cpp` |
| Card R | `include/slip/IfPhaseSlipDetector.hpp`, `src/core/slip/IfPhaseSlipDetector.cpp`, `src/core/app/ApplicationNetworkSetup.cpp` |
| Card G + run-end telemetry | `src/core/app/ApplicationNetwork.cpp`, `src/core/app/ApplicationNetworkAssembly.cpp`, `include/app/Application.hpp` |
| Config | `include/config/GnssConfigTypes.hpp` (`SlipQcConfig`, `:370` / `:911`), `src/core/config/GnssConfigLoadNetwork.cpp:3191` |
| Grader (§6.1) | NEW `scripts/grade_slip_agreement.py` |
| Unit tests (rule 7) | NEW `tests/test_if_phase_slip_detector.cpp`, `tests/test_network_slip_qc.cpp` |
| Build / configs | `CMakeLists.txt` (3 explicit source entries); NEW `config/test_suite/_wlver_150sta_10ep_d10_allfreq_sliphard.json` (cards armed) |

**Toggle deviation (stated).** The spec wanted the toggles armed in the audit config with the OFF
baseline from a stripped copy; it landed as the mirror image — the audit config
`_wlver_150sta_10ep_d10_allfreq_slipcmp.json` is **unchanged** (so §0's reproduction stays exact,
§8.4.1) and a new `..._sliphard.json` twin carries the three masters. Same two-arm gate, one less
thing that silently drifts.

### 8.2 Findings F1–F5 that contradict the spec's premises

| finding | content |
|---|---|
| **F1** | **L2's causal story is false: the four "chatty" stations assert LLI bit 2, not bit 0.** Census of the golden run's `network_slip_raw.csv` by LLI byte: `0` → 28 981; `1` (bit 0, **lost lock**) → 188; `4` (bit 2, **antispoofing**) → 396; `5` (bits 0+2) → 11. **All 396 `LLI=4` rows are the SECOND band of a JPN station** (`lli1/lli2 = 0/4`). The drain has always tested `lli & 1`, so they **never drove a single refloat**; bit-0 census is **122 rows, not 352**. Card L is correct and useful but cannot deliver the "2 318 → real-event order" falsifier — the assumed population does not exist. It found the *real* bit-0 chatterers and demoted them. |
| **F2** | **the 2 318 carry refloats are dominated by the D5 innovation gate, not the coarse detectors.** Golden run, same epochs: `slip refloats 2318`, of which D5 contributed **1 560 flags**; the coarse layer contributed **67 MW + 87 LLI** satellite detections and **49 IF-TD** flags. Chord kills are not 1:1 with ambiguity marks (one ambiguity is a graph edge), but the ordering is unambiguous: **D5 dominates.** |
| **F3** | **D5 re-taps are ~97 % NOT coincident with a raw observable step.** Card G(c) made the audit's un-decomposable 1 560 attributable: on the armed run **1 921 D5 re-taps, 54 coincident with a raw MW/GF step** at the same (station, satellite, epoch) — the gate fires on **model tension**, not on anything visible in the receiver's own observables. *(Cited by `include/config/GnssConfigTypes.hpp:2622`, `rep_error_d5_keep`. **ANSWERED from a different direction — §11.**)* |
| **F4** | **Card R's third CANDIDATE (station-local hand-off) is structurally unimplementable at this seam. NOT BUILT.** Two independent reasons, both verified by construction in `tests/test_if_phase_slip_detector.cpp`: (1) a genuine receiver-level common mode **cancels in the between-satellite single difference**, so it can never reach the non-reference double differences this gate adjudicates; (2) the only epoch it could fire on is the forced re-election epoch, where `ref_changed` is true and `processDoubleDifferences` applies the median-offset rebase (`IfPhaseSlipDetector.cpp:546-557`) — removing exactly the common mode the test looks for, before the gate is consulted. Shipping it would have left a permanently-zero counter reading as "no station-local events occurred" — the invariant-#4 failure mode. **The branch AND its counter were REMOVED**, reasoning at `IfPhaseSlipDetector.cpp:485-495`. |
| **F5** | **a coherence-only reference gate destroys the event R4 protects.** The first armed smoke suppressed the genuine, LLI-corroborated OWMG00NZL/G31 station re-lock along with the false JPLM/MKEA family: a multi-satellite re-lock produces an *incoherent* family (each satellite jumps by its own amount), exactly the drift signature. Fixed by the incoherent split (§8.3). **Standing rule: the empirical-Bayes cold prior is `floor_frac·λ_IF / k` and NEVER this epoch's dispersion** — the latter is self-contaminating and is what swallowed OWMG. |


### 8.3 Card-by-card as built

**Card E (no toggle, numerically inert).** `refloat_signal` reports **matched** and **refloated** as
separate facts; emission and the `n_mw`/`n_lli` counters key on the detection, refloat accounting is a
separate warm-only counter. A cold run writes `mw_wl_slip`/`lli_slip` rows with `action=diagnostic`.
Measured on the golden 11-epoch run: **67 wide-lane + 87 LLI detections that previously wrote nothing
at all**; `slip refloats` stayed at **2 318** — E3's no-op verified against the actual carry counter,
not asserted.

**Card R (`network.cycle_slip.if_phase_td_ref_*`).** The legacy gate (`|median(non-ref ΔΔ)| >
0.5·λ_IF`, `n ≥ 2`, fixed) is kept as the **candidate filter** (`IfPhaseSlipDetector.cpp:617`); when
armed, `evaluateRefGate` (`:399-518`) adjudicates in five parts.
| part | rule |
|---|---|
| 1 | **Scale** — `\|median\| > max(floor_frac·λ_IF, k·s_eff)`, `s_eff` an empirical-Bayes blend of the constellation's EWMA of non-reference DD **dispersion** with a cold prior of `floor_frac·λ_IF / k` (deliberately *not* this epoch's dispersion — F5). Zero empirical information ⇒ the gate degenerates to exactly the legacy physical floor. **INVARIANT: the dispersion EWMA (`updateRefDispersion`, `:351-397`) advances on EVERY epoch with a usable family, not only candidate epochs**, so a persistent drift cannot raise its own acceptance threshold. |
| 2 | **Coherence** — a reference slip is a *common* step: `MAD ≤ coherence·\|median\|`. |
| 3 | **Incoherent split (the F5 fix)** — incoherent families split on whether members are *independently* slip-sized (`\|dd\| > λ_IF`, a full IF cycle, **not** the half-cycle candidate floor). **≥ half the family slip-sized ⇒ STATION EVENT**: reset the reference arc (conservative under a zero-q carry, invariant #1) but **do NOT rebase `prev_grad` by the median** (`:669-671` skipped) — the median is a mixture; rebasing would inject a fictitious step into every other arc and hide the real per-satellite jumps from the pool arm. Otherwise ⇒ **drift: suppress.** *(Violating the invariant in 1 or 3 re-opens a fixed bug; 3 preserves the OWMG multi-sat re-lock.)* |
| 4 | **Thin family** — below `min_n` non-reference DDs **the median is untrustworthy**, so the physical floor widens by `thin_widen` and the deferral is counted. |
| 5 | **Persistence cap** — after `max_consecutive` consecutive fires on one reference the gate stops firing and forces a reference **re-selection** (`ref_force_reselect`, `:306`, excluding the incumbent for that one election), moving the drifting satellite into the pool arm. The 9-consecutive pattern is structurally impossible. |

| card | as built |
|---|---|
| **Card L** (`network.cycle_slip.mw_lli_corroborate`) | An LLI assertion **acts** when (a) an empirical MW or GF step corroborates it — decisive at *any* station, keeping a real multi-satellite re-lock intact (M3) — or (b) the station's LLI behaviour is still credible. Only an uncorroborated assertion at a demonstrably non-credible station is downgraded, then emitted `action=suppressed` and counted (M4, `NetworkSlipQc.cpp:330-346`); because the whole `refloat_signal` call is skipped, the C3c insert obeys the same qualification as the pending-reseed path (M5). Gates empirical throughout (rule 4): per-station robust EWMA of that station's **own** \|ΔMW\| / \|ΔGF\| steps, floored by a physical bound in raw metres — **0.5 wide-lane cycles / 4 cm**, the audit's own support test — and **CLEAN SURVIVORS ONLY are folded, so the scale never absorbs the event it must catch.** *Scope limitation, counted not hidden:* an LLI asserted only on a band outside the primary dual-frequency pair is not witnessable by MW/GF; those count separately as `lli_offpair` (`:329`), **never absorbed into "uncorroborated"**. |
| **Card G** (`network.cycle_slip.gap_instrument`) | Zero mechanism change. Counts arc resumptions after ≥ 1 missed **processed** epoch; whether the raw MW/GF level shifted across the break beyond the same empirical gate; and — per arc, after the slip drains have run so the C3c set is complete and before `buildCarryDdPriors` re-keys the carry — whether the arc re-entered with **bridged** carry state (a surviving phase-leveling carry value inside `phase_leveling_max_gap_s`, or a Card-4 chord the driver still holds) versus a fresh float (`ApplicationNetwork.cpp:1881`, ledger at `:1970`). Plus G1(c): D5 re-taps tagged by raw-step coincidence. |

### 8.4 Validation (Release, same-build two-arm gate, 2026-07-21)

Config `_wlver_150sta_10ep_d10_allfreq_slipcmp.json` — 150 stations, GPS-only, `frequency_mode=all` +
`channel_mode=all`, 11 epochs emitted / 0 skipped of 12 synced.

#### 8.4.1 Baseline reproduces §0 exactly

The comparison is against a real golden, not a re-derivation: IF-TD samples → flags **8 717 → 32
(0.367 %)**; margin min/med/max **1.19 / 72.6 / 2066** (§0 said med 74); refsat-reset events **17**
(JPLM ×9, MKEA ×5, OWMG ×3); unsupported flags **14**, exactly JPLM/G10 ×9 + MKEA/G10 ×5; reverse
sweep **57 → 39**; carry slip refloats **2 318**.

#### 8.4.2 OFF gate — byte-identity (all three masters off)

53 of 57 CSV/trace artifacts **md5-identical**. `network_d5_flags.csv` + `network_wl_lane_trace.csv`
are append-mode streams whose **per-run content is md5-identical** to the golden's own tail (**1 561 /
12 569 rows**). `network_run_summary.csv` identical except the two **wall-clock** columns (**576311.5 →
505196.6 ms**) — 12 synced / 11 emitted / 0/0/0 skipped / 150 stations / 20 threads unchanged.
`network_events.csv`: pre-existing **57 rows byte-identical**, **+154 rows** every one a `T0_mw_detect`
row with `action=diagnostic`, i.e. exactly and only Card E's mandated emission. Carry census `slip
refloats 2318` **unchanged**. `main_log.log` excluded by construction (wall-clock timings).

#### 8.4.3 Unit tests

`gnss_tests.exe`: **864 / 863 passed / 1 skipped** (pre-existing fixture-gated
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`). Baseline before this work **839** ⇒ **+25 additive
cases, the first dedicated coverage the slip-detector chain has ever had.** *(Dated record — the suite
is 1360 / 196 as of 2026-08-12, CLAUDE.md.)*

#### 8.4.4 ON smoke — falsifiers

| Card | Falsifier | Result |
|---|---|---|
| E | MW/LLI rows appear with `action=diagnostic`; counters count detections | **MET** — 67 MW + 87 LLI detections, previously zero rows |
| E4 | MW ↔ IF agreement readable from `network_events.csv` alone | **MET** — grader reports 57.5 % IF↔MW match on the armed run; on the golden run that section reads "MW/LLI side is EMPTY" |
| R | pool arm unchanged | **MET** — 8 717 → 8 727 samples (sync noise), 32 → 33 flags |
| R | OWMG G31 survives | **MET** — TOW 259320, 259380, both on the new **station-event** path (reset without a mixture rebase). Card-L rows show the same re-lock epoch with all 8 satellites `LLI;mw_step` corroborated and acting |
| R | JPLM/MKEA G10 family gone | **PARTIAL — 14 → 4** (−71 %) at the shipped `max_consecutive=1`. JPLM 9→3, MKEA 5→1; longest consecutive run 1, so the 9-epoch stream is structurally impossible. Not zero: the residual fires are the *physical-floor* arm, which a cold 11-epoch run never gets past (**§8.5** — the source report's row said "see §4.5"; the mechanical map gives §8.4.5, but §8.5 Readiness is the correct target for the physical-floor-arm claim, so this pointer was deliberately corrected, not mapped) |
| L | JPN stations move to suppressed counters | **VOID** — those stations never entered the LLI arm at all (F1); the card demoted the two that genuinely chatter on bit 0 |
| L | OWMG re-locks still act | **MET** — see the R row above |
| L | carry refloats 2 318 → real-event order | **NOT MET, and not achievable by this card** — F1/F2: the assumed population does not exist, and the count is D5-dominated |
| G | counters populate | **MET** — 16 resumptions (5 unmeasurable, 11 shifted); of the shifted, **0 arcs re-entered bridged / 22 as a fresh float**; 0 clean-bridged; D5 1 689 re-taps, **50 raw-step-coincident (3.0 %)** |

Full armed-run counters (shipped defaults):

```
card E   67 wide-lane + 73 LLI detections (0 refloated — carry cold)
card R   10 legacy candidates -> 6 fired (2 station events); 1 suppressed by
         scale/coherence, 0 thin-family, 3 by the consecutive-fire cap;
         3 forced reference re-selections
card L   125 LLI assertions: 34 MW-corroborated, 3 GF, 49 unmeasurable,
         2 off-primary-pair, 22 SUPPRESSED; 1631 credible / 7 demoted
         station-epochs.  Offenders: OWMG00NZL 12/33 (now corroboration-
         required), MAC100AUS 10/24 (demoted then promoted back — the
         hysteresis working, and visible)
card G   16 resumptions / 5 unmeasurable / 11 shifted / 0 shifted-bridged /
         22 shifted-fresh / 0 clean-bridged; D5 1689 re-taps, 50 stepped
carry    slip refloats 2318 (OFF) -> 2399 (ON)
```

`slip refloats` rising (2 318 → 2 399) is expected and is why F2 matters: with the refsat false family
removed, more chords survive to be judged by D5, and D5 — not the coarse layer — sets it.

#### 8.4.4.1 Independent confirmation from the grader

`scripts/grade_slip_agreement.py` adjudicates every IF-tier flag against the raw observables **without
reading any producer counter**:

| | golden (OFF) | armed (ON) |
|---|---|---|
| IF-tier flags adjudicated | 49 | 39 |
| **raw-confirmed rate** | **71 %** | **90 %** |
| unsupported (raw series flat) | **14** — JPLM/G10 ×9, MKEA/G10 ×5 | **4** — JPLM/G10 ×3, MKEA/G10 ×1 |
| `if_slip` (pool arm) unsupported | 0 | 0 |

**The pool arm was never the problem** and is untouched; the entire unsupported population was, and
still is, the refsat side-channel — now four events with no consecutive run.

#### 8.4.5 Defaults set from the measurement, not from the spec text

`if_phase_td_ref_max_consecutive` = **1** (was 2): a genuine one-epoch common step is never consecutive
so the cap costs it nothing, while a persistent drift escalates on its second epoch instead of its
third — measured on this config, the false family goes **14 → 6 at 2, 14 → 4 at 1**, OWMG untouched
either way. `mw_lli_min_epochs` = **1**: verified safe on this data — no station demoted on epoch 1 (no
start-of-file blanket LLI), and a station demoted at first sighting has fresh arcs by construction, so
a suppression there costs the carry nothing.

#### 8.5 Readiness (2026-07-21)

The warm leg was **prepared and not launched**; it became Card W (§9.2) and was **never launched**.
Needed for: **G3's decision rule** (the shifted-AND-bridged count on a *warm* carry is the required
triage artifact — the 10-epoch smoke is cold, so almost nothing bridges by construction: necessary,
not sufficient); **Card L**'s sawtooth / D5 re-tap check at the demoted stations; and **Card R** — the
empirical dispersion EWMA only reaches full weight after ~`shrink_kappa` samples, so a 10-epoch run
exercises mostly the cold prior and the residual JPLM/MKEA events are the physical-floor arm firing,
exactly what the warm empirical arm should take over.

#### 8.6 Follow-ups this work opened (not built here)

| # | follow-up |
|---|---|
| 1 | **Re-examine the D5 innovation gate** with F2 + F3 in hand — it drives the large majority of carry slip refloats and ~97 % of its re-taps have no raw observable step; either the gate is over-firing on model tension, or "model tension" is a real slip class the observables cannot see, and `network_d5_flags.csv` plus the new raw-step tag is the instrument to decide *(became Card D5-A, §9.1/§10; **ANSWERED elsewhere — §11**)*. |
| 2 | **Widen the corroboration primitive** beyond the primary dual-frequency pair if `lli_offpair` is material on a longer/multi-constellation run (2 of 125 here — immaterial) *(became Card OP, §9.3; trigger never fired)*. |
| 3 | **Standing bit-2 (antispoofing) LLI note: nothing in the producer acts on LLI bit 2, and nothing should** — recorded so the 352-row figure is never re-derived as a defect by a future session. |

---

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

## 10. Card D5-A + H implementation record, 2026-07-22 (absorbed `slip_qc_d5a_report_2026-07-22.md`)

Instrument-only: **no gate, no funnel membership or ordering, and no numerical path changed** —
verified by a same-config comparison against the pre-change binary (§10.5.2). The producer-side data
changes are one appended log-only CSV column plus two *counting* corrections the instrument itself
exposed (§10.4 H2, §10.2.3), both stated where they change previously-reported numbers.

#### 10.1 What landed

| item | where (HEAD, re-grepped 2026-08-15) | card |
|---|---|---|
| `raw_stepped` column on the D5 flag dump (`network.mw_wl.d5_dump` writer) | `NetworkInstallCarry.cpp:745` | D5-A/D2 |
| Grader section (g), D5 re-tap attribution, elevation carried through `build_raw_steps` | `scripts/grade_slip_agreement.py` | D5-A/D1 |
| Cross-experiment protocol + decision rule | §10.3 | D5-A/D3 |
| `RefGateParams::max_consecutive` default 2 → **1** | `include/slip/IfPhaseSlipDetector.hpp` | H/H1 |
| Card-G counter **units** explicit; arc ledger split three ways (`gap_clean_fresh`, `gap_unmeas_bridged`/`_fresh`); `arc_resume` event tag + ΔGF made to match | `include/slip/NetworkRawObservables.hpp`; `gap_clean_fresh` at `ApplicationNetwork.cpp:1970` | H/H2 |
| D5 counters gain the **arc** unit (`d5_retap_ambs`, `d5_retap_ambs_stepped`) beside the row unit | declared `include/slip/NetworkRawObservables.hpp:312-313`; ⚠ **incremented at `src/core/filter/NetworkInstallCarry.cpp:723` / `:727`** — they relocated with the `d5_dump` writer, which §9.1's ⚠ note covers only for the writer itself | D5-A/§10.2.3 |
| Defaults-drift guard + cap-generalized persistence case | `tests/test_if_phase_slip_detector.cpp` | H/H1 |
| Warm-leg configs, 3 arms, 1 h | `config/test_suite/_wlver_150sta_1h_d10_allfreq_{sliphard,sliphard_iono2,slipcmp}.json` | W |

**No new config toggle**: D2 rides the existing `d5_dump` opt-in; Card H is hygiene on shipped knobs.

### 10.2 D5-A as built

#### 10.2.1 D2 — the `raw_stepped` column (log-only, TRI-STATE)

| D2 as built |
|---|
| `network_d5_flags.csv` gains a twelfth column, `week,tow,group,chord_u,chord_v,station,amb_key,std_innov,ewl_stasat,hold_chord,injected_prev,`**`raw_stepped`**. **Deviation from the literal MUST ("0/1 from `slip_qc.epoch.stepped`"), stated:** it is **tri-state** — `1` = this (station, satellite) is in `slip_qc.epoch.stepped` (the Card-G empirical per-station MW/GF gate fired at this exact epoch); `0` = a step *could* be formed (adjacent-epoch dual-band predecessor) and was **flat**; ***blank*** = no evidence (raw observables not computed this run, arc start / gap / single-band, or the flag's ambiguity key did not resolve). **Writing `0` in the blank case would assert "no raw step" on exactly the population this card exists to decompose — a false negative on the headline statistic.** The blank follows this writer family's idiom (the lane trace already writes an empty cell for an absent quantity), and the grader treats it as unknown rather than as evidence. |
| The column reads the **same** `slip_qc.epoch` evidence the run-end `d5_retaps_stepped` counter uses, a few lines above in the same epoch block, so whenever both are armed they cannot disagree. **Arming matrix (different conditions, not a contradiction):** the column rides `network.mw_wl.d5_dump`; the counters ride `network.cycle_slip.gap_instrument`; the evidence exists only when some consumer armed the raw observables — `trace_slip_raw`, `mw_lli_corroborate` or `gap_instrument`. With `d5_dump` on and all three off, every cell is blank — the honest answer rather than a silent `0`. Numerically inert: the whole block sits inside `if (mw_d5_dump && carry_innov_gate)` and only *reads* `slip_qc.epoch`. *(D5-A/D4's scope statement — no gate/flag/funnel change — closes §10.2.3.)* |

#### 10.2.2 D1 — grader section (g), the eight-item contract

`python scripts/grade_slip_agreement.py <logdir>` gains **(g) D5 RE-TAP ATTRIBUTION** (the old
run-end-counter echo moved to (h)). No producer change; runs on existing CSVs; all of it also lands in
`--json`.

| # | item |
|---|---|
| 1 | **Dump provenance and RUN SEGMENTATION.** `network_d5_flags.csv` is append-mode, header written once *per file*, so a reused log dir holds several runs under one header — and since D2 under a header that may be one column short of the rows below. The reader segments on (a) a header line, (b) a backwards (week, tow) step, (c) a field-count change, grading the **last** segment by default (`--d5-segment last\|all\|N`). **Without this a cross-check against run-end counters — which describe only the last run — manufactures a disagreement out of good data.** |
| 2 | **Raw-step coincidence, two independent ways**: the producer's exact-epoch `raw_stepped` census, and an offline join against `network_slip_raw.csv` at ±`--epoch-tol` epochs on the grader's own MW/GF/LLI gates. Prints the coincident fraction over all flags and over flags with a series, flagging the <10 % regime explicitly as "firing on model tension". |
| 3 | **Cross-check vs run-end counters** scraped from `main_log.log` (`d5_retaps` / `d5_retaps_stepped`, the gate's `k`, the D5-v2 LS-VCE final σ̂₀ and its k·σ̂₀ threshold), with the two legitimate causes of a row-vs-counter delta spelled out (appended file; reverse-map misses, which the producer skips for the counter but still writes a row for). |
| 4 | **`std_innov` distribution** — min/p25/med/p75/p90/max, median margin against the k·σ̂₀ gate, grazing fraction, and the median of step-coincident vs flat rows (a genuine invisible slip class sits *higher*; a wash is evidence for tension). |
| 5 | **Top (station, sat) families** with distinct-epoch counts and **longest consecutive-run length** on the *processed*-epoch grid from `network_slip_raw.csv` (the D5 file alone holds only flagged epochs, so runs computed on it are upper bounds — stated in the output when it happens), plus the share of families with a run ≥ 3: impulsive vs sustained is the D5-v3 "innovation-persistence" decision input. |
| 6 | **Elevation profile in 10° bins** (count, stepped count, median `std_innov`) — decision input for the per-elevation-bin σ̂ candidate and the direct test of "is the tension geometry-structured?" (an iono-representation signature). |
| 7 | **Warm/cold split** at the first epoch whose `carry_warm` is 1 in `network_if_slip.csv` — **the producer's own flag, so no re-derivation of `pl_warmup` can drift from it** — falling back to `--pl-warmup N` and reporting **"not determined" rather than guessing**. |
| 8 | **Adjacency-tag census** (`hold_chord`, `injected_prev`, `ewl_stasat`) — free from existing columns, the cheapest test of whether the D5 population concentrates on lane-injected chords. |

#### 10.2.3 THE COUNTING CORRECTION — `d5_retaps` counts ROWS, not re-tapped chords

| the correction |
|---|
| Found during this card. Under the WL-3 joint pair block (`network.lane_carry.enable`, **armed in every Card-W arm**) the gate runs per band **ROW** — rows `i` and `K+i` are the two bands of chord `i` — and each flagged row appends the same `(group, chord)` to `lastInnovFlags()`. A chord flagged on both bands is counted twice, while the C3c funnel (a `std::set`) re-taps it **once**. Measured on the gate run: **1 689 flag rows = 1 144 distinct (epoch, group, chord) re-taps + 545 duplicates (32 %)**. So the 2026-07-21 headline "1 689 re-taps" — and the 3.0 % coincidence ratio's denominator — are in band-rows. **The RATIO is unaffected** (both rows carry the same verdict); **the MAGNITUDE of the carry churn was overstated by a third.** |
| Handled additively, not by rewriting history: `d5_retaps` / `d5_retaps_stepped` **keep their ROW semantics for continuity with the 2026-07-21 headline** (Doxygen says so, with the measured duplication rate); new `d5_retap_ambs` / `d5_retap_ambs_stepped` count distinct `(station, ambiguity)` inserts read straight off the funnel set's own `insert().second`, **so the "true re-tap" number cannot drift from what the funnel does**; the run-end line (`NetworkTeardown.cpp:1789-1794`) and grader §(g) print **both** units; and no gate, flag, funnel membership or ordering changed — the insert's return value was previously discarded. **D4 scope**: counter changes are additive only (`d5_retap_ambs*`; Card G's `gap_clean_fresh` / `gap_unmeas_*` under H2) — no existing counter's value or meaning moved. |

### 10.3 D3 — the cross-experiment protocol (binding, written BEFORE Card W runs)

Arms are §9.2's three. **Metric**: `d5_retaps` (run-end counter, cross-checked by grader §(g) row
count), plus the coincident fraction, the `std_innov` distribution, and the family/run decomposition —
per arm, over the *same* epoch span.

**Ruling on what "Iono-2 armed" means here (stated deviation).** Iono-2 carries two independently armable
mechanisms: **A** the dual-layer SH backbone (`layer2_max_degree`) and **B** the exact per-LOS deviation
carry (`rep_error_tau_s`). **Arm (ii) arms B ONLY** (`estimation.global_vtec_sh.rep_error_tau_s = 900`),
which is what the spec names — D3 asks whether *carrying the per-LOS representation error* absorbs the
tension, and **arming A at the same time would confound that with a change in the backbone the deviation
is measured against.** `layer2_max_degree = 4 / layer2_height_km = 1300` in the same file runs the full
card if the combined leg is wanted — noted in the config's `_comment_card_w`.

**Decision rule, recorded with the numbers.** **re-taps(ii) ≪ re-taps(i)** (order-of-magnitude class) ⇒
the tension **is** iono representation error: no D5 gate change, the D5 re-tap count becomes a standing
grading metric for Iono-2, residual still-firing population re-examined on its own. **re-taps(ii) ≈
re-taps(i)** ⇒ both the global-σ̂₀ hypothesis **and** the iono hypothesis are dead for this population ⇒
spec a **D5-v3** card from the §(g) decomposition — **do NOT build it inside this card** — candidates
being per-station or per-elevation-bin empirical σ̂ (decided by the elevation profile + top-family
concentration), innovation-persistence semantics (decided by the consecutive-run distribution), and, if a
genuinely invisible slip class emerges (raw-flat, iono-clean, sustained, `std_innov` well above the gate),
**a FINDING, not a gate defect** — reset-over-repair stands, the re-taps are justified, the card closes.
**Partial movement (a factor of ~2–3)** ⇒ report it as such and **SPLIT the population by whether it
moved; do not average the two conclusions.**

### 10.4 Card H as built

| item | as built |
|---|---|
| **H1 (N2)** | `RefGateParams::max_consecutive` default **2 → 1**, matching `SlipQcConfig::ref_max_consecutive` — the only armed path. Production behaviour is unchanged either way (`ApplicationNetworkSetup` assigns every field explicitly from config), so this is purely about what the *unit tests* and a header reader see. Doxygen carries the measured rationale (14 → 6 at 2, 14 → 4 at 1 on the audit config, OWMG untouched). Two test consequences, both landed: `PersistentCoherentStepEscalatesToReferenceReselection` runs at **BOTH caps (1 and 2)** — fire on each of the first `cap` epochs, escalate on the (cap+1)-th, re-elect on the (cap+2)-th, new reference fires on the (cap+3)-th, covering the run-length arithmetic rather than one operating point; and new **`HeaderDefaultsMatchTheShippedConfigDefaults`** pins every `RefGateParams` default against its `SlipQcConfig` twin, so this class of drift fails a test instead of waiting for a review. |
| **H2 (N3)** | The Card-G run-end line labels **every** number with its unit — RESUMPTIONS `(station, satellite)` vs ARCS `(station, satellite, signal)` — units documented on `SlipQcCounters` itself and the 2026-07-21 "11 shifted … 22 fresh" reading explained as the factor of two it is. **A SECOND DEFECT surfaced while reading the live log line and was fixed:** the arc-level "clean" cell was absorbing arcs of **UNMEASURABLE** resumptions (no pre-gap dual-band level), so a bridge across a break nobody could check was reported as clean — the `arc_resume` event even printed a fabricated `dGF=0.0000`. True split on the live run: **16 resumptions = 5 unmeasurable + 11 shifted + ZERO measured-and-unshifted** (all 7 of the "unshifted" arcs were untestable). The arc ledger now mirrors the resumption three-way split — SHIFTED / measured-and-unshifted / UNMEASURABLE, each closed as bridged + fresh (`gap_clean_fresh`, `gap_unmeas_bridged`, `gap_unmeas_fresh` added) — and the line says **in words that an unmeasurable-and-bridged arc is UNKNOWN, NOT BENIGN**. ⚠ **This bounds W1**: G3's "shifted-and-bridged ≈ 0 ⇒ defer" is only as strong as the unmeasurable-and-bridged count is small, and that number was previously invisible. Counters had no consumer beyond this log line, so the extension was free; nothing existing was renamed. |
| **H3 (N1)** | **DEFERRED, W6-gated — not built.** The refsat dispersion EWMA still folds an incoherent station event's dispersion (`updateRefDispersion` runs before the verdict is known), which can inflate the refsat threshold for ~1/α ≈ 7 epochs. Build only if W6 shows a genuine candidate suppressed inside that window; otherwise record "deferred, W6 = 0". Pool arm and D5 back it up. |


### 10.5 Validation (2026-07-22)

#### 10.5.1 Unit suite

`gnss_tests.exe` (Debug): **865 / 864 passed / 1 skipped** — the pre-existing fixture-gated
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`. Baseline before this card 864/863/1 ⇒ **+1 additive
case** (`HeaderDefaultsMatchTheShippedConfigDefaults`); the cap-generalized persistence case runs twice
inside one test. *(Dated record; the suite is 1360 / 196 as of 2026-08-12.)*

#### 10.5.2 Byte-identity — pre-change binary vs D5-A binary, same config, same build

| byte-identity record |
|---|
| An actual two-arm comparison. The `build/x64-Release/logs` tree left by the **pre-change** binary (built 2026-07-22 00:11) running `_wlver_150sta_10ep_d10_allfreq_sliphard.json` was snapshotted, then the same config re-run with the D5-A binary (Release, renamed exe `gnss_processor_d5a_run.exe`, 9.5 min, 12 synced / 11 emitted / 0 skipped / 150 stations — identical to the golden). **53 of the 54** comparable artifacts **md5-identical, 0 different**. `network_d5_flags.csv` — same **1 689** rows, **first 11 columns match ROW FOR ROW** (0 mismatches), the only delta the appended `raw_stepped` column. `network_events.csv` — **239 rows in both; exactly 7 lines differ, all `T0_gap arc_resume`**: `clean_fresh` → `unmeas_fresh` with `dGF` blanked instead of a fabricated `0.0000` (the H2 honesty fix); the other 232 rows, including every detector-tier row, byte-identical. `network_run_summary.csv` — identical except the two wall-clock columns (**500 434 → 517 720 ms**). `main_log.log` excluded by construction, plus the intended Card-G line relabel. So the D2 column and the counter work are **provably inert on the ON path**, and the *only* artifact byte that moved is the deliberate, enumerated 7-row re-tag; with `gap_instrument` off there are no `T0_gap` rows at all, so the OFF arm is untouched. ⚠ **A SEPARATE gate from §8.4.2** — different binaries; do not collapse them. |

#### 10.5.3 Card D5-A falsifier (10-ep smoke, sliphard config, `d5_dump` armed)

| falsifier record |
|---|
| *Grader `--d5` totals match the run-end counters* — **MET, exactly, on both units**: rows `d5_retaps = 1689` = CSV rows 1689 (delta 0), `d5_retaps_stepped = 50` = CSV `raw_stepped=1` count 50; arcs `d5_retap_ambs = 1144` = the grader's independently-computed distinct `(epoch, group, chord)` count 1144 (delta 0), `d5_retap_ambs_stepped = 36`. *`raw_stepped` populated* — **MET**: **1 631 × `0`, 50 × `1`, 8 × blank**; *review erratum 2026-07-22*: the blanks are flags with **no measurable exact-epoch verdict** (no adjacent-epoch dual-band predecessor at the flag epoch), **NOT reverse-map misses** — the rows-vs-counter delta of **+0** proves every flag resolved, since the counter skips unresolved flags while the dump still writes their row; correctly not claimed as "flat" either way. *OFF-toggle runs unchanged* — **MET**, §10.5.2. The 50 reproduces the hardening report's "1 689 re-taps, 50 raw-step-coincident (3.0 %)" from the *dump* rather than the counter, which is the point of D2: `network_d5_flags.csv` is now self-contained. The arc basis puts the same statistic at **36 / 1 144 = 3.1 %** — **the ratio survives de-duplication, the magnitude does not.** |

#### 10.5.4 The full COLD decomposition (arm (i) of D3)

Not a verdict — the cold arm alone cannot be one — but the instrument works and every number is
load-bearing for the standing "D5 fires on model tension" reading.

| # | finding |
|---|---|
| 1 | **Population**: 1 689 flag rows = **1 144 distinct arc re-taps** over 10 epochs and **542 distinct (station, satellite) pairs**. |
| 2 | **Exact-epoch coincidence 3.0 %** (50/1 689 rows; 36/1 144 arcs = 3.1 %) at the producer's empirical per-station gate. The **±1-epoch join at the audit's FIXED gates says 33 %** (558/1 689), witnessed by **MW = 281, GF = 161, GF+MW = 102, and only 5 by a bare LLI**. Both are printed with a note that they answer different questions — **strict floor vs permissive ceiling**. The spread is itself a measurement: a 3× epoch window plus a *fixed* 0.5-cycle MW gate sweeps in low-elevation noise excursions the per-station empirical gate rejects. |
| 3 | **ELEVATION is where the join's "steps" live**: **213 of 297** re-taps in the 10–20° bin are step-coincident, falling monotonically to **4 of 19** at 80–90°, while median `std_innov` is **FLAT at ~8 across every bin**. Low-elevation arcs are where both raw noise and iono representation error are largest — consistent with the iono-representation hypothesis and **not** with a slip class (a real slip population would show elevated `std_innov`, not the same 8). |
| 4 | **`std_innov`**: med **8.07**, p90 **18.5**, max **355.9** against a gate of k·σ̂₀ = 4 × 1.52 = **6.10**; **median margin 1.32×, 61 % within 1.5× of the gate.** Mostly *grazing* (a global σ̂₀ re-scale would move it wholesale) with a **long structured tail no scalar could absorb**. |
| 5 | **Step-coincident vs flat rows have the SAME `std_innov`** (med 8.05 vs 8.08) — evidence against "the 3 % are the real slips and the rest are noise". |
| 6 | **IMPULSIVE, not sustained**: 542 distinct families over 10 epochs, **median longest-run 1, only 4 families (1 %) with a run ≥ 3.** On the *cold* arm the D5 population is scattered rather than a persistent per-arc misfit — **the single most important number to re-measure warm**, because the Iono-2 hypothesis predicts *sustained* per-LOS families once the carry is warm. |
| 7 | **Adjacency**: only **67 of 1 689** flags sit on an EWL-fixed (station, sat) and **2** on a hold-cycle chord — the D5 population is essentially **DISJOINT from the lane machinery**, so the WL/UWL cards are not what drives it. |
| 8 | Every re-tap in this run is **COLD** (`carry_warm` never 1 across all **8 727** IF-TD samples), which the grader states explicitly — exactly why D3's decision rule is reserved for the warm leg. |

#### 10.5.5 Adversarial review of this change set

A four-lens review (spec conformance / C++ inertness / grader correctness / run mechanics) with **two
independent refuters per finding** produced **19 raw findings, of which 5 survived** and were fixed:

| # | Finding | Disposition |
|---|---|---|
| 1 | Card-G arc ledger filed **unmeasurable** resumptions under `clean_*` and called it "the intended behaviour" | fixed — three-way ledger (§10.4 H2); the live run shows all 7 such arcs were untestable, not clean |
| 2 | the `arc_resume` **event** kept the two-way tag and printed `dGF=0.0000` for unmeasurable arcs | fixed — event mirrors the three-way verdict, ΔGF blank (the 7-row delta in §10.5.2) |
| 3 | `d5_retaps` counts flag **ROWS**, double-counting a pair chord flagged on both bands | fixed additively — `d5_retap_ambs*` (§10.2.3); 1 689 rows = 1 144 arcs measured |
| 4 | grader folded `no-series` rows into the **flat** `std_innov` population, corrupting the card's decision statistic | fixed — bucketed by verdict, no-series reported separately |
| 5 | grader exited **1** ("nothing graded") after a complete section (g) when only the D5 dump was present | fixed — verified `EXIT=0` on a d5-only log dir |

**Findings 1 and 3 are the substantive ones**: both were *silently wrong numbers in the run-end
telemetry*, invisible until this card printed the ledger and the grader recomputed the population
independently.

#### 10.7 Not built, and why

**Card OP** — untriggered; its trigger is `lli_offpair` material on a warm or multi-constellation run,
or W4 > 0, and the cold GPS-only measurement was **2 of 125** assertions; re-evaluate from the Card W
numbers. **H3** — W6-gated (§10.4). **A D5-v3 mechanism** — **FORBIDDEN by D3** until the
cross-experiment rules.

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

## 12. Card G RESPONSE — `network.cycle_slip.gap_reentry_relevel` (2026-09-09)

**One-line status: the DEFECT is closed and proven live; the KNOB is KEEP_OFF as a promotion on
field lineages.** The response removes the whole identified poison population (88 → 0 bridged arcs)
and every level/DD-side instrument improves, but the float consumer pays **+56 % settled rms**,
because a bare refloat re-levels each freed arc from ONE epoch of code — standing invariant #1's
RE-COUPLING edge, charged at 129 arcs / 12 h to kill 88 bridged ones.

### 12.1 The defect (Card G's own G3 rule, come back nonzero)

`runs/fldf3_pl2` (12 h, DOY-129, 63 field stations, the converging leveling posture of
`docs/SPEC_amb_iono_split_overcount.md` §6.15.4 probe 7), run-end Card-G line
(`runs/fldf3_pl2_ctl_new/console.txt`, verbatim):

> 453 RESUMPTION(s) … = 364 SHIFTED (raw MW/GF level moved across the break) + 31
> measured-and-unshifted + 58 UNMEASURABLE … SHIFTED → **88 arc(s) re-entered with BRIDGED carry
> state (THE POISON CASE)** + 640 fresh = 728; unshifted → 23 bridged … UNMEASURABLE → **24 bridged**
> … + 49 fresh = 73.

**Why no detector fires.** An MW/GF verdict needs `epoch_idx == prev + 1`
(`src/core/slip/NetworkRawObservables.cpp:221`) and the IF-phase time-difference tier likewise needs consecutive epochs, so a
RETURN epoch is structurally outside every time-difference detector's domain. Card G measured the
consequence and, by design, only counted it (`slip_marked` reads `carry_slipped_ambs`,
`src/core/app/ApplicationNetwork.cpp` Card-G block ~:1900-2000).

**The worked case.** BSHM00ISR / G22 at TOW 271200 (`runs/fldf3_pl2/logs/network_events.csv`): gap
7 epochs, ΔMW **−1250.98 m** (≈ 1451 WL cycles), ΔGF **−276.67 m**, LLI asserted on both bands of
the return epoch, classified `shifted_bridged … pl=1;dd=0;slip=0`, no `T0_mw_detect` / `T1_if_slip`
that epoch. The pre-gap zero-q level was re-injected at fold_var ~1e-5 m² and the solve absorbed it
along the wet-mapping null (ΔCLK/ΔZWD −1.35, R² 0.9996): that station's ZWD went
**166.495 → −2445.967 m** in one epoch (`runs/fldf3_pl2/logs/network_zwd.csv`, TOW 271170 →
271200; the control arm reproduces it exactly). Invariant #1 (stiff zero-q carry crossing a jump with no wired response) and
invariant #7 (a re-coupling edge that re-validates nothing).

### 12.2 The build

| piece | where |
|---|---|
| knob `network.cycle_slip.gap_reentry_relevel` (+ `_lli` default **true**, `_unmeasurable` default **false**) | loader `src/core/config/GnssConfigLoadNetwork.cpp:4087-4095`; struct `include/config/GnssConfigTypes.hpp:467/474/480`; per-epoch snapshot `include/slip/NetworkRawObservables.hpp:301/307/313`, wired `src/core/app/ApplicationNetwork.cpp:301-303` |
| loader warns (never disarms) | `GnssConfigLoadNetwork.cpp:224-262` — no `relevel_free_epoch` under a warm leveling carry ⇒ counted `gap_reentry_no_relevel`; neither vehicle armed ⇒ `gap_reentry_cold_skipped`, "armed-and-inert (standing invariant #4)" |
| the drain | `src/core/slip/NetworkSlipQc.cpp:1395` `networkSlipDrainGapReentry` (contract banner `include/slip/NetworkSlipQc.hpp:300-424`) |
| drain siting | `src/core/app/ApplicationNetworkAssembly.cpp:1587` — PASS-A slip band, after the wind-up restart drain, **before** the lane observatory's PASS-A half and before PASS B's inject, so the freed set is complete in the SAME epoch (a response one epoch late lets the stale level fold once, which is the whole defect) |
| counters (closure documented at the declaration) | `include/slip/NetworkRawObservables.hpp:424-496` — `gap_reentry_verdicts`, `_pivot_skipped`, `_arcs`, **`_no_carry`**, **`_state_absent`**, `_freed_{shifted,lli,unmeas}`, `_cold_skipped`, `_no_relevel`, `_dd_only`, `_dd_marked` |
| run-end RESPONSE clause (appended to the Card-G line, with the in-band ARMED-AND-INERT warning) | `src/core/filter/NetworkTeardown.cpp:2511-2560` |
| Card-G classification sees the response | `src/core/app/ApplicationNetwork.cpp:1913-1917` — the arc ledger is armed by the instrument **or** by the response, so a response-only arm cannot report its own effect as a structural zero |
| per-arc event identity | tier `T0_gap`, event `reentry_relevel`, detail `sig;gap_ep;dGF;lli1;lli2;cause;carry;state` (statistic = raw ΔMW in metres, **blank** when the break was unmeasurable — a fabricated 0.0 would read as "the level did not move") |
| run-manifest keys, emitted only when armed | `src/core/logging/NetworkRunTraces.cpp:1514+` — `gap_reentry_relevel`, `_lli`, `_unmeasurable`; verified present in `runs/fldf3_pl2_gapfree2/logs/network_run_manifest.json` |

Response = the DETECTOR response verbatim (C3c funnel insert under `carry_invariant`; pending-reseed
+ `free_ambs` latch with cause `kCauseGap` under a warm leveling carry). Nothing is repaired and no
integer is applied: across a loss of lock the slip magnitude is unbounded, so re-entry is treated as
code-anchored-fresh.

### 12.3 ROUND 1 was INERT — the live-state walk (the lesson)

`runs/fldf3_pl2_gapfree` (12 h, same posture) came back **byte-identical to its control**, and the
run-end clause said exactly why (`runs/fldf3_pl2_gapfree/console.txt`, verbatim):

> RESPONSE: gap_reentry_relevel ON — **396 re-entry(ies) judged SLIPPED** …; **0 arc(s) reached with
> a live ambiguity (775 had none)** = 0 shifted + 0 lli + 0 unmeasurable arc(s) FREED … ⚠ **ARMED AND
> INERT**: resumptions exist and ZERO arcs were reached — this is dead wiring, not a quiet run.

The first cut mirrored the MW/LLI drain's `refloat_signal`: walk `StateManager::getAmbiguities()`,
match (satellite, signal). That is correct for a detected slip on a CONTINUOUS arc and structurally
wrong for a RE-ENTRY — the column was pruned with the gap (`pruneAmbiguitiesExcept` inside
`ensureRequiredStates`, `src/core/measurement/MeasurementModel.cpp:784`, PASS B) and is re-created
only AFTER every PASS-A drain. **This is item 4 of §11's "Still OWED", reproduced inside the card
written to answer it.**

Fix: synthesise the key with `StateKeys::makeAmbiguityKey` (the inject site's own format, and the
one the Card-G classification synthesises, so the two instruments cannot disagree), and ask "is there
anything to protect?" of the **CARRY** — a `LocalCarry` entry for (station, satellite, signal) still
inside `phase_leveling_max_gap_s`, or the `carry_invariant` vehicle. `gap_reentry_no_arc` →
`gap_reentry_no_carry`; new orthogonal counter `gap_reentry_state_absent`; `markAmbiguityPendingReseed`
only when the state exists (a mark on an absent key would be consumed by a later epoch's pre-pass —
a response acting on history).

Process note: **the pre-registered P1 caught this, not the unit tests** — every fixture created the
ambiguity state first. A response card's tests must include the state-ABSENT / carry-PRESENT case,
which is the only case a re-entry ever presents.

### 12.4 ROUND 2 — arms and results (`runs/fldf3_pl2_gapfree2/VERDICT.md`)

Single knob, everything else byte-identical. CONTROL `runs/fldf3_pl2_ctl_new`
(`config/test_suite/_fldf3_pl2_ctl_new.json`; `products.txt` sha256 `be523008…` **identical** to
`runs/fldf3_pl2`), ON `runs/fldf3_pl2_gapfree2`
(`_fldf3_pl2_gapfree2.json` = control + the master knob), exe
`gnss_processor_gapfree2.exe` sha256 `2CAF5A24…`; both exit 0, 1440/1440 epochs.

Run-end clause, ON arm: **396 re-entries judged SLIPPED, 0 pivot-skipped, 129 arcs reached with a
PROTECTABLE carry, 646 with no carry to protect, 129 of 129 state-absent, = 109 shifted + 20 lli
+ 0 unmeasurable FREED**. Two closure identities pin the round-1 defect to the unit: `129 + 646 = 775`
(the pre-fix arm's own "775 had none") and `state_absent = 129 of 129` — a live-state match could not
have freed a single arc; the null was structural, not statistical.

| # | prediction | control | ON | verdict |
|---|---|---|---|---|
| P1 | SHIFTED→bridged 88 → ≤ 5 **and** ≥ 80 arcs freed | 88 bridged; RESPONSE OFF | **0 bridged; 129 freed**; 646 no_carry; 129/129 state_absent | **PASS** (both clauses) |
| P2 | BSHM ZWD @271200 within ±5 m of 271170; settled span 369 → < 20 m | 166.495 → **−2445.967** (Δ −2612 m); span **368.873 m** | 1.3148 → 1.2866 (**Δ −0.028 m**); span **33.373 m** | **SPLIT** — poison event PASS, span FAIL |
| P3 | fleet settled ZWD span p50 ≤ 0.80 m; > 10 m count 3 → ≤ 1 | p50 **0.797**, p90 6.829, > 2 m 14, > 10 m 3 (BSHM, COCO, CUSV) | p50 **0.660**, p90 **4.910**, > 2 m **12**, > 10 m **3** (BSHM, CUSV, **WROC**) | **SPLIT** — centre PASS, tail FAIL |
| P4 | consumer settled ≥ h6 ≥ 10 % better | rms **3.178**, p50 **2.416**, p90 5.090 | rms **4.946 (+55.6 %)**, p50 **2.879**, p90 8.520 | **FAIL — materially worse** |
| P5 | DD arc-wander tail (raw e1 drift > 1 cyc) 47 % → lower | 47.0 %, MAD p50 0.117; wl 28.5 % | **38.2 %**, MAD p50 **0.057**; wl **20.4 %** | **PASS** (all six statistics) |
| P6 | T4a attribution refloats 2032 → lower | **2032** / 665 332 opp | **1092** / 665 310 (**−46.3 %**) | **PASS** |
| P7 | withholds 0 → 0, solved 1440 | 1440 / 0 | 1440 / 0 | **PASS** |
| P8 | reported — prires worst \|t\| EXCLUDING the RNEG class (§ `docs/SPEC_prior_residual_census.md` §17) | **1.2649e5** at `pl/CLEAN COCO00AUS/G06/L1C @276150`; RNEG 32 177 rows, worst \|ê\| 139.44 m | **1.8172e5** at `pl/CLEAN COCO00AUS/G15/L2W @292740`; RNEG 33 644 rows, worst \|ê\| 142.73 m (same INEG row) | reported |

**Evidence split of the 129 freed arcs** (`runs/fldf3_pl2_gapfree2/logs/network_events.csv`, tier
`T0_gap` / event `reentry_relevel`, 129 rows): **95** with \|ΔMW\| > 0.5 WL cycles (p50 \|ΔMW\|
**18.095 m** — genuine breaks, not threshold grazing), **15** GF-only (gap p50 **23 epochs**, p50
\|ΔGF\| **0.447 m**), **19** LLI-only. By recorded cause: 57 `shifted`, 52 `shifted|lli`, 20 `lli`;
`carry=pl` on all 129 and `state=absent` on all 129.

Card-G classification, same epoch as the worked case, ON arm: the two `arc_resume` rows read
`shifted_fresh … slip=1` where the control reads `shifted_bridged … slip=0` — the instrument sees the
response, by construction (both sites synthesise the same key).

### 12.5 Verdict

**`network.cycle_slip.gap_reentry_relevel` = KEEP_OFF as a promotion on field lineages** (ledger
`docs/CONFIG_promotion_ledger.json`, regime unchanged: `on("network.phase_leveling") or
on("network.carry_invariant")`). It trades a **bounded, identified** poison population (88 bridged
arcs / 12 h) for an **unbounded** re-level tax, and the graded consumer says the tax is larger:
settled rms +56 %, concentrated in hours 3–9 (hourly ctl→ON: h3 4.33→5.49, h6 5.09→6.52,
h7 4.28→8.31, h9 0.79→2.42; h0–h2 and h11 flat-to-better), and one station newly damaged —
**WROC00POL 3.56 → 15.64 m** settled ZWD span — while BSHM 368.9 → 33.4, BOR1 9.03 → 0.46,
INEG 7.91 → 3.58, COCO 12.67 → 9.78. The fleet centre improves and the tail reshuffles.

This is `docs/SPEC_slip_relevel_walk.md`'s measured mechanism at a new edge: **a refloat re-levels
the arc from ONE epoch of code**, so a response that fires 129 times per 12 h buys metre-class
code-level re-seeds. The DEFECT is real and now instrumented; the bare refloat is not its answer.

**What is KEPT regardless of the verdict:** the instrument half — `gap_instrument`'s arc ledger now
armed by the response too, the `state_absent` / `no_carry` buckets (an inert wiring reads as inert),
the per-arc `reentry_relevel` event identity, the manifest keys, and the prires RNEG re-partition
those runs produced (`docs/SPEC_prior_residual_census.md` §17).

⚠ BSHM00ISR's residual 33.4 m settled span is NOT this card: that station is independently broken
from epoch 0 (`docs/SPEC_amb_iono_split_overcount.md` §6.16). The card removed its 2.6 km excursion,
not its baseline pathology.

### 12.6 The successor tried once — bridge v3 is FLAT on this posture

Pre-registered next step: a **level-PRESERVING** re-entry response (bridge the level across the gap
from the raw MW/GF step instead of dropping it). The mechanism already exists for DETECTOR slips —
`cycle_slip.relevel_delta_bridge` (bridge v2 pend-and-replace) + `relevel_bridge_gf` (v3's GF row) —
so it was graded on the same field posture first, before being extended to re-entries.

Arm `runs/fldf3_pl2_bridge` (`config/test_suite/_fldf3_pl2_bridge.json` = the same control + those
two knobs and the run paths; nothing else differs), same 12 h bed. Run-end
(`runs/fldf3_pl2_bridge/console.txt`, verbatim):

> Network (slip re-level response): 1486 detector-marked (station, amb) free-epoch latches, 1155
> inject-site skips actually taken … **Bridge v2 (pend-and-replace): 282 pend(s) created, 52 REPLACED
> (of which 2 paired same-epoch; max |ΔN| 52.3 cyc)** — those arcs recovered their pre-slip level at
> phase precision; 43 corroboration-rejected (kept the free re-level — the §8.1 wrong-bridge guard),
> 16 kernel-rejected, 109 expired unpaired, 60 voided by re-slip, 28 no-carry, 13 refsat-frame, 63
> MW-without-flag.
>
> Network (bridge v3): GF rows captured 103 (E1+E4 float-plane applies 49, wrong-WL plane rejects 0);
> EWL integers filled 0 same-epoch + 0 belated, band-3 replaces 0.

| channel (settled ≥ h6) | control `fldf3_pl2_ctl_new` | bridge `fldf3_pl2_bridge` |
|---|---|---|
| consumer 3D rms (m), n = 720 | 3.178 | **3.131** (−1.5 %) |
| consumer 3D p50 (m) | 2.418 | **2.823** (+16.7 %) |
| fleet ZWD span p50 (m) | 0.797 | **0.904** |
| stations > 2 m / > 10 m | 14 / 3 | **18 / 4** |
| worst-2 span stations (m) | BSHM 368.9, CUSV 12.8 | **INEG 327.1, BSHM 203.3** |
| T4a attribution-ARC flags | 2032 | **1801** |
| T3c RAIM consensus flags | 1105 | **896** |

(ZWD spans and consumer recomputed from each arm's own `logs/network_zwd.csv` /
`logs/closed_loop_trace.csv`; tier counts from `logs/network_flag_rates.csv`.)

**Reading: FLAT.** 52 of 282 pends replaced (18 %) and 49 GF float-plane applies over 12 h is a
small population; the consumer's rms moves −1.5 % and its p50 +17 %, and the ZWD sink gets slightly
worse (p50 +13 %, and the > 10 m tail gains a station). So the lane-preserving re-level **as built
for detector slips is not the successor either on this posture** — extending it to re-entries is not
justified by this arm, and any level-preserving re-entry response needs a design that is graded on
what it re-seeds, not only on what it stops erasing.

### 12.7 Open items this card leaves

1. **A level-preserving re-entry response is still unbuilt and now unsourced** — v2/v3 as built are
   flat here (§12.6). Design constraint from P4: whatever it does at the re-coupling edge must cost
   less than 129 code-level re-seeds / 12 h, i.e. it must carry the level across the break at phase
   precision or not act.
2. **The MW/LLI drain has the SAME live-state hole** (§11 "Still OWED" item 4,
   `src/core/slip/NetworkSlipQc.cpp:151-153`): any detector assertion raised on a not-yet-recreated
   state is silently inert there too. The fix shape is now proven; it was not applied there because
   that drain's population is dominated by measured arcs and the change would not be byte-identical.
3. **No sim reproduction — owed.** The simulator injects `cycle_slips.*` only
   (`src/sim/SimConfig.cpp:95-101`, `:526-568`); there is no tracking-gap / dropout / outage
   scenario anywhere in the tree, so this whole class is field-only until a `dropout` injector is
   added. Until then the response cannot be graded against truth, which is precisely why the P4
   consumer regression is the binding evidence.
4. **P2/P3's tails are unexplained by this card**: the response left three stations > 10 m and
   created one (WROC00POL). Whether that is the re-coupling tax alone or a second mechanism was not
   separated.

## 13. Card ORACLE — `network.cycle_slip.oracle_relevel`, the sim-only CEILING of a level-preserving re-level (2026-09-10)

§12.7 item 1 left "a level-preserving re-entry response is still unbuilt and now unsourced", and the
truth-graded T4 census on the slipped 60-station bed (`docs/SPEC_amb_iono_split_overcount.md` §6.17b)
added the reason it matters: **the re-level VALUE is the defect** — an erase injects ≈ 2.5× the
bed's ambient DD error, walks back over ~20 min without reaching the floor, and 48 % of T4 events
fire within 15 min of a previous re-level on the same (station, signal), the first erase having made
it WORSE in 64 % of consecutive pairs.

Building the engineering answer (EWL / WL-MW integer estimation of the jump,
`docs/SPEC_lane_slip_response.md`) is expensive. This card measures its **CEILING** first.

### 13.1 The knobs

| knob | default | meaning |
|---|---|---|
| `network.cycle_slip.oracle_relevel` | `false` | arm the response |
| `network.cycle_slip.oracle_truth_root` | `""` | the fixture root holding `<STATION>/truth_slips.csv`; **missing root or missing station file = hard error** |
| `network.cycle_slip.oracle_noise_cyc` | `0.0` | 1-σ of a deterministic Gaussian added to each applied Σδ (the noise ladder; 0.0 is the exact ceiling) |
| `network.cycle_slip.oracle_false_alarm` | `"keep"` | `keep` = an oracle knows it was a false alarm and leaves the level alone (the ceiling); `free` = fall through to today's response, so the arm measures repair-only and keeps the false-alarm cost |
| `network.cycle_slip.oracle_window_s` | `90.0` | detector-latency bound (three 30 s epochs) |

Parsed in `src/core/config/GnssConfigLoadNetwork.cpp:4151-4190`; the arming banner and both
warn-and-DISARM interlocks at `:270-307`.

**It is FIELD-INERT by construction.** A field lineage has no truth root, so the loader hard-errors
when the knob is armed and the root does not exist, and setup hard-errors when any ROSTER station is
missing its `truth_slips.csv`. It is warn-and-DISARMED under `network.carry_invariant` (it shifts
`LocalCarry::value_abs`, which that vehicle does not own) and without `network.phase_leveling`
(there is no carried level to preserve). **Never promote it; never put it in a golden.**

### 13.2 Where it actuates, and where it deliberately does not

One helper (`networkSlipOracleHandle`, `include/slip/NetworkSlipOracle.hpp`) is called from each of
the four DETECTOR refloat actuation sites in `src/core/slip/NetworkSlipQc.cpp`: `refloat_signal`
(the MW confident-wide-lane drain and the RINEX-LLI drain both funnel through it), `refloat_pair`
(the IF-phase triple-difference driver, both bands), and `networkSlipDrainWindupRestart`
(whole-satellite, every signal). When it returns `Repaired` or `Kept` the caller skips every freeing
action for that arc — no `free_ambs` latch, no `carry_slipped_ambs` funnel insert, no
pending-reseed mark, no bridge-pend void, no erase — but still emits the site's event row with
action `oracle_repair` / `oracle_keep`, so the trace population stays comparable to the control's.

`networkSlipDrainGapReentry` (§12) and the T4 RAIM-attribution erase in
`src/core/filter/NetworkRaim.cpp` are deliberately **left alone**: the census question is what a
perfect DETECTOR response buys with every other mechanism held fixed.

### 13.3 The window — a detector-latency bound floored by the carry's BIRTH

    ( max(last_applied_s, carry.birth_s, now_s − window_s),  now_s ]

lower bound EXCLUSIVE, upper INCLUSIVE. Each term earns its place, and two of them were paid for:

- **`now_s − window_s`** is the load-bearing one. Detectors fire 1–3 epochs after the slip (MW needs
  its window to fill, IF-TD needs the triple difference), which is the 90 s the truth census calls
  DETECTED.
- ⚠ **`LocalCarry::last_seen_s` is NOT usable** even though it looks like the natural "already
  absorbed" mark. It is the PREVIOUS epoch's stash time, and a zero-q carried level is not re-set by
  an intervening solve (the stiff prior barely moves against one epoch of phase), so the jump is
  still un-absorbed when a late detector arrives. Using it made the normal population read as false
  alarms — **701 "false-alarm kept" against 88 repaired on a 60-epoch smoke** — and the ceiling that
  arm would have reported was a badly biased lower bound.
- **`LocalCarry::birth_s`** (new; stamped once at creation, preserved across re-stashes, alongside
  the existing loop-epoch-index `birth_ep`, which is NOT usable here) is the hard floor. A T4 erase
  deletes the entry and the next harvest re-creates it from a fresh solve whose level ALREADY
  contains any earlier jump, so a truth slip stamped before the entry's birth is absorbed by
  construction and re-applying it would inject the very error this card exists to remove. It is
  deliberately **not** snapshotted, so a warm-resume-restored entry reads 0 and the window falls
  back to the latency bound — the conservative direction for a diagnostic.
- **`ArcState::last_applied_s`** guards the same arc being handled twice before a re-harvest (two
  detectors in one epoch, or on consecutive epochs).

A window holding no truth slip is a genuine FALSE ALARM and is answered by the `false_alarm` policy.

### 13.4 Counters (standing invariant #4)

Run-end clause, `src/core/filter/NetworkTeardown.cpp:2532-2560`. Closure is asserted:
`seen == applied + fa_kept + fa_freed + no_carry + no_lambda`. Beyond the outcome counts it reports
**which of the three lower-bound terms was BINDING** (`bound_window` / `bound_birth` /
`bound_applied`) — without that the only readable outcome is "N repaired, M false alarm", which
cannot separate a window that is too narrow from a population that genuinely did not jump, and the
two have opposite fixes.

E0 arm's own clause: `13485 detector actuation(s) on carried arcs, 1161 repaired (sum |delta|
62424.0 cyc, max 125.0, 0 multi-slip), 9175 false-alarm kept, 0 false-alarm freed, 3149 fell through
with no carry, 0 with no wavelength; … lower bound set by the window on 6968, by the carry's birth
on 1041, by an earlier repair on 2327`. On the 60-epoch smoke, widening the window to 1e5 s moved
`applied` 88 → 90, so on this bed **late detection is not what the false-alarm population is made
of**.

### 13.5 The four arms — the mechanism is EXACT and the plateau does not move

`runs/golden_sim_campaign{,_zwdblk}{,_oracle}{,_oracle_e1}` against
`runs/simnet60_campaign_wu93`, 720 epochs / 6 h / 60 stations, graded with
`scripts/simnet_oracle_grade.py`, `simnet_grade.py`, `simnet_amb_integer.py --ref-free`,
`simnet_t4_census.py`, `simnet_slip_census.py`.

**Mechanism** (`scratchpad/oracle/repair_effect_A.txt`): 1 161 repairs; |δ| applied p50 50 cyc;
detection latency 0 for 1 124 of 1 161 (so a pre/post slope test is uninformative — the ratio is the
instrument); **unrepaired fraction |e(t₀) − e(t₀−30)| / |δ| p50 0.005** (0.239 cyc), |e(t₀)|/|δ|
**0.070**, |e(t₀+150 s)|/|δ| **0.078**. The jump is gone.

**Slip-side census** — of the 2 452 truth slips: **47.3 % REPAIRED**, **25.6 % detected but KEPT**
(the window/keying missed them — an open defect that biases the ceiling DOWNWARD), **21.2 % FREED**
with no carry entry (60 % of those within 3 epochs of a T4 erase that had deleted it), **5.9 %
SILENT** (no detector row at all). Of 2 697 `oracle_keep` rows: 603 partner-band of a repair, 430
the same slip already repaired earlier (`last_applied_s`, correct), **0** carry-re-born-by-T4
(`birth_s` floor, by design), 69 standalone with a truth slip within ±600 s, and only **8 genuine
data-domain detector false alarms**.

**Results:**

| settled | E0 (`golden_sim_campaign`) | **E0 + ORACLE** | E0 + ORACLE + E1 | zwdblk | **zwdblk + ORACLE** | zwdblk + ORACLE + E1 |
|---|---|---|---|---|---|---|
| ZWD truth rms (m) — gate ≤ 0.5 | 2.098 | **1.779** | 1.819 | 1.188 | 1.104 | 1.239 |
| consumer 3D p50 / rms (m) | 1.751 / 1.799 | 1.730 / 1.896 | **3.781 / 4.909** | 1.084 / 2.389 | 2.234 / 3.493 | 3.334 / 4.294 |
| ambient per-band DD \|e\| (cyc) | 1.65 | **2.43** | **6.09** | 1.65 | 1.89 | 5.66 |
| T4 `attrib_arc` events | 7 656 | **13 562 (+77 %)** | 0 (E1) | 9 871 | 16 095 (+63 %) | 0 (E1) |
| detector re-levels | 4 545 | **687 (−85 %)** | 275 | 4 545 | 717 | 275 |
| ref-free WL(1,2) at own integer | 81.5 % | 81.5 % | **35.4 %** | 86.2 % | 85.9 % | 39.7 % |
| ref-free EWL(2,5) | 99.0 % | 98.6 % | 75.5 % | 99.2 % | 99.0 % | 74.7 % |
| sat-clock contrast rms (m) | 8.050 | 7.777 | 3.159 | 11.791 | 11.806 | 3.243 |
| sat code-bias residual rms (m) | 0.608 | 0.537 | 0.362 | 0.678 | 0.638 | 0.356 |
| oracle repairs applied | — | 1 161 | **1 549** | — | 1 142 | 1 549 |
| … `no_carry` fall-throughs | — | 3 149 | **1 396** | — | 3 285 | 1 396 |

Slip DETECTION is flat at **94.0 %** on every arm (`simnet_slip_census.py`, 2 486 truth slips) —
nothing in this table is a detection story.  ⚠ **The E1 pair is the sharpest reading.** Starving the
T4 erase recovers exactly what the oracle was losing — repairs 1 161 → 1 549 on an *identical*
13 485-actuation detector population, `no_carry` −3 149 → 1 396, `bound_birth` 1 041 → 273, i.e. the
21 % FREED class of §13.5 WAS T4 deleting the carry out from under the repair — and the result is
WORSE than E1 alone on every channel E1 improved (ZWD 0.933 → 1.819, consumer p50 2.559 → 3.781, WL
65.4 → 35.4 %).  A better ceiling instrument, a worse product: the two mechanisms are
anti-synergistic, and the ambient DD error rises to 6.09 cyc with nothing left to disturb the frozen
split.

**Verdict: FAIL on every pre-registered gate; the card is a measured CEILING, not a fix.**
Detector re-levels fell **85 %** while T4 grew **77 %**: arcs restored to their true integer are
rejected by the corrupted MAJORITY. The per-band DD errors are equal in CYCLES while the lanes are
clean (E0 ref-free L1C 0.896 / L2W 1.086 / L5Q 0.980 / L7Q 0.816 cyc against EWL 0.032, WL 0.141),
which is a (range, iono) split rather than an ambiguity error — solving `δρ + γ_b δI = ε λ_b` on the
L1/L2 pair gives (δρ, δI) = (0.1069, 0.0834)·ε m and predicts L5's own error to 0.6 %. **The station's
zero-q levels are frozen around a self-consistent mis-split of range against ionosphere, and a
single-arc level preservation, however perfect, cannot undo a station-wide one.** Full reading and
the T4 census it rests on: `docs/SPEC_amb_iono_split_overcount.md` §6.17b / §6.17e, §7 items 29 / 33.

Ledger: `network.cycle_slip.oracle_relevel` (+ `_truth_root`, `_noise_cyc`, `_false_alarm`,
`_window_s`) = **KEEP_OFF**, scope "SIM-ONLY DIAGNOSTIC — a truth-fed response; never a posture".
Unit contract `tests/test_slip_oracle.cpp` (19 cases): window arithmetic including all three lower
bounds, the birth floor, multi-slip summation, the unknown-signal fall-through, both false-alarm
policies, the noise ladder's determinism, closure of the counters, and the loader's two
warn-and-DISARM interlocks plus both hard errors.

### 13.6 Addendum, 2026-09-10 afternoon — the two levers this card named, both measured

§13.5's verdict ("a single-arc level preservation cannot undo a station-wide (ρ, I) mis-split")
named two successors. Both ran the same afternoon, config-only, on the same bed; full record
`docs/SPEC_amb_iono_split_overcount.md` §6.18a–§6.18c and §7 items 34 / 35 / 37.

**1. The levels' MEMORY — FALSIFIED ×4.** `network.phase_leveling_q_amb` 1e-7 / 1e-6 m²/s
(`runs/golden_sim_campaign_plq7` / `_plq6`) and `network.phase_leveling_var_floor_m2` 1e-4 / 1e-3 m²
(`_plvf4` / `_plvf3`), each a single knob off E0. The q half HALVES the T4 churn (arc erases
22 968 → 12 348 / 12 477, −46 % — so the erase tier *is* sensitive to level stiffness) and still
leaves ZWD truth rms at 1.93 / 1.96 m against E0's 2.098, with the truth |residual| MEDIAN WORSE
(p50 0.123 → 0.256 / 0.450 m) and the consumer +57 % (settled p50 1.751 → 2.76 / 2.75). The
var-floor half is NEUTRAL on all four channels (ZWD 2.089 / 2.060, consumer 1.85 / 1.89, T4
22 524 / 21 429). Both are non-monotone in their own value across a decade. ⇒ softening the level
lets it MOVE but gives it nothing to move TOWARD while arcs keep restarting; the freeze is not the
levels' stiffness. Both knobs now carry KEEP_OFF ledger rows.

**2. Starving the erases — the other half of §13.5's "T4 was the only thing re-randomising the
split", now graded ON THE WIRE.** The `network.raim_arc_persist_epochs` ladder K = 1 / 10 / 40 / 1e6
on the in-block posture improves every producer channel monotonically (ZWD truth rms 1.188 → 0.818 m,
satellite-clock datum-free contrast 11.79 → 2.66 m, −77 %) while the CONSUMER degrades monotonically
(settled p50 1.085 → 2.296 → 2.727 m). The wire says why: within-tenure WANDER rises with K (Galileo
L1C 0.249 → 0.793 cyc, GPS L1C 0.507 → 0.944) while per-epoch jitter is flat (~0.07 cyc), step size
is flat and **handover counts are identical in every arm** — K changes the CORRELATION of the steps,
not their number or size — and the motion is a 2-parameter (range, iono) walk at R² 0.993–0.996 whose
SHAPE is invariant in K. Arc-level-removed, the clock channel is flat at 5–12 cm across the ladder
(the 4.4× clock gain is ALL arc LEVEL) while the arc-removed PHASE-clock degrades on every Galileo
band. Within each run the consumer's |error|-vs-arc-age correlation FLIPS sign (−0.335 at K = 1 to
+0.20…+0.42 at K ≥ 10). ⇒ **the T4 erases are the crude cure for the anchors' walk** — they chop a
continuous walk into steps a consumer flushes by resetting — so neither a level-preserving response,
nor T4-off, nor a K is the fix. The named build is now the (ρ, I) split's I half carried in-block
(`network.carry_iono_in_block`, branch `worktree-ionoblk`), under the B9/Q1 quiet-bed protocol.

⚠ **Two riders for anyone re-using §13.5's numbers.** (a) `sat-clock contrast` is not a
consumer-relevant metric by itself: it improves 4.4× across the K ladder while the consumer gets 2.5×
worse — grade a consumer-facing slip response on the per-satellite PHASE-clock (C + P). (b)
`scripts/simnet_wire_integer_consistency.py` treated `truth_ambiguities.csv` as a STATIC integer, so
on a slipped bed (2 486 slip rows touching 32.3 % of the keys) its integer-dependent steps were
contaminated; it now accumulates the truth slips through `simnet_amb_integer.load_truth` / `n_at`.
Everything quoted from that grader in §13.5 and here is integer-free.

### 13.7 The FALSE-ALARM-ONLY corner — `oracle_true_slip`, and what it proves (2026-09-10 evening)

§13.5 measured the ceiling of a level-preserving response to a TRUE slip.  It left the other half of
the 2×2 unmeasured: what the detectors' FALSE alarms are costing.  On this bed that population is not
a rounding error.

**The measurement first (`scripts/simnet_slip_census.py`, in-block arm, post-Card-R goldens).**  Of the
bed's 2 486 truth slips, **2 337 (94.0 %) are DETECTED** — MW 2 006, IF-TD 331, T4 59, raim 16 — and
**74 (3.0 %) are SILENT**, mostly ≥ 90-cycle jumps at el ≈ 10°.  But of **4 708 slip-tier events,
2 371 (50 %) have NO truth slip anywhere in the window: MW 1 132, IF-TD 1 239** (ignition candidates
and two refsat resets included).  That is ≈ **6.4 false re-levels per station-hour against 6.8 true
slips per station-hour**.  After Card R fixed the IF-TD reference gate on the QUIET bed (§11 / SPEC_amb_iono
§6.14.6), BOTH detectors still fire as often on nothing as on something on the SLIPPED bed — consistent
with the fixture's phase noise being ~5× the field's while the MW thresholds are field-empirical
(`feedback_mw_thresholds_empirical`; a fixed PHYSICAL threshold does not port between beds).

⚠ **Two counting bases, never to be mixed in a rate.**  The census counts EVENTS per
(station, satellite) — ~2 300 false per 6 h on this bed.  The oracle's counters below count per-signal
ACTUATIONS — 6 804.  The earlier "13 485 actuations" figure was the per-signal OFFER count, a third
basis again.

**The knob (commit `3dd1284`).**  `network.cycle_slip.oracle_true_slip` = `"repair"` (default —
§13.5's ceiling, shift the level by the true Σδ·λ) | `"free"` (decline; the caller runs today's
ordinary free/re-level for that arc).  It is independent of the existing
`network.cycle_slip.oracle_false_alarm` = `"keep"` | `"free"`, so the two span a 2×2 whose fourth
corner (both `"free"`) is a pure no-op — the loader WARNs rather than refuses, because that corner is
the byte-identity control for the hook itself.  The false-alarm keeps are now also split **per tier**
in the run-end counters (invariant #4), which is what makes the arm below attributable.

**The arm — `runs/golden_sim_campaign_zwdblk_orafa`** (`true_slip: free` + `false_alarm: keep` on the
in-block posture).  The ONLY difference from the control is that actuations with no truth slip in the
window keep their level; every real slip pays the shipped response.  **6 804 false alarms kept**
(MW 4 156 / IF-TD 2 131 / LLI 517), 3 409 true slips freed as today.

| | in-block control | false alarms KEPT |
|---|---|---|
| ZWD truth rms | 1.188 m | 1.066 (−10 %) |
| ZWD truth p50 | 0.048 | **0.104** |
| consumer settled p50 | 1.085 m | **2.92** |
| consumer settled rms | 2.389 | **4.46 (2.7×)** |
| T4 arc erases | 29 613 | **48 078 (+62 %)** |

**FAIL — and decisive in the direction opposite to the one it was built to test.**  NOT re-levelling an
arc that had no slip makes the consumer 2.7× worse and forces 62 % more T4 erases.  ⇒ **the
false-alarm re-levels are a RESYNC mechanism**: on this bed their function is not slip repair, it is
dragging a drifting arc back into its station's current (walking) frame, and the detectors' 50 %
false-alarm rate is doing useful work by accident.

**What this closes.**  "Tighten the detector thresholds on this bed" is dead as a card — a better
detector would remove the resyncs and leave the walk.  Taken with §13.6's two ladders and
SPEC_amb_iono §6.17b (T4 mostly re-erases the damage its own previous re-level created), all three
accuracy-of-response levers are now measured and none of them is what the consumer is buying: the
response's ACCURACY is not the lever, the frame's OBSERVABILITY is (SPEC_amb_iono §6.19a / §7 item 40,
and the receiver-clock card of §7 item 42).

`oracle_true_slip` is a **sim-only diagnostic** — it reads `truth_slips.csv` — and carries a KEEP_OFF
ledger row for the same reason `oracle_relevel` does.  ⚠ It is also the reason §13.5's "26 % detected
but KEPT" class must be re-read: some of that class is this one.
