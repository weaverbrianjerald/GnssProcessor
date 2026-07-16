# SPEC — MW-seeded wide-lane layer (M5 card "WL-1")

**Status:** SPEC (Jul-15 2026, Fable). Implementation target: Opus session.
Verification: Fable session after. Default-OFF, byte-identical when off.

**One sentence:** feed the MW detector's per-arc Welford means, double-differenced
over the Card-4 fundamental-cycle chord basis, into the solve as rank-K
pseudo-observation blocks on cross-band ambiguity columns — giving the network
the per-arc code-coherence accumulator it structurally lacks, and making the
wide-lane integers reachable in tens of epochs (Phase A float, Phase B fix).

---

## 1. Motivation — the Jul-15 evidence chain (do not relitigate)

- Estimator-free MW census from raw RINEX (30 ep, 305 sta, C2W/L2W star DDs):
  **circR = 0.62, spread 0.16 cyc = the code-noise prediction** — WL integers
  are in the data, 1/√N-clean. `scripts/mw_census.py`.
- KF-implied WL (dd-frac trace, L1−L2 per pair): **uniform (circR 0.001–0.11)
  in EVERY configuration** — SH on/off, IF on/off, 30/60/300 s, `code_bias_q`
  and `ionosphere_q` A/Bs (both proven null: no receiver-CB states exist on the
  network path; iono-q change gave a byte-identical epoch-5 NEQ).
- Root cause: locals (per-LOS iono, ZWD, rclk, RPB) are epoch-fresh in the
  3card lineage; the only cross-epoch transports are the invariant DD carry
  (phase-side) + border carry. Long-horizon CODE coherence — which is what
  determines N_WL — has no container. See memory
  `project_wl_structural_gap_jul15` and `docs/NIGHTRUN_REPORT_2026-07-15.md`.
- Consequence for AR ordering: WL is the earliest-converging integer functional
  (iono-insensitive, code-noise-limited, σ≈0.5 cyc/√N_ep); per-band N₁/N₂
  separation is iono-limited (hours). Classical cascade: WL fixed ⇒ NL is a
  single integer per pair at λ_eff ≈ 10.7 cm on mm-precision IF phase ⇒ N₁, N₂
  recovered exactly, per band. WL-1 is the first rung; it does NOT replace the
  uncombined estimator — it adds missing information INTO it.

## 2. Estimability (why this is S-system-legal)

Undifferenced MW = λ_WL·N_WL + b_WL,r + b_WLˢ + ε — datum-contaminated; never
used directly. The card constrains ONLY closed-cycle (DD) functionals: for a
fundamental cycle C in the (station,sat) arc graph,
Σ_C ±MW̄_arc = λ_WL·(∇ΔN₁ − ∇ΔN₂) + Σ_C ±ε̄_arc — both bias lumps cancel
IDENTICALLY (each receiver and satellite appears with +/− once per cycle).
∇ΔN_WL is an integer combination of the canonical Odijk estimable set — the
SAME invariance class as the Card-4 carried chords. Gauge transformations
(clock shifts, PB re-levels, pivot handovers) have zero component along these
rows ⇒ the W5 null-space gate must be provably untouched (FIREWALL test, §7).
BIA-lessness of the network MW is IRRELEVANT in this form (biases cancel in the
cycle; we never need the absolute WL integer of a single arc).

**No pivot, no second S-system (conceptual guard):** the WL rows define NO
datum — nothing is pinned, no reference satellite/station is privileged. The
spanning tree is a BASIS choice for the cycle space (any tree spans the same
invariant subspace; tree swaps re-express without information change), NOT an
S-basis choice. The constraints are S-system-invariant MEASUREMENTS applied to
the network's states inside the network's ONE S-system (eq34 et al.); they are
valid unchanged under any datum the network runs. Classical BSSD/BRSD pivots
eliminate states (a datum act); this card differences derived arc STATISTICS
(a measurement act). Corollary for §3.1: read the detector's RAW undifferenced
MwArcStats — never its level-mode / pivot-referenced outputs, which DO carry
the user-path pivot convention.

## 3. Mechanism

### 3.1 Source statistics (per station, no new math)
`MwSlipDetector::MwArcStats` already maintains per-arc {mean, M2 (Welford),
count} in meters (MwSlipDetector.hpp ~464). Add a const accessor if none is
public: `arcStats(sat, pair) -> {mean_m, var_of_mean_m2 = M2/(n(n-1)), n,
arc_start_s}`. The detector's own arc lifecycle (slip flag ⇒ stats reset;
gap ⇒ new arc) IS the reset wiring — variance of a fresh arc is honest-huge.

