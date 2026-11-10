# SPEC — 300 s dev-tier perf & plumbing cards

*Two cards from the 2026-07-14 Fable session (300 s dev-tier bring-up). §1-§7 = the per-LOS
post-solve sweep parallelization card, was `docs/HANDOFF_perlos_parallel_sweeps.md`; §8 = the
rate-transferable-windows card, was `docs/HANDOFF_rate_transferable_windows.md`. Both absorbed
2026-08-14 with section numbers UNCHANGED. Implementer: Opus.*

> **PIVOTED 2026-07-14 (user decision):** INC-P0's sub-timers attributed ~79% of "other" to the
> Card-4 invariant-carry DD driver (dense K≈7834 DD covariance), NOT the per-LOS sweeps (14.5% =
> cov-consumers, the D1 card). **P1/P2 are PARKED** pending a post-pivot re-measure; the P0
> instrumentation stays live. Successor: `docs/SPEC_carry_factored_pz_C2.md`, which absorbed the
> Card-4 driver perf card (was `HANDOFF_carry_driver_perf.md`, now under `docs/attic/`).

| Increment | State (2026-08-14) | Evidence |
|---|---|---|
| **INC-P0** sub-instrument "other" | **LANDED** | D0b ranked table emitted at `src/core/filter/NetworkTeardown.cpp:314` ("the ranked table the perlos card acts on"); knob documented at `include/config/GnssConfigTypes.hpp:1014` ("measured; see …") |
| **INC-P1** sweep fusion | **DEFERRED** | `docs/SPEC_carry_factored_pz_C2.md` §6, bullet "Next measured card = P1 sweep fusion … Deferred (perf deemed sufficient)" |
| **INC-P2** parallel fan-out | **UNBUILT** | `docs/CONFIG_promotion_ledger.md`, `network.trace_section_timers` → **interlock** (b) calls it "the unbuilt parallel-sweeps card"; no `network_parallel_sweeps_` member exists |
| **§8** rate-transferable `_s` twins | **UNBUILT** | re-grepped 2026-08-15: `_window_s` returns zero hits across `include/` and `src/` |

