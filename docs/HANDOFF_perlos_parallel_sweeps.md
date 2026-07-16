# HANDOFF — Per-LOS post-solve sweep parallelization (perf card)

> **PIVOTED 2026-07-14 (user decision):** INC-P0's sub-timers attributed ~79%
> of "other" to the Card-4 invariant-carry DD driver (dense K≈7834 DD
> covariance), NOT the per-LOS sweeps (14.5% = cov-consumers, the D1 card).
> **P1/P2 below are PARKED** pending a post-pivot re-measure; the P0
> instrumentation stays live. Active card:
> [HANDOFF_carry_driver_perf.md](HANDOFF_carry_driver_perf.md).

*Written 2026-07-14 (Fable session, 300 s dev-tier bring-up). Implementer: Opus.
Companion docs: [network_decomposition_plan.md](network_decomposition_plan.md),
[network_decomposition_cutmap.md](network_decomposition_cutmap.md) (anchors AUTHORITATIVE,
its line numbers are stale — surveyed at 7,329 lines, file is now ~11.7k).*

## 1. Problem statement (measured, not conjectured)

D0 section timers (`network.trace_section_timers`) on three 11-epoch smokes,
305 stations, 300 s decimated tier (`epoch_decimation_n=10`), Release,
`parallel_stations=true` + `parallel_seam=true` (both verified armed in log):

| Run | Config | s/epoch | assembly | reduce | solve | cov-inv | cov-consumers | **other** |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| A | GPS+Gal, SH deg-15, em25 (`_net305_3card_dev300s_smoke.json`) | 51.7 | 21.0% | 8.5% | 0.2% | 0.5% | 15.4% (908 calls/ep) | **54.5% = 28.2 s** |
| B | GPS-only, SH deg-8, em10 (`_net305_3card_dev300s_gps_d8_em10_smoke.json`) | 52.8 | 3.9% | 1.6% | 0.01% | 0.05% | 13.2% (606 calls/ep) | **81.4% = 42.9 s** |
| C | B + `carry_invariant_honesty=false` | 52.9 | — | — | — | — | — | **81.4% (NULL)** |
| D | B + dd-frac OFF + carry_vce OFF + honesty OFF (`..._smoke_lean.json`) | 52.2 | 4.0% | 1.6% | — | — | 6.5% (303 calls/ep) | **87.7%** |

Run D (the "lean diagnostics" arm, estimation counters byte-identical to B): stripping
every safely-removable diagnostic bought **−1.1% wall**. dd-frac's own recon sweep was
half of all cov-consumer calls (6676→3338 = 303/ep — the H1 "+303 recon/ep" class,
reproduced), its ~39 s of bracketed time vanished, and net wall moved 6 s — the rest
is run-to-run noise (a concurrent implementation session shares this box; treat ±5%
as the single-run noise floor and re-baseline on a quiet box before publishing
speedups). **Bound: the config lever on "other" is ≤ ~8% even in the most favorable
reading. The residue is structural. This card is the fix, not config.**

Attribution by differential evidence:

- A→B collapsed the shared border 1142→534 cols (deg-15→8 removed 175 SH cols;
  GPS-only halved keys) — assembly+reduce+solve fell from 30% to 5.5%. Epoch cost
  did NOT fall.
- A→B **cut** dd-frac trace pairs 2000→500 and turned `trace_ionosphere` OFF, yet
  "other" **grew** 28.2→42.9 s/ep. The only cost-increasing delta was the
  elevation mask 25°→10° (more LOS per station). ⇒ "other" scales with LOS count.
- Run C: the Lemma-1 honesty audit is **free** (Δwall 0.3%, PB/D5 counters
  byte-identical). Eliminated as a suspect.

Conclusion: the bottleneck is the **serial per-LOS / per-station post-solve
machinery** — QC scans, health/monitor sweeps, back-substitution consumers,
emit-side prep — none of it timed individually today ("other" = loop total minus
the five timed sections).

**Target:** "other" ≤ 15 s/ep at Run-B config (≥ 2.8×), i.e. 300 s-tier 3 h run
(36 epochs) ≤ ~15 min wall. Stretch: ≤ 10 s/ep.

Baseline artifacts for before/after comparison are preserved under
`build/x64-Release/run_dev300s_{smoke,gps_smoke,gps_nohon}/` (logs + D0 totals).

## 2. What "other" contains (candidate blocks, with grep anchors)

Line numbers drift — use the anchors. All in `src/core/app/ApplicationNetwork.cpp`
unless noted. To be CONFIRMED and ranked by INC-P0 sub-timers before any
parallelization:

| # | Block | Anchor (verbatim grep) | Scaling |
|---|---|---|---|
| 1 | PASS-A slip scans (MW refloat collect, IF-TD jumps) | `sta_mw_refloat` ; `getLastIfJumps` | sta × sat |
| 2 | RAIM pass-1/pass-2 + median-polish attribution | `struct AmbStat` ; `struct RaimDetail` ; `medianPolish2Way` | sta × sat (recon per station) |
| 3 | T1a SH health sweep + H1 fused deweight gate | `T1a: SH-field health probe` | sta × LOS (recon per station) |
| 4 | Iono back-substitution supply (+trace when armed) | `reconstructStation` sites in the iono-supply block | sta × LOS |
| 5 | Phase-leveling / carry harvest + D2/C1 stash prep | `Phase-leveling carry: harvest posterior + stash` | sta × amb (leveling OFF in tier configs — may be near-zero here) |
| 6 | dd-frac trace (B5, 500 pairs in tier configs) | `B5: cross-station between-satellite DD float ambiguities` | pairs |
| 7 | eq34/re-base/PB-emit-filter + store push + solution CSV | `Card 4/M1 phase D emit re-base` ; `NetworkPbEmitFilter` | keys (~200–420) — expected small |
| 8 | Clock monitor / ClockOsc predict-update, epoch-status QC | `struct EpochStatusRow` ; `ClockOsc` | sat, sta |

~13 `reconstructStation` call sites exist post-solve (grep `est.reconstructStation`).
H1 established the "one sweep, many consumers" doctrine (its deweight gate was
fused into the T1a sweep precisely to kill +303 recon/ep — see the comment near
anchor `at ~zero marginal cov-consumer cost`). Several of the remaining sweeps are
plausibly fusable before any threading.

`cov-consumers` (13.2%, timed separately, "D1 per-station target") is a
**separate planned card (D1: selected inverse / per-station batching)** — out of
scope here; don't conflate.

## 3. Increment plan

### INC-P0 — Sub-instrument "other" (do this FIRST, no exceptions)

Extend the existing D0 machinery (`anchor: Tier D0: per-epoch section timers`)
with sub-timers around blocks 1–8 above, under the same `sec_timers_on` gate.
Accumulate run totals + per-epoch readout exactly in the D0 style ("D0b" lines).
Log-only change: every CSV must be byte-identical; only `main_log` grows lines.

**Gate P0:** run `_net305_3card_dev300s_gps_d8_em10_smoke.json` (see §5 run
recipe), produce the ranked table. Everything below targets the measured top
blocks — if the ranking contradicts §2's guesses, follow the measurement.

### INC-P1 — Sweep fusion (serial, byte-identity-checked)

Merge redundant per-station `reconstructStation` sweeps whose consumers are
read-only on `x_r` and mutually independent within the epoch (verify per pair —
consumer B must not read state consumer A writes this epoch). One recon per
station per epoch is the ideal. This is pure reordering of read-only consumers:
byte-identity ON is achievable and REQUIRED (float ops per consumer unchanged,
only the recon call count drops).

**Gate P1:** CSVs + run-end counters + emitted store byte-identical to pre-P1
build on the Run-B smoke AND on one 30 s config
(`_net305_3card_gpsonly_30m.json`). Report recon-call-count before/after (the
cov-consumers "calls" figure in D0 is the counter).

### INC-P2 — Deterministic parallel fan-out of the fused sweep(s)

New toggle following the CLAUDE.md convention (member + getter + `jval` in
`loadNetworkSection_` + summary log + config-echo JSON block near
`"parallel_seam":` + L15 manifest):

```cpp
bool network_parallel_sweeps_{false};  // default OFF = byte-identical
```

Pattern = the proven `parallel_seam` seam (commit bb0a7ab; anchor
`Deterministic-parallel post-solve seam`):

1. Partition stations across threads (`#pragma omp parallel for` — the two
   existing regions use `schedule(dynamic)` legally because every iteration
   writes ONLY its own indexed slot; keep that discipline).
2. Each thread computes into a **pre-sized per-station slot** (the
   `StationAssembly` precedent: flags + payload per slot, written by exactly one
   thread; per-station try/catch → error flag in slot, never throw across the
   OMP boundary — existing pattern at the recon try/catch sites).
3. **ALL shared-state mutation drains SERIALLY afterward, in station-index
   order**: trace/CSV rows (buffer per-station strings, drain in order — files
   byte-identical), network-level medians/robust stats (collect values in the
   parallel pass, compute serially — `medianCopy`'s upper-middle convention is
   load-bearing), monitor updates (per-station EWMA state is slot-local = safe;
   anything cross-station updates in the drain), counters.
4. **Float-accumulation rule:** any Σ over stations stays serial+fused in the
   drain (the GMGᵀ lesson from the seam card: parallel accumulation drifted
   ~1e-4 — never again).

**Thread-safety verification (required, do not assume):**