**Construction gate change:** detectors currently exist only under
`networkPhaseLeveling() && (turbo_edit || mw_detect)` (ApplicationNetwork
~1130). Relax to `... || networkMwWlSeed()` so the card works in the 3card
lineage (phase_leveling stays OFF). Keep OFF byte-identical: detectors are
observation-side only; when the card is off nothing else changes.

### 3.2 Rows — ride the Card-4 chord basis (cross-band blocks)
For each carried chord (a fundamental cycle, per (const, signal-group) graph),
the Card-4 driver already owns the cycle arc list with signs. The WL row for a
cycle couples the L1-band and L2-band ambiguity columns of the SAME
station/sat cycle:

  row terms: for each arc (r,s) in the cycle with sign c:
    (+c/λ₁ on station r's L1 amb column of sat s) and
    (−c/λ₂ on station r's L2 amb column of sat s)          [columns in meters]
  z (cycles) = Σ_C ±MW̄_arc / λ_WL
  R (cycles²) = Σ_C var_of_mean / λ_WL²  (+ a floor, §4)

`NetworkNormalEquations::DdPriorBlock::Term{station, local_col, coeff}` rows
are ALREADY general signed combos across stations
(NetworkNormalEquations.hpp ~349) and `foldReducedDdPriors` folds arbitrary
rank-K blocks. Package the epoch's WL rows as one or more NEW DdPriorBlock
instances appended to the same `dd_priors_` fold path (diagonal Pz = R;
zhat = z). NO kernel change expected — verify the fold has no per-band
assumption (it does not; it sees only Terms).

**Band/channel discipline:** build a WL row ONLY when every arc in the cycle
has both bands present on a consistent signal pair at that station, and the MW
stats were computed on the SAME (C/L) channels the estimator uses for that
station (channel mixing leaves DCB residue — the mw_census lesson). Skip
otherwise; count skips.

### 3.3 Phase A — float WL constraint (the card's core)
Every epoch, for every eligible chord-cycle with n ≥ `min_arc_epochs` on all
arcs: append the WL pseudo-obs block. That is all. The joint solve tightens
the WL direction; per-band DD posteriors inherit; the carried z's stop
wandering along WL; D5 flag rate should DROP (its innovations lose their
largest component). No integers are asserted in Phase A.

**Double-count guard (required):** the current epoch's P1/P2/L1/L2 are in the
solve AND inside a same-epoch arc mean — a 1/n information double-count.
Inject the LAGGED arc statistics (mean/var/n as of the END of the PREVIOUS
epoch, i.e. snapshot the stats before this epoch's addMeasurement), making the
pseudo-obs independent of the current epoch's measurement noise. Residual
temporal correlation (multipath) is what `sigma_floor_cyc` prices; the M2
LS-VCE monitor is the honesty check (watch σ̂ on the WL rows in validation).

### 3.4 Phase B — WL integer fixing (separate toggle, after A is graded)
When a cycle's constrained WL posterior passes the FFR gate
(σ_WL < `fix_sigma_cyc`, |frac| < `fix_frac_cyc`, both configurable, defaults
0.10/0.15): replace the pseudo-obs with the rounded integer at
`fix_weight_sigma_cyc` (hard-ish row, NOT an exact constraint — leaves a
rollback path). Un-fix immediately when any member arc slips/reset (the
detector arc-id changes) — wired + force-fire tested BEFORE any field ON run
(standing invariant #1; the C3b lesson applies verbatim).

### 3.5 Phase D — EWL/L5 cascade (after B; the true single-epoch lane)
For the fleet subset tracking L5, the extra-wide lane (L2−L5, λ_EWL =
c/(f₂−f₅) ≈ 5.861 m) has single-epoch DD σ ≈ 0.05–0.12 cyc from code noise
alone — **one-epoch rounding is safe** (P(wrong) ≪ 0.1%), no averaging window
needed. Extend the row builder to (L2,L5) EWL cycles with
`min_arc_epochs = 1` eligibility and the same FFR gate; the fixed EWL then
(a) cross-checks the MW WL (three-band closure: EWL + WL constrain the same
integer lattice), and (b) gives the L5 subset the full near-instant ladder
5.86 m → 0.86 m → 0.107 m. Same estimability argument (closed cycles, biases
cancel); same channel discipline per band. Caveat: L5X receiver-side ATX
fallback inflates L5 PHASE residuals on some stations (known, not a bug) —
irrelevant to the EWL integer content but keep L5 out of NL rows in v1.

## 4. Config surface (all `network.mw_wl.*`, loader in loadNetworkSection_)

| key | default | meaning |
|---|---|---|
| `enable` | false | Phase A master. OFF = byte-identical (no detectors built for the card, no blocks). |
| `min_arc_epochs` | 10 | Welford n per arc before its cycles are eligible. |
| `sigma_floor_cyc` | 0.05 | R floor per row (multipath honesty; MAD-vs-Welford consistency check deferred, §9). |
| `max_rows_per_epoch` | 0 | 0 = unlimited; else lowest-R rows first (perf guard; count drops). |
| `fix_enable` | false | Phase B master (requires `enable`). |
| `fix_sigma_cyc` / `fix_frac_cyc` | 0.10 / 0.15 | FFR-style gate per cycle. |
| `fix_weight_sigma_cyc` | 0.01 | fixed-row σ (soft-hard; rollback-able). |

Also add to the toggle summary log line + `isLoggerEnabled` if user-visible
(CLAUDE.md convention).

## 5. Lifecycle / slip response (invariant #1 pre-flight)

- Phase A rows are RE-DERIVED each epoch from live detector state — nothing is
  carried by the card itself; there is no stiff prior to go stale. Arc slip ⇒
  detector resets stats ⇒ next epoch's row has honest-huge R (or is skipped by
  `min_arc_epochs`). Gap ⇒ same via the detector's arc lifecycle.
- Phase B fixes ARE stiff assertions ⇒ explicit un-fix on member-arc slip
  (delete the fixed row; the cycle reverts to Phase A), plus a D5-style
  innovation check on fixed rows (|z_solve − z_fix| > k·σ ⇒ un-fix + count).
- Coordinate with Card-4: a chord refloat (D5/coarse) does NOT invalidate the
  MW stats (the detector arc may be clean while the carry re-taps) — the WL row
  persists and is precisely what should stop spurious re-taps. A DETECTOR slip
  invalidates both (already funneled via carry_slipped_ambs).

## 6. Telemetry (invariant #4 — counters or it didn't happen)

Run-end line: rows built / folded, cycles eligible / skipped (by reason:
missing band, young arc, channel mismatch), Phase-B fixes applied / active at
end / un-fixed (by reason), mean R (cyc), and the D5 flag rate (already
logged) for cross-reference. Per-epoch debug line behind the existing trace
discipline. Add `mw_wl` fields to the dd-frac grading flow only if needed —
grading stays `grade_ddfrac.py` + `scripts/mw_census.py` as the external
truth.

## 7. Validation plan (Fable, after Opus lands it)

1. **Byte-identity OFF** (structural): full-toggle run vs pre-card build —
   counters + CSVs identical (same-build comparison per invariant #6).
2. **FIREWALL** (unit, blocking): fold a WL block in the gate-test fixture and
   assert `assertNullSpaceRank` nullity UNCHANGED (the §2 claim, mechanically —
   mirror the phase-leveling Inc-1 firewall test).
3. **Unit battery:** row construction (signs, λ scaling, channel discipline),
   R propagation from Welford, Phase-B gate + un-fix on synthetic slip
   (force-fire), eligibility counters.
4. **Falsifier FIRST (10-ep smoke, 50 sta):** inject a known integer slip into
   one arc mid-window (or use the C3c falsifier fixture pattern) — assert the
   affected cycles' rows widen/drop and no wrong integer is asserted. Arms +
   force-fires per the staged-cadence directive.
5. **Rung runs (50 sta, minutes each):** KF-WL circR vs the MW census on the
   same window — Phase A success = circR climbing toward ~0.6 as arcs mature;
   D5 rate drop is the secondary marker. NEQ export (`neq_export_epoch`) +
   `scripts/neq_svd.py`: the WL-direction marginals must visibly tighten.
6. **Graded 3 h @30 s (305 sta, em10):** vs the R5 em10d8 control — dd-frac
   clustering emergence, DD-step churn, D5 trend, PB wire health (gate_v2
   unchanged). Grade datum-invariant quantities only (invariant #5); Release
   only (#2); pcarry per its usage rule.
7. **Phase B only after Phase A grades clean.**

## 8. Perf notes

Rows add rank-K_WL to the existing fold: K_WL ≤ #carried chords per band pair
(~2-3k at 305 sta) but each row is 8 terms — the fold's per-station reduction
absorbs them like DD priors. Expect small; measure with the D0/P0b timers. If
the fold grows, `max_rows_per_epoch` is the guard (count what's dropped — no
silent caps). The netar Phase-2 probe now has `ils_max_dim` (default 0;
100 at scale) — REQUIRED for any fixability logging at 305 sta.

## 9. Deferrals (triaged per invariant #3)

- **Field-inert:** GLONASS (FDMA — excluded like netar); cross-day arc
  spillover (bounded by detector max-gap); Welford-vs-MAD R cross-check
  (sigma_floor covers; revisit if calib shows optimism); L5/third-band EWL
  cascade (pure extension).
- **Field-blocking if skipped:** none identified — Phase A is runnable once
  §7.1-7.4 pass. Phase B without the un-fix wiring is NOT field-runnable
  (invariant #1) — do not arm `fix_enable` before §7.4's force-fire passes.

## 10. Verified anchors (Jul-15 tree, uncommitted state)

- `MwSlipDetector::MwArcStats{mean, M2, count}` — include/slip/MwSlipDetector.hpp ~464.
- Detector construction gate — src/core/app/ApplicationNetwork.cpp ~1126-1139.
- `DdPriorBlock::Term{station, local_col, coeff}` general rows —
  include/filter/NetworkNormalEquations.hpp ~349-358; fold =
  `foldReducedDdPriors` (same file ~360+); driver block assembly =
  NetworkInvariantCarryDriver (owns cycle arc lists + signs).
- h2local (StateManager idx → local col) available at the epoch call sites
  (`asm_res[i].h2local`); per-band ambiguity columns via
  `StateManager::getAmbiguities()` + `StateKeys::parseAmbiguityKey`.
- Config conventions: GnssConfigTypes struct + loadNetworkSection_ jval — see
  `neq_export_epoch` (added Jul-15) as the freshest template.
- Concurrent uncommitted Jul-15 work in the same files: NEQ export
  (NetworkNormalEquations/NetworkEstimator accessors, ApplicationNetwork dump
  block ~8455), netar `ils_max_dim`. Re-grep all anchors before editing.

## 11. Open questions for the implementer (answer in the handoff back)

1. Cycle basis reuse: read chords + signs directly from the carry driver's
   block metadata, or rebuild an independent cycle basis from the arc graph?
   (Prefer reuse — one graph truth; but the WL layer must also work when
   `carry_invariant` is off? Decide: v1 REQUIRES carry_invariant ON — mutex-
   check at load like the other coupled toggles.)
2. Where the MW stats are read: PASS-A tail (station-local, parallel-safe,
   like the Inc-2 protocol) with a serial drain into block assembly — confirm
   no shared-write hazards under parallel_stations.
3. Signal-pair selection when a station tracks >2 GPS bands (L5): v1 = the
   estimator's primary L1/L2 pair only; L5/EWL deferred.
4. **Loose vs tight coupling (STUDY, do not block v1 on it):** this SPEC is the
   loosely-coupled form (detector stats as lagged pseudo-obs) — zero new
   states, arc lifecycle = reset wiring, hot path untouched. The
   tightly-coupled alternative (per-arc WL states, or a per-LOS iono
   LocalCarry so the estimator accumulates code coherence natively) is
   statistically purer (no summary-statistic approximation, exact correlation
   bookkeeping) but re-opens the carried-state obligations (slip wiring,
   staleness, datum interactions — the Jun-30/Jul-10 saga class). Recommend:
   implement loose v1; if the LS-VCE honesty check on the WL rows shows
   sustained σ̂ ≫ 1 (multipath correlation defeating the σ²/n model), the
   tight form is the fallback. Document the verdict either way.
5. **Phase C (product-side extension, post-B):** with DD WL integers fixed,
   the satellite WL bias (UPD) network becomes estimable up to the standard
   per-component datum — the network can then EMIT WL phase-bias products for
   consumers, still fully self-contained (no .BIA anywhere). Out of scope for
   this card; note the hook.