> ⚠ **Provenance.** Every `runs/<name>` and `build/x64-Release/run_<name>` path below resolves under
> `D:\GnssProcessor_archive\2026-08-12\` — `runs/` and the pre-VS2026 `build/` trees were archived
> off C: on 2026-08-12. They are not in the working tree.

## 1. Problem statement (measured, not conjectured)

D0 section timers (`network.trace_section_timers`) on three 11-epoch smokes, 305 stations, 300 s
decimated tier (`epoch_decimation_n=10`), Release, `parallel_stations=true` + `parallel_seam=true`
(both verified armed in log):

| Run | Config | s/epoch | assembly | reduce | solve | cov-inv | cov-consumers | **other** |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| A | GPS+Gal, SH deg-15, em25 (`_net305_3card_dev300s_smoke.json`) | 51.7 | 21.0% | 8.5% | 0.2% | 0.5% | 15.4% (908 calls/ep) | **54.5% = 28.2 s** |
| B | GPS-only, SH deg-8, em10 (`_net305_3card_dev300s_gps_d8_em10_smoke.json`) | 52.8 | 3.9% | 1.6% | 0.01% | 0.05% | 13.2% (606 calls/ep) | **81.4% = 42.9 s** |
| C | B + `carry_invariant_honesty=false` | 52.9 | — | — | — | — | — | **81.4% (NULL)** |
| D | B + dd-frac OFF + carry_vce OFF + honesty OFF (`..._smoke_lean.json`) | 52.2 | 4.0% | 1.6% | — | — | 6.5% (303 calls/ep) | **87.7%** |

- Run D (lean-diagnostics arm, estimation counters byte-identical to B): stripping every safely removable diagnostic bought **−1.1% wall**. dd-frac's own recon sweep was half of all cov-consumer calls (6676→3338 = 303/ep — the H1 "+303 recon/ep" class, reproduced), its ~39 s of bracketed time vanished, net wall moved 6 s; the rest is run-to-run noise. A concurrent session shares this box: treat **±5% as the single-run noise floor** and re-baseline on a quiet box before publishing speedups.
- **Bound: the config lever on "other" is ≤ ~8% even in the most favorable reading. The residue is structural. This card is the fix, not config.**
- A→B collapsed the shared border 1142→534 cols (deg-15→8 removed 175 SH cols; GPS-only halved keys): assembly+reduce+solve fell 30% → 5.5%. Epoch cost did NOT fall.
- A→B **cut** dd-frac trace pairs 2000→500 and turned `trace_ionosphere` OFF, yet "other" **grew** 28.2→42.9 s/ep. The only cost-increasing delta was the elevation mask 25°→10° (more LOS per station) ⇒ **"other" scales with LOS count.**
- Run C: the Lemma-1 honesty audit is **free** (Δwall 0.3%, PB/D5 counters byte-identical). Eliminated as a suspect.

Conclusion: the bottleneck is the **serial per-LOS / per-station post-solve machinery** — QC scans,
health/monitor sweeps, back-substitution consumers, emit-side prep — none of it timed individually
("other" = loop total minus the five timed sections). **Target:** "other" ≤ 15 s/ep at Run-B config
(≥ 2.8×), i.e. a 300 s-tier 3 h run (36 epochs) ≤ ~15 min wall; stretch ≤ 10 s/ep. Before/after
baselines: `build/x64-Release/run_dev300s_{smoke,gps_smoke,gps_nohon}/` (logs + D0 totals).

> ⚠ **Do NOT resolve this against the 30 s reading.** `docs/CONFIG_promotion_ledger.md`
> §"Cross-cutting notes from the audit" (the "COST EVIDENCE IS REGIME-DEPENDENT" bullet) deliberately
> preserves the pair as an apparent contradiction: at 305 sta / 30 s / all-on, dd-frac + the T1a/H1
> sweep are 25.1% of the epoch and dropping dd-frac is quoted as a −13% bracket; here, at 305 sta /
> 300 s decimated / GPS-only deg-8, stripping dd-frac AND carry_vce AND honesty bought −1.1%, inside
> the ±5% noise floor. Different tier, different border size (1142 vs 534 shared cols). Never quote
> one as the other's number.

## 2. What "other" contains (candidate blocks, with grep anchors)

Guesses, **to be CONFIRMED and ranked by INC-P0 sub-timers before any parallelization** (§3 records that
the ranking DID contradict this list). Line numbers drift — use the anchors. **The anchors are still
valid; the host-file statement is not:** these blocks were written when they all lived in
`src/core/app/ApplicationNetwork.cpp`, and the decomposition series (`docs/network_decomposition_plan.md`)
has since relocated most of them into seam TUs — RAIM → `filter/NetworkRaim`, slip scans →
`slip/NetworkSlipQc`, run-end readout → `filter/NetworkTeardown`, emit/re-base →
`NetworkSolPrep`/`NetworkEmitStore`, iono/dd-frac/SH diags →
`NetworkIonoDiag`/`NetworkDdFrac`/`NetworkShDevVtec`/`NetworkShHealth`.

| # | Block | Anchor (verbatim grep) | Scaling |
|---|---|---|---|
| 1 | PASS-A slip scans (MW refloat collect, IF-TD jumps) | `sta_mw_refloat` ; `getLastIfJumps` | sta × sat |
| 2 | RAIM pass-1/pass-2 + median-polish attribution | `struct AmbStat` ; `struct RaimDetail` ; `medianPolish2Way` | sta × sat (recon per station) |
| 3 | T1a SH health sweep + H1 fused deweight gate | `T1a: SH-field health probe` | sta × LOS (recon per station) |
| 4 | Iono back-substitution supply (+trace when armed) | `reconstructStation` sites in the iono-supply block | sta × LOS |
| 5 | Phase-leveling / carry harvest + D2/C1 stash prep | `Phase-leveling carry: harvest posterior + stash` | sta × amb (leveling OFF in tier configs — may be near-zero) |
| 6 | dd-frac trace (B5, 500 pairs in tier configs) | `B5: cross-station between-satellite DD float ambiguities` | pairs |
| 7 | eq34/re-base/PB-emit-filter + store push + solution CSV | `Card 4/M1 phase D emit re-base` ; `NetworkPbEmitFilter` | keys (~200–420) — expected small |
| 8 | Clock monitor / ClockOsc predict-update, epoch-status QC | `struct EpochStatusRow` ; `ClockOsc` | sat, sta |

~13 `reconstructStation` call sites exist post-solve (grep `est.reconstructStation`). H1 established
the "one sweep, many consumers" doctrine — its deweight gate was fused into the T1a sweep precisely to
kill +303 recon/ep (anchor `at ~zero marginal cov-consumer cost`); several remaining sweeps are
plausibly fusable before any threading. `cov-consumers` (13.2%, timed separately, "D1 per-station
target") is a **separate planned card (D1: selected inverse / per-station batching)** — don't conflate.

## 3. Increment plan

### INC-P0 — Sub-instrument "other" (do this FIRST, no exceptions) — **LANDED**

Extend the D0 machinery (anchor `Tier D0: per-epoch section timers`) with sub-timers around blocks 1–8
under the same `sec_timers_on` gate; accumulate run totals + per-epoch readout in D0 style ("D0b" lines).
Log-only: every CSV byte-identical, only `main_log` grows. **Gate P0:** run
`_net305_3card_dev300s_gps_d8_em10_smoke.json` (§5 recipe), produce the ranked table; if the ranking
contradicts §2's guesses, follow the measurement.

**Gate P0 ANSWER (2026-07-14): the ranking DID contradict §2** — ~79% of "other" is the Card-4
invariant-carry DD driver (dense K≈7834), 14.5% the per-LOS cov-consumers. That is the pivot in the
banner above, and it is the P0-first discipline working as designed. As built, the D0b brackets tile the
loop body across **19** sub-blocks, not §2's 8 (`SB_SLIP, SB_PREASM, SB_GAUGEBUILD, SB_DDBUILD,
SB_SOLVEGLUE, SB_INSTALL, SB_NETAR, SB_SOLPREP, SB_EMITSTORE, SB_IONODIAG, SB_DDFRAC, SB_SHDEV_VTEC,
SB_T1A, SB_LEVELING, SB_RAIM, SB_CLOCKUPD, SB_FRAMEDIAG, SB_STASH_STATUS, SB_FASTCLOCK`), with
`net = wall − Δ(reduce+solve+cov-inverse+cov-consumers)` inside each bracket so recon/solve already
counted in the five D0 sections is not double-counted; the residual logs as "unattributed".

⚠ The full ranked table was never written into a doc — it lives only in `main_log.log` D0b RUN TOTALS of
runs that are now archived. **OWED:** re-emit it on a quiet box if P1/P2 are ever revived.
⚠ Interlock (`CONFIG_promotion_ledger.md`, `network.trace_section_timers` → interlock (a)):
`lane_carry.parallel_components` SILENCES the carry driver's per-op timer for its parallel pass
(`NetworkMwWlGlue.cpp:367`, `carry_driver.setPerfTiming(false)`) ⇒ D0 numbers undercount when both are on.

### INC-P1 — Sweep fusion (serial, byte-identity-checked) — **DEFERRED** (perf deemed sufficient; `SPEC_carry_factored_pz_C2.md` §6)

Merge redundant per-station `reconstructStation` sweeps whose consumers are read-only on `x_r` and
mutually independent within the epoch (verify per pair — consumer B must not read state consumer A
writes this epoch). One recon per station per epoch is the ideal. Pure reordering of read-only
consumers, so byte-identity ON is achievable and REQUIRED (float ops per consumer unchanged; only the
recon call count drops).

**Gate P1:** CSVs + run-end counters + emitted store byte-identical to the pre-P1 build on the Run-B
smoke AND on one 30 s config (`_net305_3card_gpsonly_30m.json`). Report recon-call-count before/after
(the cov-consumers "calls" figure in D0 is the counter).

### INC-P2 — Deterministic parallel fan-out of the fused sweep(s) — **UNBUILT**

New toggle per the CLAUDE.md convention (member + getter + `jval` in `loadNetworkSection_` — since
INC-1 of the decomposition series that lives in `src/core/config/GnssConfigLoadNetwork.cpp` — +
summary log + config-echo JSON block near `"parallel_seam":` + L15 manifest):

```cpp
bool network_parallel_sweeps_{false};  // default OFF = byte-identical
```

Pattern = the proven `parallel_seam` seam (commit bb0a7ab; anchor `Deterministic-parallel post-solve seam`):

1. Partition stations across threads (`#pragma omp parallel for`; the two existing regions use `schedule(dynamic)` legally because every iteration writes ONLY its own indexed slot — keep that).
2. Each thread computes into a **pre-sized per-station slot** (the `StationAssembly` precedent: flags + payload per slot, written by exactly one thread; per-station try/catch → error flag in slot, never throw across the OMP boundary).
3. **ALL shared-state mutation drains SERIALLY afterward, in station-index order:** trace/CSV rows (buffer per-station strings, drain in order — files byte-identical); network-level medians/robust stats (collect in the parallel pass, compute serially — `medianCopy`'s upper-middle convention is load-bearing); monitor updates (per-station EWMA state is slot-local = safe, anything cross-station updates in the drain); counters.
4. **Float-accumulation rule:** any Σ over stations stays serial+fused in the drain (the GMGᵀ lesson from the seam card: parallel accumulation drifted ~1e-4 — never again).

**Thread-safety verification (required, do not assume):**

- `NetworkEstimator` is documented **"Not thread-safe"** (`include/filter/NetworkEstimator.hpp` class banner), but `reconstructStation` is `const` and the kernel is documented pristine after `solve`. Audit the full const path (`NetworkNormalEquations::reconstructStation` and anything it touches) for mutable members / lazy caches; pre-warm any lazy cache serially before the fan-out. Update the class Doxygen with the concurrent-const-use contract you establish.
- **Known race:** the D0 perf-timing counters (`est.setPerfTiming`) — the cov-consumer wall/call accumulation inside the estimator is mutable state hit BY `reconstructStation`. Fix: per-thread accumulation reduced serially, or thread-local timing, or (minimum) document that `parallel_sweeps` + `trace_section_timers` degrades those two numbers and gate the timing calls — but do NOT let timing mutate shared state from OMP threads.
- spdlog inside the parallel region is thread-safe but line-ORDER varies — route per-station log lines through the slot + drain, or demote to the buffered trace. The byte-compare gates intentionally exclude `main_log` line order; do not weaken that by calling log diffs "just ordering".

**Gate P2 (the decisive one), all on the SAME build:** OFF = byte-identical to the pre-card baseline
(structural: toggle never read → paths unreachable). ON vs OFF byte-identical on `network_trace.csv`,
`network_solution.csv`, `network_dd_fractional.csv`, `network_vtec_sh*.csv`, `network_carry_vce.csv`, all
run-end counters (D5 / PB filter / re-base / deweight / RAIM) and the emitted product store — on (a) the
Run-B 300 s smoke and (b) a 30 s 30m lineage config. Thread-count invariance: ON at 2 threads vs ON at 20
threads → identical outputs (match the per-station compute claim `parallel_stations` already makes).
Suite green (~700 cases at the time of writing; 1360 as of 2026-08-12), zero new warnings beyond the
known C4828 set.

**Gate P3 (perf):** D0 + P0-sub-timer before/after table on Run-B config. Accept ≥ 2× on "other"; target
≤ 15 s/ep. Report honestly if a block resists (e.g. inherently serial store push) — that residue defines
the next card.

## 4. Interaction with the decomposition series (now COMPLETE)

Written when the series was pending: INC 9 (RAIM → `NetworkRaim.{hpp,cpp}`) and INC 11 (Emit) would
RELOCATE most of these blocks, so this card was to land **before** those increments — anchors, not
line numbers, so the rebase is mechanical. **As it turned out the series landed first** (INC-0..12
complete), so §2's anchors now resolve inside the seam TUs listed there. The authority for where a
block lives is `docs/network_decomposition_plan.md` §1 (target tree) + Appendix A (freeze-SHA anchor
map). The `network_decomposition_cutmap.md` this card originally cited as AUTHORITATIVE — surveyed at
7,329 lines when `ApplicationNetwork.cpp` had already reached ~11.7k, so stale even then — was deleted
in commit `a8f044f`. If you pick P1/P2 up, land them in whatever seam owns the block and tell the
decomposition doc's owner; do not fork the sweep logic across files.

## 5. Run recipe / conventions (non-negotiable, from CLAUDE.md + memory)

- Build: `scripts/vs2026_build.ps1` (CLAUDE.md §Build). The VS 2022 vcvars pattern this card originally named is RETIRED by standing user directive (2026-08-12). Release ONLY for any timed or data run; `gnss_tests` may run from Debug.
- Run a **renamed exe copy** (`gnss_processor_opus_run.exe`) so concurrent sessions never hit a locked exe; `LNK1104` = transient lock → retry. Pre-kill `gnss_processor*` zombies before timed runs (WER wedge).
- Timed runs: fresh working dir under `build/x64-Release/` (logs land in `<cwd>/logs`); invocation `gnss_processor_opus_run.exe <config.json>`.
- Smoke first (10-epoch class); the 300 s tier smoke IS the perf harness — no long runs needed here.
- Byte-identity comparisons are same-build only (verification integers are build-config-sensitive).
- No `Co-Authored-By` trailer on commits.
- New `.cpp` files (P0/P2 shouldn't need any) must be added to `CMakeLists.txt` explicitly; match the heavy Doxygen style on any new public API.

## 6. Non-goals

- `cov-consumers` reduction (D1 selected-inverse / batching) — separate card.
- Estimator-internal threading (solve is 0.2% — dead end).
- Any change to estimation results, gate behavior or emitted values — wall-clock only; byte-identity is the contract.
- `carry_invariant_honesty` — measured free; leave it alone.
- The emit/re-base/PB-filter chain (block 7) — expected small; touch only if P0 ranks it top-3, and then prefer fusion over threading (it is order-sensitive by design).

## 7. Context pointers

- Memory: `project_dev300s_tier.md` (tier + these measurements), `project_perf_deterministic_parallel_seam.md` (seam precedent + GMGᵀ drift lesson), `project_network_decomposition_plan.md`.
- The three baseline run dirs in §1 (D0 RUN TOTALS at the end of each `logs/main_log.log`).
- Tier configs: `config/test_suite/_net305_3card_dev300s_gps_d8_em10_{smoke,3h}.json` (300 s), `_net305_3card_gpsonly_30m.json` (30 s gate config).

---

## 8. Companion card — rate-transferable adaptive windows (seconds-denominated time constants)

*Was `docs/HANDOFF_rate_transferable_windows.md` (2026-07-14, Fable; implementer Opus), absorbed
2026-08-14. Small config/plumbing card, **independent of P1/P2** — not parked with them, simply
unbuilt and separately actionable. Touches the config loader + the accumulator-decay sites.*

**VERIFIED STILL UNBUILT (re-grepped 2026-08-15):** no `_window_s` key exists anywhere in `include/`
or `src/`. `network_carry_vce_window_epochs_{60}`, `network_emit_pb_filter_window_epochs_{60.0}`,
`prior_residual_window_epochs{60}` and `block_baarda_window_epochs{60}` are all still epoch-denominated.

### 8.1 Mandate and the general config-surface principle

Bringing up the 300 s dev tier (`network.epoch_decimation_n=10`) required hand-rescaling every
epoch-denominated forgetting window (D5 σ̂₀ calibrate 60→15, PB-filter σ̂_step 60→15, iono-deweight
robust-z 60→15) because a "60-epoch half-life" silently became 5 h of wall time. User mandate:
swapping 30 s ↔ 300 s must not require config edits. General principle for this codebase's config
surface: **thresholds in Nσ, time constants in seconds of product time, fixed magnitudes only where a
wire contract demands them.**

### 8.2 The load-bearing classification (do NOT blanket-convert)

Two knob classes, opposite correct treatments:

- **Class A — forgetting half-lives of continuous-time processes** (iono variability regime, PB
  step-noise level, σ̂₀ regime): they track wall-clock physics → convert to seconds.
- **Class B — sample-count guards** (warmups, min-sample gates, weak-prior pseudo-counts): these track
  information accumulation, which arrives per EPOCH regardless of cadence → stay in epochs/samples.
  **Converting these to seconds would be a REGRESSION.**

Initial classification — complete the survey by grepping `window_epochs|warmup_epochs|_window_` and
extending this table before implementing; the survey is part of the deliverable:

| Knob | Class | Action |
|---|---|---|
| `network.carry_invariant_innov_calibrate_window` (D5 σ̂₀ half-life) | A | add `_s` twin |
| `network.emit_pb_filter_window_epochs` (σ̂_step half-life) | A | add `_s` twin |
| `network.iono_constraint_deweight_window_epochs` (robust-z window) | A | add `_s` twin |
| `network.iono_constraint_deweight_min_epochs` | B | keep |
| `network.phase_leveling_warmup_epochs` | B | keep |
| PB filter `kPriorRows` (code constant) | B | keep |
| M2 `carry_vce` window (monitor-only) | A, low priority | optional `_s` twin |

### 8.3 Spec

- For each Class-A knob add a parallel `<name>_s` key (double, seconds of product time; default 0.0 = **inert, legacy epoch key rules — byte-identical**). When `_s` > 0 it wins; if BOTH set, warn once at load (legacy value ignored).
- Convert at the accumulator-decay site, per update, from the ACTUAL inter-update product-time gap (not nominal cadence): φ = 0.5^(dt/window_s). Measured dt makes decimation, gaps and mixed-rate replays all correct with zero further plumbing. Where an accumulator today decays once per emitted epoch with φ = 0.5^(1/window_epochs), the v2 path decays by 0.5^(dt/window_s) with dt = product-time since the previous decay.
- Startup summary logs the resolved mode and φ per knob (invariant #4-style visibility — a mis-set window should be readable in one log line).

### 8.4 Byte-identity honesty note (put this in the gates, not under the rug)

`0.5^(1/60)` (legacy) and `0.5^(30/1800)` (seconds twin at 30 s cadence) are the same real number but
different float expressions — expect possible 1-ulp divergence. Therefore OFF (=0 default) is the
byte-identity contract and the ON-equivalence gate is **numerical** (assert the two φ values agree to
~1 ulp in a unit test; the field A/B compares counters/graded metrics, not bytes). **Do not claim ON
byte-identity; do not weaken the OFF claim.**

### 8.5 Gates

- **G1 unit:** φ conversion at dt ∈ {30, 300, irregular-gap sequence}; both-set warning; 0-default inertness.
- **G2:** build + suite green; OFF byte-identity on the Run-B smoke config (same build).
- **G3 equivalence:** 30 s config with `_s` twins set to 60·30 = 1800 s vs the legacy 60-epoch run — graded metrics and run-end counters equal (ulp-level trace diffs allowed only if G1's ulp assertion documents them).
- **G4 transfer:** the 300 s tier config with the SAME `_s` values as the 30 s config (e.g. 1800 s) — calibrators converge by wall-time (≈6 epochs at 300 s), no per-tier window edits remaining. Then delete the hand-rescaled window values from `_net305_3card_dev300s_*.json` in the same commit.

### 8.6 Decay-site pointers

`NetworkPbEmitFilter::filterEpoch` (anchor `Advance the trailing window`), the D5 σ̂₀ accumulator
(anchor `carry_invariant_innov_calibrate`), `NetworkIonoDeweightGate` (header-only leaf). Config site:
`loadNetworkSection_` — as of INC-1 of the decomposition series that is
`src/core/config/GnssConfigLoadNetwork.cpp`, **not** `GnssConfigLoad.cpp` as originally written.
Context: memory `project_dev300s_tier` (the hand rescale this card retires). The original sequencing
note pointed at `docs/HANDOFF_pb_gate_v2.md` ("lands in the same leaf — sequence pb-gate-v2 first or
merge carefully"); that doc was deleted in `a8f044f` and the surviving record is memory
`project_pb_gate_v2.md` plus CLAUDE.md's verdict — **do NOT arm `emit_pb_filter_gate_v2`** (tested
Aug-4 on `runs/agu63_pbv2`, 12 h: solve byte-identical, effect null-to-negative). The leaf collision is
therefore moot; only this card will touch it.
