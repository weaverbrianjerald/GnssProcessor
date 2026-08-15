# CARD 1b — Coupling rows: JOINT whitening (fixes the inert-carry flaw in Card 1)

*Fable-specced 2026-07-10 after grading runs/t7_joint_lowfloor_12h. Card 1 as built is
CORRECT per its spec and its unit gate — the spec itself under-modelled one term. Do not
refactor Card 1; this card only changes how the coupling rows' noise is applied.*

## Evidence + mechanism (why your Card-1 work shows a flat σ)

T7 (Cards 1+2, low floors, 12 h): NO divergence (a00 ±35 TECU — Card 1's objective
achieved) and σ-calibration ~1 (honest covariance) — but formal DD σ is FLAT at ~5-6
cycles for 12 h (per-epoch code level; hour-by-hour medians 5.8→4.8). Mechanism: each
carried ambiguity's regression row M_r contains the WHITE shared columns (satellite
clocks under clock_model, any un-stashed border column). Card 1 marginalizes those
per-row into a SCALAR σ² (dep_var), which is huge — so every row is individually
deweighted to nothing. But those terms are COMMON across the station's rows: in any DD
they cancel. Marginalizing diagonally destroys the between-row correlation that carries
the DD information — the carry becomes inert while remaining honest (exactly the
observed flat-σ + calib≈1 signature).

## The fix (exact; mirrors an existing pattern)

