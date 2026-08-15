# HANDOFF — Rate-transferable adaptive windows (seconds-denominated time constants)

*Written 2026-07-14 (Fable). Implementer: Opus. Small config/plumbing card;
touches `GnssConfigLoad.cpp` + the accumulator-decay sites — coordinate the
config merge point with the parallel-sweeps and pb-gate-v2 cards.*

## 1. Context

Bringing up the 300 s dev tier (`network.epoch_decimation_n=10`) required
hand-rescaling every epoch-denominated forgetting window (D5 σ̂₀ calibrate
60→15, PB-filter σ̂_step 60→15, iono-deweight robust-z 60→15) because a
"60-epoch half-life" silently became 5 h of wall time. User mandate: swapping
30 s ↔ 300 s must not require config edits. General principle for this
codebase's config surface: **thresholds in Nσ, time constants in seconds of
product time, fixed magnitudes only where a wire contract demands them.**

## 2. The load-bearing classification (do NOT blanket-convert)

Two knob classes, opposite correct treatments:

- **Class A — forgetting half-lives of continuous-time processes** (iono
  variability regime, PB step-noise level, σ̂₀ regime): these track wall-clock
  physics → convert to seconds.
- **Class B — sample-count guards** (statistical convergence: warmups,
  min-sample gates, weak-prior pseudo-counts): these track information
  accumulation, which arrives per EPOCH regardless of cadence → stay in
  epochs/samples. Converting these to seconds would be a regression.

Initial classification (Opus: complete the survey by grepping
`window_epochs|warmup_epochs|_window_` and extending this table in the card
before implementing — the survey is part of the deliverable):

| Knob | Class | Action |
|---|---|---|
| `network.carry_invariant_innov_calibrate_window` (D5 σ̂₀ half-life) | A | add `_s` twin |
| `network.emit_pb_filter_window_epochs` (σ̂_step half-life) | A | add `_s` twin |
| `network.iono_constraint_deweight_window_epochs` (robust-z window) | A | add `_s` twin |
| `network.iono_constraint_deweight_min_epochs` | B | keep |
| `network.phase_leveling_warmup_epochs` | B | keep |
| PB filter `kPriorRows` (code constant) | B | keep |
| M2 `carry_vce` window (monitor-only) | A, low priority | optional `_s` twin |

## 3. Spec

- For each Class-A knob add a parallel `<name>_s` key (double, seconds of
  product time; default 0.0 = **inert, legacy epoch key rules — byte-identical**).
  When `_s` > 0 it wins; if BOTH set, warn once at load (legacy value ignored).
- Conversion at the accumulator-decay site, per update, from the ACTUAL
  inter-update product-time gap (not nominal cadence): φ = 0.5^(dt/window_s).
  Using measured dt makes decimation, gaps, and mixed-rate replays all correct
  with zero further plumbing. Where an accumulator currently decays once per
  emitted epoch with φ = 0.5^(1/window_epochs), the v2 path decays by
  0.5^(dt/window_s) with dt = product-time since the previous decay.
- Startup summary must log the resolved mode and φ per knob (invariant #4-style
  visibility — a mis-set window should be readable in one log line).

## 4. Byte-identity honesty note (put this in the gates, not under the rug)

`0.5^(1/60)` (legacy) and `0.5^(30/1800)` (seconds twin at 30 s cadence) are
the same real number but different float expressions — expect possible 1-ulp
divergence. Therefore: OFF (=0 default) is the byte-identity contract; the
ON-equivalence gate is **numerical** (assert the two φ values agree to ~1 ulp
in a unit test; the field A/B compares counters/graded metrics, not bytes). Do
not claim ON byte-identity; do not weaken the OFF claim.

## 5. Gates

- **G1 unit**: φ conversion at dt ∈ {30, 300, irregular-gap sequence}; both-set
  warning; 0-default inertness.
- **G2** build + suite green; OFF byte-identity on the Run-B smoke config
  (same build).
- **G3 equivalence**: 30 s config with `_s` twins set to 60·30=1800 s vs legacy
  60-epoch run — graded metrics and run-end counters equal (allow ulp-level
  trace diffs only if G1's ulp assertion documents them).
- **G4 transfer**: the 300 s tier config with the SAME `_s` values as the 30 s
  config (e.g. 1800 s) — calibrators converge by wall-time (≈6 epochs at
  300 s), no per-tier window edits remaining. Then delete the hand-rescaled
  window values from `_net305_3card_dev300s_*.json` in the same commit.

## 6. Pointers

Decay sites: `NetworkPbEmitFilter::filterEpoch` (anchor `Advance the trailing
window`), the D5 σ̂₀ accumulator (anchor `carry_invariant_innov_calibrate`),
`NetworkIonoDeweightGate` (header-only leaf). Config: `GnssConfigLoad.cpp`
`loadNetworkSection_`. Context: memory `project_dev300s_tier` (the hand
rescale this card retires), `docs/HANDOFF_pb_gate_v2.md` (lands in the same
leaf — sequence pb-gate-v2 first or merge carefully).