- `NetworkEstimator` is documented **"Not thread-safe"**
  (`include/filter/NetworkEstimator.hpp`, class banner), but
  `reconstructStation` is `const` and the kernel is documented pristine after
  `solve`. Audit the full const path (`NetworkNormalEquations::reconstructStation`
  and anything it touches) for mutable members / lazy caches. If a lazy cache
  exists, pre-warm it serially before the fan-out. Update the class Doxygen with
  the exact concurrent-const-use contract you establish.
- **Known race:** the D0 perf-timing counters (`est.setPerfTiming`) — the
  cov-consumer wall/call accumulation inside the estimator is mutable state hit
  BY `reconstructStation` calls. Fix: per-thread accumulation reduced serially,
  or thread-local timing, or (minimum) document that `parallel_sweeps` +
  `trace_section_timers` degrades those two numbers and gate the timing calls —
  but do NOT let timing mutate shared state from OMP threads.
- spdlog calls inside the parallel region: thread-safe but line-ORDER varies —
  route any per-station log lines through the slot + drain, or demote to the
  buffered trace. Byte-compare gates (below) intentionally exclude `main_log`
  line order; do not weaken that by claiming log diffs are "just ordering".

**Gate P2 (the decisive one), all on the SAME build:**

- OFF = byte-identical to pre-card baseline (structural: toggle never read →
  paths unreachable).
- ON vs OFF: byte-identical `network_trace.csv`, `network_solution.csv`,
  `network_dd_fractional.csv`, `network_vtec_sh*.csv`, `network_carry_vce.csv`,
  all run-end counters (D5 / PB filter / re-base / deweight / RAIM), and the
  emitted product store — on (a) the Run-B 300 s smoke and (b) a 30 s 30m
  lineage config.
- Thread-count invariance: ON at 2 threads vs ON at 20 threads → identical
  outputs (the per-station compute claim already made by `parallel_stations` —
  match it).
- Suite green (~700 cases), zero new warnings beyond the known C4828 set.

**Gate P3 (perf):** D0 + P0-sub-timer before/after table on Run-B config.
Accept ≥ 2× on "other"; target ≤ 15 s/ep. Report honestly if a block resists
(e.g. inherently serial store push) — that residue defines the next card.

## 4. Interaction with the decomposition plan

Cutmap INC 9 (Raim → `NetworkRaim.{hpp,cpp}`) and INC 11 (Emit →
`ApplicationNetworkEmit.cpp`) will RELOCATE most of these blocks. This card
lands **before** those increments (anchors, not line numbers, so the rebase is
mechanical). If decomposition has already started when you pick this up, land
P0/P1 in whatever file owns the block at that time and tell the decomposition
doc's owner — do not fork the sweep logic across files.

## 5. Run recipe / conventions (non-negotiable, from CLAUDE.md + memory)

- Build: vcvars pattern (CLAUDE.md §Build). Release ONLY for any timed or data
  run. `gnss_tests` may run from Debug.
- Run from a **renamed exe copy** (`gnss_processor_opus_run.exe`) so concurrent
  sessions never hit a locked exe; `LNK1104` = transient lock → retry. Pre-kill
  `gnss_processor*` zombies before timed runs (WER wedge).
- Timed runs: fresh working dir under `build/x64-Release/` (logs land in
  `<cwd>/logs`). Invocation: `gnss_processor_opus_run.exe <config.json>`.
- Smoke first (10-epoch class) before anything longer; the 300 s tier smoke IS
  the perf harness here — no long runs are needed for this card at all.
- Byte-identity comparisons are same-build only (verification integers are
  build-config-sensitive).
- No `Co-Authored-By` trailer on commits.
- New `.cpp` files (P0 shouldn't need any; P2 shouldn't either) must be added to
  `CMakeLists.txt` explicitly. Match the existing heavy Doxygen style on any new
  public API.

## 6. Non-goals

- `cov-consumers` reduction (D1 selected-inverse / batching) — separate card.
- Estimator-internal threading (solve is 0.2% — dead end).
- Any change to estimation results, gate behavior, or emitted values — this card
  is wall-clock only, byte-identity is the contract.
- `carry_invariant_honesty` — measured free; leave it alone.
- The emit/re-base/PB-filter chain (block 7) — expected small; only touch if P0
  ranks it top-3, and then prefer fusion over threading (it is order-sensitive
  by design).

## 7. Context pointers

- Memory: `project_dev300s_tier.md` (tier + these measurements),
  `project_perf_deterministic_parallel_seam.md` (the seam precedent + GMGᵀ
  drift lesson), `project_network_decomposition_plan.md`.
- The three baseline run dirs listed in §1 (D0 RUN TOTALS at the end of each
  `logs/main_log.log`).
- Tier configs: `config/test_suite/_net305_3card_dev300s_gps_d8_em10_{smoke,3h}.json`
  (300 s), `_net305_3card_gpsonly_30m.json` (30 s gate config).