Per station, per epoch, build the coupling rows AS A GROUP:
1. Partition each row's shared terms: PRESENT stashed columns stay as A_s coefficients
   (unchanged from Card 1); WHITE/DEPARTED columns (no stash entry, or a white per-epoch
   state such as sat clocks when clock_model carries them outside D2) go into a
   marginalization matrix `M_w` (n_rows × n_white) with per-column prior variance
   `P_w,jj` (stash P for departed; the state's a-priori/initial variance for white).
2. Row noise becomes the JOINT covariance
   `R = diag(var_eff_r) + M_w · diag(P_w) · M_wᵀ`  (n_rows × n_rows, SPD).
3. LLT-whiten rows, A_s block, A_r block, and RHS by the SAME factor — copy the
   per-constellation whitening in `appendShIonoRows` verbatim (`llt.matrixL().solve`).
   The huge-variance common directions then act as common-mode-free: information
   ORTHOGONAL to them (the DD content) is preserved instead of destroyed.
4. Group scope: all coupling rows of ONE station in one whitening block (they only
   correlate within a station). Cost: one LLT of ~n_carried (≈30-40) per station — trivial.
5. Numerical guard: if the LLT fails (R not SPD from rounding), add 1e-12·max(diag) to
   the diagonal once and retry; if it still fails, fall back to Card-1 per-row scalars
   for that station THIS epoch and count it (log a WARN with the count per run).

## Before coding — 15-minute verification of the mechanism (do first, report numbers)

Instrument (temporarily or behind a trace flag) the Card-1 row construction on the T7
config's first 10 epochs: log, for a sample of rows, `var_eff` vs `dep_var`/white-term
contribution. CONFIRM dep_var/white dominates (≥10× var_eff) — that is this card's
premise. If it does NOT dominate, STOP and report (the flat σ then has a different
cause and this card is the wrong fix).

## Validation

1. Unit gate: extend `ConditionalCarryRecursiveEqualsBatch` with a WHITE shared column
   (per-epoch re-estimated) in the synthetic: recursive-with-joint-whitening == batch to
   1e-9; ALSO assert the Card-1 scalar path does NOT match in this scenario (documents
   why 1b exists).
2. Suite green (≥ 602/1); byte-identity OFF: `_t4_gg_treated_smoke.json` SHA ==
   `69C20D38E22B5EBE` on a FRESHLY REBUILT x64-Release (verify exe mtime > source mtime
   — stale-exe validation has burned us three times).
3. **Falsifier: rerun `config/test_suite/_t7_joint_lowfloor_12h.json`** (same toggles).
   SUCCESS = hour-by-hour median dd_sigma_cyc DECLINES (~1/√k trend toward ≲0.3 cyc by
   hour 12 is plausible at code σ 0.3 m), calib stays ~1, a00 stays bounded, no
   divergence. Grade with the Fable grader (metrics documented in memory
   `project_network_ddfrac_rootcause_shvtec`). Report 1 h/3 h tails + the σ-by-hour
   table + |frac| deciles. Either fraction outcome is a finding — do not tune.

## Out of scope

Card 2 code (leave as is), refloat/RAIM gating (arm T2d separately only if the σ trend
shows arc-lifetime limiting AFTER this fix), floors, datum work, kernel reduce/folds.

## ADDENDUM (2026-07-10 late — Fable verification of the 1b implementation): ARMED BUT INERT + 10× SLOWDOWN

Measured (runs/t7c_jointwhiten_12h, toggle armed, vs runs/t7_joint_lowfloor_12h): output
BYTE-IDENTICAL through the first 3,926 trace rows (sha-confirmed) while running ~10×
slower (11 s/epoch vs 1.1; D0-bracketed sections unchanged at ~0.5 s — the extra ~10 s
is in UNBRACKETED new code). Conclusion: the whitening computes and is then discarded,
or M_w is empty so R degenerates to Card-1 diagonals (a mathematical no-op) while an
unrelated hot loop burns the time.

Debug order (do 1 before touching anything):
1. RUN THE PREMISE CHECK THE CARD REQUIRED (it was skipped): for the first 10 epochs log
   per station: n coupling rows, n M_w columns, per-column P_w, and var_eff vs the
   marginalized term for 5 sample rows. If M_w is EMPTY: the white-column classification
   is wrong — satellite clock columns under network.clock_model are NOT in the D2/dense
   stash and MUST be classified white (P_w = the clock's a-priori variance), not
   "present". That classification is the entire point of the card.
2. Verify the whitened blocks are the ones actually appended (not a whitened copy
   discarded — check the A_s/A_r/y written after llt.solve are the solved ones).
3. Find and D0-bracket the ~10 s/epoch hot loop in the new code (suspect: M_w assembly
   scanning all shared columns per row → make it per station, keyed lookups).
4. CHEAP falsifier BEFORE any long run: 200-epoch A/B vs runs/t7_joint_lowfloor_12h —
   the first 3,926 trace rows must NOT hash-match (python one-liner in the Fable
   verification), per-epoch wall must return to ~1 s, and the epoch-median dd_sigma_cyc
   should already bend below T7's flat ~5.8 within the window. Only then rerun the 12 h.

## RESULT (2026-07-10, Opus — all four addendum steps executed): PREMISE FALSIFIED; toggle is a NO-OP; no hot loop

Instrumented the Card-1b row construction (census of M_w columns + var_eff vs the
marginalized dep term, per station, first 40 epochs) and ran a clean, matched
scalar-vs-joint A/B (`config/test_suite/_t7_ab_{scalar,joint}.json`, 200 epochs /
3.3 h, the ONLY differing key = `carry_coupling_joint_whiten`). Instrumentation was
log-only (byte-identity preserved) and has been reverted; the exe is rebuilt clean.

**Step 1 — premise check (DECISIVE, falsifies the card):**
- `M_w` is EMPTY in 723/735 station-epochs (98.4%). When non-empty (12/735) it holds
  1–2 columns — and every one is kind **CodeBias, never a satellite clock** (T7 has no
  `clock_model`, so no sat-clock state ever departs; the addendum's clock-column
  reclassification has no referent in this config).
- The marginalized term is **10–1000× SMALLER than `var_eff`** (`dep` ≈ 1e-3…0.16 m² vs
  `var_eff` ≈ 0.6…4.3 m², and ≈5e4–9e4 m² on new arcs). The card REQUIRED `dep ≥ 10×
  var_eff`; the measurement is the exact opposite. **Per the card's own gate (line 45):
  STOP — the flat σ has a different cause and this card is the wrong fix.**

**Step 2 — whitened blocks: appended correctly, NOT discarded.** The append (4551–4557)
and refloat re-whiten (6113–6117) write the SOLVED `L⁻¹` blocks (`Hr/Hs/z`) with unit
weight; the gate test `JointWhitenCouplingMarginalizationEqualsBatch` proves the math
when M_w is non-diagonal. The addendum's "compute-and-discard" hypothesis is wrong: it
is compute-and-apply, but the applied result EQUALS the scalar path because M_w is
diagonal in ~all cases (empty, or each departed column touches one row).

**Step 3 — the "~10 s/epoch hot loop" is a PHANTOM.** Measured wall: scalar
**434.7 ms/epoch**, joint **439.2 ms/epoch** (200-epoch matched A/B); premise run
435.7 ms/epoch; the finished 12 h t7 baseline 478.8 ms/epoch. No 10×, no unbracketed
hot loop. The addendum's "11 s/epoch" is the **total epoch-loop wall (11 328.8 ms over
26 epochs) misread as per-epoch** (÷26 = 436 ms). D0 breakdown normal: assembly 60%,
reduce 19%, other 17% (pre-existing QC/emit/tracers).

**Step 4 — falsifier: byte-identical, i.e. the toggle changes NOTHING.** scalar vs
joint `network_dd_fractional.csv` are byte-identical across ALL 10 614 rows (not just
3 926); `network_solution.csv`, `network_products*.txt`, `network_vtec_sh.csv`,
`closed_loop_trace.csv` all byte-identical; 0 LLT fallbacks. The addendum's own A/B was
uninformative anyway: `_t7_joint_lowfloor_12h.json` and `_t7c_jointwhiten_12h.json`
differ ONLY by `trace_section_timers` — both have joint ON — so its "byte-identical"
compared two identical-math runs. The true toggle A/B (this result) had never been run.

**What the flat σ actually is.** `dd_sigma_cyc` = `√(gᵀ·jointConditionalCovariance·g)/λ`
(ApplicationNetwork.cpp:7755–7769) = the CURRENT-EPOCH conditional DD-ambiguity σ, which
equals `var_eff` (≈1–4 m² ≈ 5–11 L1 cyc). It is FLAT at ~5.6 cyc (p10 2.85 / med 5.63 /
p90 9.51 over 3.3 h; |frac| exactly uniform: mean 0.251, P(<0.1)=0.200). The carry is
"inert" not because a huge dep term deweights it, but because the **pure conditional
ambiguity variance is itself cycle-scale** — the ambiguity is not separable from the
per-station slant iono / SH-VTEC border at this field quality. That is the
iono–ambiguity conditioning problem (see `project_network_ddfrac_rootcause_shvtec`),
NOT a coupling-row marginalization defect. Neither Card 1's scalar nor Card 1b's joint
marginalization can move it. Recommend retiring/dormant-izing `carry_coupling_joint_whiten`
and directing effort at (a) whether `jointConditionalCovariance` can ever decline across
epochs through this producer's per-epoch conditional readout, and (b) the iono/SH field
health + floor regime — not at the coupling-row noise model.
