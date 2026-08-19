# CARD 1 — Coupling-row carry: border TIME UPDATE (1d) + gauge restore (1e) + JOINT whitening (1b, FALSIFIED)

> **Status: Card 1d + 1e BUILT 2026-07-10 late session. Card 1b PREMISE FALSIFIED the same day and RETIRE-recommended.** Two knobs, both `network.*`, both default OFF and byte-identical OFF, both requiring `carry_conditional_coupling` (hard error otherwise): **`carry_coupling_time_update`** (Card 1d — the fix) and **`carry_coupling_joint_whiten`** (Card 1b — the falsified mechanism; keep dormant). Card 1b was absorbed here 2026-08-15 from `docs/HANDOFF_carry_rigor_card1b.md`, now in `docs/attic/`; the file was renamed `HANDOFF_carry_timeupdate_card1d.md` → `SPEC_card1_coupling_carry.md` at the same time.
> ⚠ **Archive.** Every `runs/<name>` path below (`runs/t7_joint_lowfloor_12h`, `runs/t7c_jointwhiten_12h`, …) resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (CLAUDE.md, archived 2026-08-12).

## 1. Status banner — the two Card-1e structural discoveries

**BUILT 2026-07-10 late session — with two structural discoveries (Card 1e).**

1. **The Card-1 coupling rows had NEVER entered a solved field epoch.** The harvest's FREE-column restriction amputated the pinned datum columns' coefficients, breaking each row's exact gauge-invariance; the W5 null-space gate then (correctly) withheld every rows-epoch, and the 180 s withhold cadence exceeded `phase_leveling_max_gap_s = 120 s`, dropping every carry at solved epochs. That 3-cycle is what made every coupling-carry A/B byte-identical — **Cards 1b and 1d both.** Fix (Card 1e): keep pinned/gauge-zeroed columns in the coupling with `p_prev = 0`.
2. **The v1 per-column κ shrink itself re-breaks the gauge** (clock κ ≪ 1 vs pb/cb κ ≈ 1), so the shrink is applied only in the gauge-orthogonal complement: each (constellation × kind) coefficient sum is restored to its unshrunk value (**tuGaugeRestore** — the projection of the shrink delta onto the group direction restricted to the row's support; the variance inflation is kept in full, conservative).

**Measured null directions** (`network.trace_estimability`, now the first 6 gate evaluations): pure per-constellation uniform CLK shifts; a Galileo ambiguity's row has **Σ_E(shared) = −1 exactly** (the E gauge moves E ambiguities +1).

**Validation — 50-sta GPS+Gal 30-min smoke:** legacy carry **30/30** emitted; Card 1 pre-1e **13/30**; Card 1+1e (OFF arm) **30/30**; +1d with gauge restore (ON arm) **30/30 with 3.15M κ-shrunk terms in solved epochs.** Run-end "coupling-carry volume" counters + one-shot row/solve-checksum dumps were added so an inert carry can never again masquerade as a healthy A/B.

*Fable-specced 2026-07-10 evening from the Card-1c verification (its handoff, `HANDOFF_carry_recursion_card1c.md` §VERIFICATION, was pruned in commit `a8f044f` and is recoverable at `a8f044f^`; also memory `project_network_ddfrac_rootcause_shvtec` top entry). **Card-1c branch verdict: (b) structural defect in the harvest→inject loop** — not a q_cond accumulation stall (q_cond contracts 1/k textbook-style), not row gating, and not carry-reset churn: the injected coupling rows are themselves inconsistent.*

## 2. Card 1d — the defect (verified 4 ways, file:line)

The Card-1 coupling row injected at epoch k+1

```
r + M_rᵀ·s = (r̂_prev − x_lin) + M_rᵀ·ŝ_prev      weight 1/(var_eff + dep_var)
```

is an EXACT pseudo-observation of the combination `u = r + M_rᵀ·s` **at the harvest epoch** (`Var(u|D_k) = cond_var` — u is independent of s by construction of the regression). But between epochs the border moves: `s_{k+1} = s_k + w`, `w ~ N(0, Q)` (the SAME Q the D2/C1 fold applies at ApplicationNetwork.cpp:5646-5654 / 5703-5713), so

```
u_{k+1} = u_k + M_rᵀ·w      ⇒      Var must inflate by  M_rᵀ·Q·M_r
```

and exact re-independence from the border prior requires the shrinkage `M' = (P+Q)⁻¹·P·M_r`. **Production applies NEITHER to layout-present columns** — `buildCplRow` (src/core/app/ApplicationNetwork.cpp:4413-4431 scalar, 4375-4411 joint) uses the raw harvested `cs.coeff` at full strength; the `dep_var = Σ M²·p_prev` marginalization (4420-4422) fires ONLY for columns absent from the layout (`resolveSharedCol == -1`). A present-but-moving column is the un-handled middle case; full marginalization is its q→∞ limit.

## 3. Magnitude in the T7 lineage, and why the symmetric toy hid it

T7 has `network.clock_model` OFF ⇒ clocks in the C1 border with `sat_clock_q = 1e-2 m²/s`, `dt_eff = 60 s` ⇒ `q·dt = 0.6 m²/epoch`; `M_clk ≈ −2` per the trace. Each row is therefore wrong by `M²·q·dt ≈ 1.2 m²` (σ ≈ 1.1 m ≈ **5.8 L1 cyc**) while its claimed σ² is floored at `phase_leveling_var_floor_m2 = 4e-5 m²` — a **~1.5e4× weight overstatement, re-injected EVERY epoch, with weight growing ~k** as q_cond contracts (positive feedback).

**Field signature (T7 12 h forensics):** per-arc DD steps flat-with-age at **5.5–7.8 cyc/epoch**; values trending (median **106-cyc drift per arc ≈ 20 formal σ**); formal σ flat ~5 cyc; step p50 growing **2.3 → 11 cyc over 12 h**; blow-up onset bracketed by the two C1 non-PD fold warnings.

**Why Opus's symmetric toy hid it:** with identical stations the two stations' `M_r` are identical and the between-station DD cancels the poison EXACTLY; any asymmetry (station weights, geometry, per-station SH mapping) leaks it in proportion. Opus's own correlated-iono probe (map_r 1.0 vs 1.4) leaked ±5 m into the DD at 30–50× overconfidence — that IS this defect through the SH column (q_sh large), not "σ-incest physics". The field's 50 heterogeneous stations are maximally asymmetric.

## 4. Batch-exact validation, and the known bounded conservatism

**Fix validated batch-exact** (NumPy, scratchpad `card1c_replicate.py` / `card1c_falsify.py`): the matrix form makes the 8-epoch recursion equal the exact batch to 4 decimals (jsig 0.1500 = batch 0.1500; val_err equal; perfect 1/√k through 50 epochs). The scalar per-column form is consistent (never overconfident, max |err|/σ 2.2) but conservative while border cross-covariances are strong (σ lags batch 2.75× @8ep → 1.64× @50ep).

**Falsifier experiments** (`card1c_falsify.py`): **E3** asymmetric-station DD err/σ **250× (production) → ≤1.6 (fix)**; **E4** = Opus's correlated-iono probe, **32.5× overconfident (production**, reproducing the "σ-incest" signature) **→ ≤1.23 (fix)**, honest DD σ contracting with NO meter-scale floor — **the probe's ±5 m was this defect, not iono physics.**

**Known bounded conservatism (both forms):** the time update creates a cross-STATION conditional correlation `Cov(x_r, x_r' | s⁺) = M_rᵀ(P⁻¹+Q⁻¹)⁻¹M_r'` that independent per-station rows drop — fix σ sits above batch (e.g. 0.148 vs 0.107 @8ep in E4) but never below truth (**max |err|/σ ≤ 1.8 across all fix arms/horizons**). Recovering minimal variance would need joint cross-station carry rows (v3, only if needed).

## 5. Build

### v1 — scalar per-column κ (the shipped form)

Toggle `network.carry_coupling_time_update` (default OFF, byte-identical; requires `carry_conditional_coupling`; GnssConfigLoad pattern + mutex/log per repo convention). In `buildCplRow` (both scalar and joint paths), for each PRESENT CoupledShared `cs` with gap-scaled process noise `qj = q_kind(cs) · gap`:

```
κ_j   = cs.p_prev / (cs.p_prev + qj)                    // 1 when qj = 0
coeff = κ_j · cs.coeff                                  // into A_s AND the RHS term
σ²   += cs.coeff² · cs.p_prev · qj / (cs.p_prev + qj)   // = M²·κ_j·qj
```

- `q_kind` ladder = EXACTLY the D2/C1 fold's (5646-5648): Clock→q_clock, CodeBias→q_cb, PhaseBias→q_pb — plus VtecSh→`q_per_degree/n²` (the SH GM carry's per-degree q, 5252-5255; `cs.n` is stashed). Same `getProcessNoise()` constants already in scope at the assembly site.
- `gap` = the existing `pl_now_s − last_seen_s` already computed at the inject call sites (4468 amb, ZWD analogue).
- Clock under `clock_model=ON`: the column is carried by the two-state model, not D2/C1 — v1 treats it with the same q_clock (conservative; refine later). A clock-monitor reinit-flagged sat (the D2 `inflate_q` path): q=∞ ⇒ κ=0, σ² += M²·p_prev (mirror the dense fold's decorrelation rule).
- Departed columns unchanged (already the κ=0 limit). Joint path (Card 1b, dormant): shrink present columns identically before whitening; M_w/white_pw unchanged.
- Floors: pl_var_floor applies to var_eff (the conditional part) as today; the border inflation adds AFTER the floor.
- No stash/snapshot/wire change: κ needs only `cs.p_prev` (already stashed, 8117-8119) + q ladder + gap at inject time.

### v2 — matrix-exact (optional, after v1 field validation)

`M' = (P+Q)⁻¹·P·M` and joint row covariance `R = diag(cond_var_i) + M_i,shrunkᵀ(P⁻¹+Q⁻¹)⁻¹M_j,shrunk` over each station's rows — **this is EXACTLY the Card-1b whitening structure** with `P_w → (P⁻¹+Q⁻¹)⁻¹` and applied to present columns, i.e. **Card 1b's real mechanism (un-retire its plumbing)**. P source: `prev_carry_cov.P` + `sharedCarryKey` row map (serial scope; read-only in parallel assembly). Factor (P+Q) once per epoch. Motivation: scalar conservatism costs ~2.7× in accumulation time toward AR-grade σ.

## 6. Tests and the field falsifier

1. Gate test `ConditionalCarryTimeUpdateRecursiveEqualsBatch`: the Card-1c trace fixture **with the card-spec LARGE-q clock border (q=1e4)** — recursion(fixed) == batch. NOTE: the existing `ConditionalCarryRecursiveEqualsBatch` passes only because its border is quasi-static between epochs — **that is the test gap that shipped this.** The new fixture must keep the large q.
2. q=0 regression: all q_kind = 0 ⇒ κ=1, zero inflation ⇒ rows numerically identical to the unfixed path (guards the OFF-adjacent behavior).
3. q→∞ equivalence: scalar path with huge q reproduces the departed-column dep_var branch exactly.
4. Extend `ConditionalCarryContractionTrace` with a fixed arm: val_err/σ ≤ 3 all epochs, DD σ AND single σ contract, asymmetric-station variant clean.
5. Suite green; byte-identity OFF (SHA vs frozen baseline).

**Falsifier (field, the only grading that counts).** T7 rerun (same config + `carry_coupling_time_update: true`), 12 h: per-arc DD steps collapse from 5.5–7.8 cyc/epoch toward √2·honest-σ and DECAY with arc age; formal `dd_sigma_cyc` declines hour-over-hour (no 5-cyc plateau); no run-level step growth; C1 dense folds stay PD. THEN the T6 low-floor 12 h question is finally answerable: if σ → AR-grade and fractions cluster ⇒ producer floats healthy ⇒ Card 3 (Eq-34 datum) + armed AR. If σ converges honestly but fractions stay uniform ⇒ next suspect (code-bias double-datum) per the master trail.

## 7. Known-open and sequencing

Known-open (adversarial review wf_879ed034, deferred):

- **Pivot-handover response for coupling rows**: on a clock-datum handover the D2 fold skips the retired-datum keys, but the coupling rows' ŝ_prev terms still reference the OLD frame's solved values (a pre-existing Card-1 gap, not 1d-specific; the rows stay gauge-INVARIANT structurally, but their VALUE frame is stale for one epoch). Sticky-pivot makes this rare; the clean fix is to drop/re-seed the coupling stash on `datum_changed` (mirroring the D2 skip).
- The unit gate tests validate the κ math on a **gauge-free fixture**; production additionally applies `tuGaugeRestore`, which in a gauge-free system would undo the shrink for single-column groups (correct only when the group spans a real gauge, as in the field). The field smoke (30/30 emitted, A/B differing) is the acceptance test for the restore itself.

Sequencing: Card 1d → floors can drop honestly → 12 h fraction readout → Card 3 Eq-34 datum (per-(receiver,signal) phase-bias states + single-receiver datum / de Jonge chain pivoting) + self-AR harness. Card 3's spec is independent of this fix and can be drafted in parallel, but its field grading is meaningless until the carry is honest.

---

## 8. Card 1b — coupling-row JOINT whitening: the mechanism and its pre-registered gate

*(Absorbed from `docs/HANDOFF_carry_rigor_card1b.md`, Fable-specced 2026-07-10 after grading `runs/t7_joint_lowfloor_12h`. Card 1 as built is CORRECT per its spec and its unit gate — the spec itself under-modelled one term. Do not refactor Card 1.)*

**T7 evidence baseline** (`runs/t7_joint_lowfloor_12h` — Cards 1+2, low floors, 12 h): **NO divergence (a00 ±35 TECU — Card 1's objective achieved)** and **σ-calibration ~1 (honest covariance)** — but **formal DD σ is FLAT at ~5–6 cyc for 12 h** (per-epoch code level; hour-by-hour medians 5.8 → 4.8). Card 1b's proposed mechanism: Card 1 marginalizes each row's WHITE shared columns into a huge SCALAR `dep_var`, deweighting every row to nothing — yet those terms are COMMON across a station's rows and cancel in any DD, so diagonal marginalization destroys the between-row correlation that carries the DD information. The carry goes inert while staying honest: the observed flat-σ + calib ≈ 1 signature.

**The fix as specced** (exact; mirrors `appendShIonoRows`). Per station, per epoch, build the coupling rows AS A GROUP:

1. Partition each row's shared terms: PRESENT stashed columns stay as `A_s` coefficients (unchanged from Card 1); **WHITE/DEPARTED columns** (no stash entry, or a white per-epoch state such as sat clocks when `network.clock_model` carries them outside D2 — those columns are NOT in the D2/dense stash and MUST be classified white, `P_w` = the clock's a-priori variance, not "present"; that classification is the entire point of the card) go into a marginalization matrix `M_w` (n_rows × n_white) with per-column prior variance `P_w,jj`.
2. Row noise becomes the JOINT covariance **`R = diag(var_eff_r) + M_w · diag(P_w) · M_wᵀ`** (n_rows × n_rows, SPD).
3. **LLT-whiten rows, `A_s`, `A_r` and RHS by the SAME factor** — copy the per-constellation whitening in `appendShIonoRows` verbatim (`llt.matrixL().solve`). The huge-variance common directions then act as common-mode-free: information ORTHOGONAL to them (the DD content) is preserved instead of destroyed.
4. **Group scope: all coupling rows of ONE station in one whitening block** (they only correlate within a station). Cost: one LLT of ~n_carried (≈30–40) per station — trivial.
5. **Numerical guard:** if the LLT fails (R not SPD from rounding), add `1e-12·max(diag)` to the diagonal once and retry; if it still fails, **fall back to Card-1 per-row scalars for that station THIS epoch and count it** (log a WARN with the count per run).

**The pre-registered premise gate.** Instrument the Card-1 row construction on the T7 config's first 10 epochs and log, for a sample of rows, `var_eff` vs the `dep_var`/white-term contribution. **CONFIRM the dep_var/white contribution dominates `var_eff` by ≥ 10× — that is this card's premise. If it does NOT dominate, STOP and report: the flat σ then has a different cause and this card is the wrong fix.**

Out of scope (as written): Card 2 code, refloat/RAIM gating (arm T2d separately only if the σ trend shows arc-lifetime limiting AFTER this fix), floors, datum work, kernel reduce/folds.

### 8.1 ⚠ SUPERSEDED — the 2026-07-10 late ADDENDUM ("ARMED BUT INERT + 10× SLOWDOWN")

**This addendum's perf claim is FALSIFIED by §9 below, 25 lines later in the original document. It is preserved because the misread is itself a recorded lesson: a reader who stops at the heading carries away a wrong perf number and a phantom hot loop.**

It reported, from `runs/t7c_jointwhiten_12h` (toggle armed) vs `runs/t7_joint_lowfloor_12h`: output BYTE-IDENTICAL through the first 3,926 trace rows (sha-confirmed) while running "~10× slower (**11 s/epoch vs 1.1**; D0-bracketed sections unchanged at ~0.5 s — the extra ~10 s is in UNBRACKETED new code)", and concluded the whitening computes and is then discarded, or `M_w` is empty so R degenerates to Card-1 diagonals while an unrelated hot loop burns the time. **The "11 s/epoch" was the TOTAL epoch-loop wall (11,328.8 ms over 26 epochs) misread as per-epoch (÷26 = 436 ms).** Its four-step debug order (run the skipped premise check; verify the whitened blocks are the ones appended; find and D0-bracket the hot loop; cheap 200-epoch A/B before any long run) was executed in full — see §9.

## 9. Card 1b — RESULT (2026-07-10, all four addendum steps executed): PREMISE FALSIFIED; toggle is a NO-OP; no hot loop

Instrumented the Card-1b row construction (census of `M_w` columns + `var_eff` vs the marginalized dep term, per station, first 40 epochs) and ran a clean, matched scalar-vs-joint A/B (`config/test_suite/_t7_ab_{scalar,joint}.json`, 200 epochs / 3.3 h, the ONLY differing key = `carry_coupling_joint_whiten`). Instrumentation was log-only (byte-identity preserved) and has been reverted; the exe was rebuilt clean.

**Step 1 — premise check (DECISIVE, falsifies the card).**

- **`M_w` is EMPTY in 723/735 station-epochs (98.4 %).** When non-empty (**12/735**) it holds **1–2 columns — and every one is kind CodeBias, NEVER a satellite clock** (T7 has no `clock_model`, so no sat-clock state ever departs; the addendum's clock-column reclassification has no referent in this config).
- **The marginalized term is 10–1000× SMALLER than `var_eff`** (`dep` ≈ 1e-3…0.16 m² vs `var_eff` ≈ 0.6…4.3 m², and ≈5e4–9e4 m² on new arcs). The card REQUIRED `dep ≥ 10× var_eff`; the measurement is the exact opposite. **Per the card's own gate (§8): STOP — the flat σ has a different cause and this card is the wrong fix.**

**Step 2 — whitened blocks: appended correctly, NOT discarded.** The append (4551–4557) and refloat re-whiten (6113–6117) write the SOLVED `L⁻¹` blocks (`Hr/Hs/z`) with unit weight; the gate test `JointWhitenCouplingMarginalizationEqualsBatch` proves the math when `M_w` is non-diagonal. The addendum's "compute-and-discard" hypothesis is wrong: it is compute-and-apply, but **the applied result EQUALS the scalar path because `M_w` is diagonal in ~all cases** (empty, or each departed column touches one row).

**Step 3 — the "~10 s/epoch hot loop" is a PHANTOM.** Measured wall: scalar **434.7 ms/epoch**, joint **439.2 ms/epoch** (200-epoch matched A/B); premise run **435.7 ms/epoch**; the finished 12 h T7 baseline **478.8 ms/epoch**. No 10×, no unbracketed hot loop. D0 breakdown normal: assembly 60 %, reduce 19 %, other 17 % (pre-existing QC/emit/tracers).

## 10. The accidental `trace_section_timers` byte-identity A/B (Card 1b step 4)

**Step 4 — falsifier: byte-identical, i.e. the toggle changes NOTHING.** scalar vs joint `network_dd_fractional.csv` are byte-identical across **ALL 10 614 rows** (not just 3 926); `network_solution.csv`, `network_products*.txt`, `network_vtec_sh.csv`, `closed_loop_trace.csv` all byte-identical; **0 LLT fallbacks**. The addendum's own A/B was uninformative anyway: `_t7_joint_lowfloor_12h.json` and `_t7c_jointwhiten_12h.json` differ ONLY by `trace_section_timers` — both have joint ON — so its "byte-identical" compared two identical-math runs. The true toggle A/B (this result) had never been run.

> ⚠ **STABLE HEADING — do not retitle §10.** `docs/CONFIG_promotion_ledger.md` and its generated twin `docs/CONFIG_promotion_ledger.json` cite this paragraph as the MEASURED byte-identity evidence for the `network.trace_section_timers` promotion row. They previously cited it by LINE RANGE (`HANDOFF_carry_rigor_card1b.md:129-131`), which breaks on any edit; re-point them at this section anchor instead.

## 11. What the flat σ actually is — and the verdict on `carry_coupling_joint_whiten`

`dd_sigma_cyc` = `√(gᵀ·jointConditionalCovariance·g)/λ` (ApplicationNetwork.cpp:7755–7769) = **the CURRENT-EPOCH conditional DD-ambiguity σ, which equals `var_eff`** (≈1–4 m² ≈ 5–11 L1 cyc). It is FLAT at ~5.6 cyc (p10 2.85 / med 5.63 / p90 9.51 over 3.3 h); |frac| exactly uniform (mean 0.251, P(<0.1) = 0.200). The carry is "inert" **not** because a huge dep term deweights it, but because the **pure conditional ambiguity variance is itself cycle-scale** — the ambiguity is not separable from the per-station slant iono / SH-VTEC border at this field quality.

**That is the iono–ambiguity conditioning problem** (memory `project_network_ddfrac_rootcause_shvtec`), **NOT a coupling-row marginalization defect**, and **neither Card 1's scalar nor Card 1b's joint marginalization can move it.**

**Recommendation (Card 1b): retire / dormant-ize `carry_coupling_joint_whiten`** and redirect effort at (a) whether `jointConditionalCovariance` can ever decline across epochs through this producer's per-epoch conditional readout, and (b) the iono/SH field health + floor regime — not at the coupling-row noise model.

## 12. Artifacts and configs

- **Card 1d / 1e:** knob `network.carry_coupling_time_update` (default OFF, requires `carry_conditional_coupling`); gates in `tests/test_network_estimator.cpp` (`ConditionalCarryTimeUpdateRecursiveEqualsBatch`, `ConditionalCarryContractionTrace`); NumPy oracles `card1c_replicate.py` / `card1c_falsify.py`.
- **Card 1b:** knob `network.carry_coupling_joint_whiten` (default OFF, RETIRE-recommended); gate `JointWhitenCouplingMarginalizationEqualsBatch`; configs `_t7_joint_lowfloor_12h.json`, `_t7c_jointwhiten_12h.json`, `_t7_ab_{scalar,joint}.json` (only differing key = `carry_coupling_joint_whiten`); run `runs/t7c_jointwhiten_12h`.
- **OFF byte-identity anchor:** `_t4_gg_treated_smoke.json` SHA == **`69C20D38E22B5EBE`** on a **FRESHLY REBUILT x64-Release** — verify exe mtime > source mtime; *"stale-exe validation has burned us three times."* Suite bar at the time: green ≥ 602/1.

## 13. Card 1f / 1g — the TRUNCATED-ROW gauge defect, and the reinit-source widening

**Landed 2026-08-15, both default-OFF.** `network.carry_coupling_gauge_project` (1f) and
`network.carry_coupling_reinit_sources` (1g).  Origin: the carry↔datum handover theory
campaign (memory `project_carry_datum_handover_theory`), fix-program items A and B.

### 13.1 The defect (1f)

A harvested coupling row is gauge-invariant by the regression identity
`Mᵀg_s = −g_r(c)`: it observes each datum gauge **exactly as much as its own `A_r = +1`
local requires**, and no more.  §1's measured null directions record the concrete value —
a Galileo ambiguity's row has **`Σ_E(shared) = −1` exactly**, because the E gauge moves E
ambiguities by +1.

TRUNCATION breaks the identity.  A DEPARTED column is marginalized by **deleting** its
coefficient (`NetworkCoupling.cpp`, the `c < 0` branches), which leaves
`Σ_family M = −M_dep` — so the row becomes a high-weight observation of the **current**
gauge pinned at the **old** realization.

**Measured (simnet24, TOW 305220, seven G-clock departures in one epoch):** smallest
null-space eigenvalue **1e-11 → 9.43e-3**, five decades over the rank-gate cutoff and
level with the weakest genuine phase-bias layer ⇒ **4 spurious OVER-specified withholds**,
self-healing only at 120 s = `pl_max_gap_s` by blanket carry expiry (a silent mass
re-cold-start).

### 13.2 Why "annihilate the family sum" is the WRONG fix

The obvious repair — project the coefficient vector onto the complement of the null basis
so the family sum is zero — is wrong for exactly the rows the carry consists of.  The
target is `−g_r(c)`, not `0`; annihilating it would **inject** a gauge observation on
every row whose local carries gauge weight, which §1 measured to be the normal case for
an ambiguity row.

The correct target is the **harvest** sum, and it needs no null basis at all: at build
time `lc.coupling` still holds every harvested entry, present and departed, so
`Σ_all coeff` **is** the harvest sum (up to the 1e-8 relative sparsify threshold).  This
also means **no measured eigenvector basis `V` is required** — which matters, because the
rank gate is `EigenvaluesOnly` and no eigenvector solve of the border runs on a default
config, a stashed `V` would be one epoch stale, and on a truncation epoch the previous
epoch's near-null set is precisely the one **missing** the direction that got filled.

### 13.3 What 1f actually does

It redefines Card 1d's deficit.  `tuGaugeRestore` restored
`Σ_present(m_un − m_shr)` — the κ shrink alone, blind to deletion.  1f seeds the same
per-(constellation,kind) deficit map with the truncated columns' coefficients, so the
restored quantity becomes the **harvest** sum.  Consequences:

- it fixes departures and the κ shrink in **one** expression;
- it reduces **exactly** to Card 1d when nothing was truncated (so the OFF path and the
  no-departure ON path are byte-identical);
- the y-correction is automatic — the post-restore emission loop already accumulates
  `rhs += m_shr · s_prev` with the corrected coefficient;
- it is frame-agnostic: restoring each family sum to its harvested value requires no
  knowledge of *which* combination is the true null direction.

With κ OFF the present terms are routed through the same buffer (unshrunk, `m_shr = m_un`)
so the restore can run; iteration order is preserved and departed terms never touch `rhs`,
so that re-routing is bit-identical.

**A family with truncated mass but NO present recipient** has nowhere to put the deficit.
Those rows stay gauge-dirty and are counted (`cpl_gauge_orphan`) rather than
half-corrected — invariant #4: the miss is telemetry, not a silent partial fix.

### 13.4 The 1g half

`tuReinitSince` was gated on `carry_cpl_tu`, and `tu_reinit_hist` was fed **only** by
clock-QC sources (`prev_carry.inflate_q`, clock refloats).  **No realization-step source
reached it.**  A phase-anchor handover, an eq34 chain re-reference or a datum-slip PB
re-realization each erase the re-referenced family's *own* carry, but every *other*
carried local keeps a `coupling[]` entry on that column holding a **retired-frame
`s_prev`**, folded at full weight — the stale-`ŝ_prev` wire-poison channel (30 PBGAUGE +
27 CLK_STEP re-realizations in 120 epochs).

1g keys the history `(kind, sat, signal)` — an empty signal being the **sat-wide
wildcard**, which is what the clock-QC feeds write and what preserves their existing
kind-wide matching — feeds it from `phase_anchor_reref` / `eq34_reref` /
`datum_reref_pb_sats`, and honours it independently of κ.  Re-realized columns then take
the departed route (κ=0), which is the only correct treatment for an **unknown** step;
1f then repairs the deficit that marginalization opens.  **The two are designed to be
armed together** — arming 1g alone trades a stale frame step for a gauge observation, and
the loader warns.

### 13.5 First armed run — WIRING PROVEN, EFFECT UNGRADED

`config/test_suite/_card1f_wiring_probe.json` (= `_dcmp_50sta_10ep_cpllive.json` + the three
knobs; 50 sta, 11 epochs, x64-Release, 2026-08-15).  This is a **wiring probe, not a
validation** — 11 epochs cannot grade any estimation channel.

| arm | departed terms | 1f held / orphan | C-GF downdated / declined | inv-#7 R max |
|---|---|---|---|---|
| baseline (all three OFF) | **31** | — | — | 2.191e-06 |
| 1g only (departures, NO repair) | **356** | — | — | 2.191e-06 |
| 1f + 1g + C-GF | **415** | **310 / 0** | **310 / 10** | 2.554e-06 |

What this establishes:

- **All three cards FIRE** (invariant #4 satisfied): 1f held 310 family sums with **zero**
  orphans; C-GF downdated 310 keys with **zero** inert epochs and **declined 10** directions
  for partial support, so the closure test is doing real refusals rather than being
  decorative.
- **1g does what it claims, and the attribution closes exactly**: departures 31 → 356 on the
  knob alone, and the card's dedicated counter reports **384 terms diverted by the widened
  (kind,sat,signal) probe** on the all-armed arm — against an observed departed-term increase
  of 415 − 31 = **384**.  The two are independent measurements of the same quantity and they
  agree to the term, so the mechanism is precisely attributed rather than merely correlated.
  Those 384 terms were previously folded at FULL WEIGHT in a retired frame.
- **The invariant-#7 counter is calibrated**: R ∈ [0, 2.6e-06] on a healthy config, i.e. the
  declared gauge carries ~six decades less information than the gate's own cutoff.  A run
  whose datum genuinely owns its gauge now says so positively.

⚠ At this scale the 1g-only arm drove 356 UNREPAIRED departures and the ratio did **not** move
(2.191e-06, identical to baseline).  That was NOT evidence the counter cannot fire — see §13.7,
where it fires at 7.1e+06 in the regime the defect was actually measured in.  **The window, not
the scale, was the problem**: 11 epochs from a cold start is far too short for the carry to age
into the failure.  §13.8 shows the same 50-station lineage breaching at 1.557e+03 once it runs a
full hour, and 305 stations breaching at 7.537e+03.  ❌ A hypothesis that dense rosters make
departures rare (raised from this same 11-epoch probe, which measured ZERO departed columns at
305 sta) is **FALSIFIED**: over 1 h departures scale UP with station count — 14 989 @50,
111 599 @150, 284 618 @305.  Never characterise this defect from a short cold-start window.

Arms: `_card1f_wiring_probe.json` (all three) and `_card1f_negctl.json` (1g only, no repair),
both derived from `_dcmp_50sta_10ep_cpllive.json` by adding knobs and nothing else.

### 13.6 Defects found by the post-implementation review (all fixed)

Two adversarial passes (39 candidate findings → 11 surviving independent refutation).  Recorded
because each is a *class* that will recur in the next card of this family.

0. **MAJOR — a fix that added a parameter and never read it.**  The withdraw primitive
   `refoldSharedPriorClockGaugeFreeDropping` gained `require_closure` in its signature — and
   the callers passed it in good faith — while the body ignored it, so the withdraw rebuilt
   and subtracted the GAUGE-FREE block for a group the fold had (correctly) folded PLAINLY.
   Consequences were PSD-preserving but real: a partial refloat that retains ~1/n of the
   refloated satellite's prior, survivors re-anchored against a mean they should not see,
   and — on a lone-clock group — a `cap_priors` desync in which the caller erased a prior as
   "withdrawn" while it was still folded.  *Class: threading a flag through a signature is
   not threading it through the behaviour; grep the body, not the call sites.*

1. **CRITICAL — the closure test dropped the carry instead of declining the downdate.**  On
   the DIAGONAL gauge-free path the loop body is what ACCUMULATES the priors, so `continue`
   on a closure failure folded **no prior at all** for the whole group, silently discarding
   carried clock information.  The dense path has no such hazard (Λ already holds those rows;
   closure only removes a column of `U`).  Fixed by folding the group PLAINLY on decline.
   *Class: when two paths "mirror" each other, verify what the loop body OWNS, not just what
   it decides.*
2. **CRITICAL — `require_closure` reached the FOLD sites but none of the WITHDRAW/REPLAY
   sites.**  Closure selects WHICH directions `U` spans, so it is part of "what was folded"
   exactly as `clock_gauge_free` is — and the header already said that flag "MUST match the
   flag the original fold used".  Threading one and not the other reintroduces the
   negative-semidefinite residue this card exists to prevent.  Fixed at all four
   withdraw/restore sites plus both replay folds.
3. **MAJOR — the breach set latched across un-evaluated epochs.**  A direction whose
   constellation dropped out of the layout kept its breached flag forever, so its next breach
   fired no edge.  That is invariant #7's own failure mode (a returning member read as a
   continuing one).  Fixed by emitting a `ddi_clear` edge for any latched direction the epoch
   did not evaluate.
4. **MAJOR — the run-end advice named a fold the counter cannot see.**  The ratio is measured
   on the reduced normals AS THE GATE SEES THEM: after the station blocks (which carry the
   coupling rows) and the pre-gate datum rows, but **BEFORE** the D2/C1 dense carry fold and
   the clock-osc prior.  The advice listed the D2/C1 fold as a candidate it could indict.
   Corrected to state the observable window explicitly.
5. **MINOR** — the partial-skip tally was epoch-scoped and read at the last fold site, so it
   under-counted the withdraw and replay folds that run later in the epoch; it is now
   run-scoped on the estimator and read once at teardown.  Plus three missed
   `cta_gauge_free` → `carry_gauge_free` replacements and one dangling doc reference.

⚠ **ONE DELIBERATE BYTE-IDENTITY EXCEPTION, and it matters for A/B methodology.**  Everything
above is byte-identical with the new knobs off — *except* the within-epoch RAIM replay, whose
BODY changed unconditionally.  It previously re-folded the captured priors with the plain
accumulator even when the primary solve had folded them gauge-free, so on a config running
`clock_anchor_gauge_free` **+** `raim_within_epoch` the replayed system was not the system
being tested and the RAIM trip decision was taken against normals containing information the
primary solve had removed.  That was a defect, and fixing it necessarily moves those configs.
**No fingerprint token discriminates the old from the new behaviour**, so an A/B that spans
this commit on such a lineage is not a single-variable comparison — re-baseline it.  Configs
that arm neither knob, and configs that arm the anchor without the within-epoch tier, are
unaffected.
### 13.7 POSITIVE CONTROL — simnet24 at TOW 305220 (2026-08-15).  **PASS.**

The pre-registered test of §13.5, executed.  Single-variable A/B over
`_simnet24_inj_pareref_on` with the Card-1 coupling made LIVE
(`carry_conditional_coupling` + `shared_carry_full_cov` + `carry_coupling_time_update`,
`carry_invariant: false`); the two arms differ **only** in `carry_coupling_gauge_project` and
`carry_coupling_reinit_sources`.  Configs `config/test_suite/_s24cpl_{off,on}.json`, runs
`runs/s24cpl_{off,on}`, x64-Release.

**The OFF arm reproduces the documented defect exactly.**  Four OVER-specified gate withholds,
at TOW **305220 / 305250 / 305280 / 305310** — the first is the epoch named in the theory
campaign, and the four consecutive 30 s epochs span exactly the **120 s = `pl_max_gap_s`**
blanket-carry-expiry self-heal the card predicted.  This is the first time that defect has been
reproduced on demand rather than observed in passing.

| | OFF (defect) | ON (1f+1g) |
|---|---|---|
| epochs emitted | 116 / 120 | **120 / 120** |
| OVER-specified withholds | **4** | **0** |
| invariant-#7 R max | **7.123e+06** (CLK\|G, TOW 305310) | **2.086e-03** |
| breach epochs / edges | 4 / 2 | **0 / 0** |
| observed near-null min..max | 0 .. 1 | **1 .. 1** (= declared, every epoch) |
| `ddi_over_breach` | **4 of 4** | 0 |
| 1f held / orphan | — | **560 / 0** |
| 1g diverted | — | **507** |

R falls **9.5 decades**, from far above the gate cutoff to far below it.  Every one of the four
withholds coincided with a breach, so the new run-end advice correctly refused to say
"re-calibrate" and instead named `network.carry_coupling_gauge_project` — the fix that in fact
resolves it.  The instrument, the diagnosis and the remedy agree.

**Quality, graded against injected truth on MATCHED epochs.**  The ON arm's four extra epochs
were removed so the populations are identical (n = 5502 code-bias residuals, 1776 phase):

| channel | OFF | ON (matched) |
|---|---|---|
| code-bias residual rms | 0.4306 m | 0.4347 m (+0.9 %) |
| code-bias residual mean | −8.5e-04 m | **−4.4e-04 m** |
| sum-to-zero datum check | mean −8.53e-04, rms 4.60e-03 | **mean −4.36e-04, rms 2.91e-03** |
| phase SD fractional rms | 0.269–0.293 cyc | 0.282–0.288 cyc |

Read honestly: code-bias quality is **FLAT** (+0.9 % on a channel whose rms is dominated by a
heavy-tailed C5Q at 0.75 m), the **datum realization is 1.6× tighter**, and the phase channel
sits at 0.269–0.293 cyc in BOTH arms against a uniform-random rms of 0.2887 — i.e. the
fractional phase bias is unresolved either way and that column carries no signal for this A/B.
The four recovered epochs are genuinely harder than average (their inclusion moves the
unmatched rms 0.4347 → 0.4412) but are usable, not garbage.

**Verdict: the defect is real, reproducible, and fixed, at no measured quality cost and with a
measurably better-realized datum.**  ⚠ Scope: **n = 1**, 24 stations, GPS-only, simulated, 1 h,
one lineage.  Not graded at 63/150/305, not on field data, not multi-constellation, not warm.
`network.mw_wl.bridge_sample_debias` was validated at 50 stations and is net-HARMFUL at 305 —
that is why this is registered **RECOMMEND_SCOPED**, not RECOMMEND_ON.

### 13.8 THE VALIDATION LADDER — 24 (sim) / 50 (field) / 150 (field).  **PASS at every point.**

Three single-variable A/Bs, defect present in every OFF arm and absent in every ON arm.

| arm | stations | data | withheld OFF → ON | inv-#7 R max OFF → ON | departed OFF → ON |
|---|---|---|---|---|---|
| `s24cpl` | 24 | sim (truth-graded) | **4 → 0** | 7.123e+06 → 2.086e-03 | 3 308 → 1 334 |
| `fld50`  | 50 | **real IGS** | **20 → 0** (16.5 %) | 1.557e+03 → 3.148e-06 | 14 989 → 38 771 |
| `s150`   | 150 | **real IGS** | **20 → 0** | 7.807e+02 → 9.254e-06 | 111 599 → 6 393 |
| `s305`   | **305** | **real IGS** | **20 → 0** | 7.537e+03 → 1.664e-05 | 284 618 → 15 131 |

(`fld50` is the one rung where departures RISE with the fix, because it is the only field rung
where Card 1g was live — 37 441 diverted terms are added departures by construction.  At 150/305
1g diverted nothing, so only the cycle-breaking drop shows.)

Two results the ladder adds beyond §13.7:

- **The field defect is ~5× more prevalent than the sim one** (16.5 % of epochs vs 3.3 %), so the
  sim control UNDER-states the field payoff rather than over-stating it.
- **The withhold→expiry→more-departures cycle is real and the fix breaks it.**  At 150 stations
  departed coupling terms fell **111 599 → 6 393 (17×)** with the knobs on.  The OFF arm's
  withholds age the carry past `pl_max_gap_s`, blanket expiry then manufactures a fresh crop of
  departed columns, and those breach the gauge again.  That is the "silent mass re-cold-start"
  the theory card named, observed closing on itself.
- ⚠ **At 150 stations Card 1g diverted ZERO terms**, so the entire 150-sta improvement is
  attributable to **1f alone**.  Cause (measured, now a loader warning + a ledger interlock):
  1g's realization-step feeds are populated ONLY by `phase_datum_eq34` or
  `phase_anchor_carry_reref`, and that lineage ran `phase_datum_prioritized` with neither.
  Diverted counts across the ladder: 507 (sim, pa_carry_reref) · 37 441 (50-sta, eq34) ·
  **0** (150-sta, neither).

**QUALITY — improved on both available instruments, not merely neutral.**

| instrument | arm | OFF | ON |
|---|---|---|---|
| sim truth grade (matched epochs) | s24cpl | code-bias rms 0.4306 m | 0.4347 m (+0.9 %, flat) |
| sim sum-to-zero datum check | s24cpl | mean −8.53e-04, rms 4.60e-03 | **−4.36e-04, 2.91e-03** (1.6× tighter) |
| FIELD datum-invariant wire step | fld50 | **5.631 cyc/ep** (1.2549 m/ep) | **4.445 cyc/ep** (0.9983 m/ep) — **−21 %** |

The field wire number is the pivoted between-satellite step from `scripts/grade_wire.py` (the
Psychas single-realization quantity).  RAW rises 7.470 → 8.365 cyc/ep, but RAW deliberately
includes the benign common-mode frame drift the pivot exists to remove — grading on it is the
error that grader was written to prevent.  The ON arm achieves the lower pivoted step *while also
emitting 20 more epochs*, i.e. while covering the harder epochs the OFF arm withheld.

⚠ **A first field wire grade was DISCARDED as invalid**, and the reason generalises: the trace
CSVs land in a log dir derived from the CONFIG path (`./logs/`), NOT next to the exe, and they are
overwritten by every run.  An initial snapshot helper pointed at `build/x64-Release/logs/` and
silently captured a **two-day-old fossil** — four different arms produced byte-identical 85 442-byte
files, which is what exposed it.  Always verify a captured trace differs between arms before
grading it.

**COST — none.**  305-station arms: 15.4 min OFF vs **14.7 min ON**.  The fix is marginally FASTER,
because holding the gauge suppresses the withhold→carry-expiry→more-departures cycle and there is
less marginalization work to do.

### 13.8.1 FIELD ARM detail — 50 real IGS stations, 1 h, DOY-129

The arm §13.7 said was owed.  Real observations (not the synthetic fixture), 50 stations,
1 h at 30 s, GPS, coupling LIVE (`carry_invariant: false` + `phase_leveling`), single-variable
A/B on the two knobs.  Configs `_fld50_{off,on}.json`, runs `runs/fld50_{off,on}`.

| | OFF | ON |
|---|---|---|
| epochs emitted | 101 / 121 | **121 / 121** |
| gate-withheld (all OVER-specified) | **20  (16.5 %)** | **0** |
| invariant-#7 R max | **1.557e+03** | **3.148e-06** |
| breach epochs / edges | 20 / 1 | **0 / 0** |
| coupling terms present / departed | 4 339 142 / 14 989 | 5 340 791 / 38 771 |
| 1f held / orphan | — | **15 852 / 0** |
| 1g diverted | — | **37 441** |

**The field defect is five times more prevalent than the sim one** — 16.5 % of epochs withheld
against 3.3 % on simnet24 — and the fix recovers all of them.  R falls **8.7 decades**.  Note
the breach is ONE sustained era (20 breached epochs, a single edge), not 20 independent events:
the field carry sits in the filled-null state for a stretch and the gate withholds throughout,
which is exactly the "silent mass re-cold-start" shape the theory card described.

⚠ **Quality on the field arm is NOT graded here.**  Field data has no truth, so the available
instrument is the datum-invariant wire step (`scripts/grade_wire.py`).  A first attempt was
DISCARDED as untrustworthy: the trace CSVs are written to a SHARED `build/x64-Release/logs/`
that the next run overwrites, and a concurrent 305-station job was writing there at the time.
Re-run both field arms back-to-back with `scripts/_arm_run.sh` (which snapshots and VERIFIES
the capture) before quoting any wire number.  Availability and the gauge ratio above are
unaffected — they come from each run's own captured `run.log`.

**Promotion (2026-08-15).**  Registered in `docs/CONFIG_promotion_ledger.json` (1f and 1g
RECOMMEND_SCOPED with the regime `on("network.carry_conditional_coupling") and not
on("network.carry_invariant")`; C-GF UNVALIDATED — it was NOT varied in this A/B) and as
loader POSTURE rows in `GnssConfigNetworkPosture.cpp`, so every in-regime run that leaves them
off now says so at startup with the evidence.  Loader↔checker parity verified
(`config_posture_check.py --verify-loader`).  Code defaults deliberately UNCHANGED: both knobs
are fingerprinted, so flipping a default invalidates every stored snapshot — a migration card,
not an edit.

⚠ One defect was found and fixed BY this run: the counter's range-error sentinel was
overloaded onto the sign of the accumulating quadratic form, and since `N`'s off-diagonals
are freely negative, an ordinary partial sum aborted the accumulation.  First armed run
reported **11 epochs, 0 direction samples** — the exact "armed and inert" signature invariant
#4 exists to surface.

### 13.9 THE 2×2 ISOLATION — 1f and 1g each carry a DIFFERENT half of the win (2026-08-17)

§13.8's open item ("1g's INDEPENDENT effect is unisolated"), closed with a full 2×2 factorial
on BOTH lineages, one build (post-§14-fix — structurally inert here, shift door unarmed on
both):  field `_fld50_{00,1f,1g,11}` (eq34, 50 real IGS sta, 1 h) and sim `_s24cpl2_{00,1f,1g,11}`
(pa_carry_reref, 24 sta, truth-graded).  Every config certified single-knob against its §13.8
parent, and the corner cells REPRODUCE the stored §13.8 arms **byte-for-byte** (products.txt
and network_solution.csv identical for 00↔off and 11↔on) — the two intervening commits are
certified inert on these lineages and the 2×2 sits exactly on the ladder baseline.

**Field (matched-epoch wire = the 101 epochs every cell emits):**

| cell | withheld | inv-#7 R max | departed | diverted (1g) | held (1f) | pivoted wire step |
|---|---|---|---|---|---|---|
| 00 | 20/121 | 1.557e+03 | 14 989 | — | — | 5.631 cyc/ep |
| 1f | **0** | healthy | 892 | — | 667 | 5.543 |
| 1g | 20/121 | **1.030e+04** | 72 019 | 54 662 | — | **1.532** |
| 11 | **0** | healthy | 38 771 | 37 441 | 15 852 | 3.935 |

**Sim truth (matched to the common 116 epochs — n identical in every cell: clock n=1776,
code-bias n=5502):**

| cell | withheld | clock CONTRAST rms (datum-free) | clock max abs | code-bias rms | datum-check rms |
|---|---|---|---|---|---|
| 00 | 4/120 | 4.157 m | 17.75 m | 0.4306 m | 4.60e-03 |
| 1f | **0** | 4.688 m | 17.99 m | 0.4495 m | **2.85e-03** |
| 1g | 4/120 | **0.435 m** | 1.01 m | 0.4195 m | 4.65e-03 |
| 11 | **0** | **0.378 m** | 0.96 m | 0.4347 m | **2.91e-03** |

**Attribution — the two cards decompose almost cleanly:**

- **1f owns availability, gauge honesty, and the datum realization.**  Withholds → 0 in both
  1f-armed cells on both lineages; the invariant-#7 ratio collapses only when 1f is armed; the
  sum-to-zero datum check tightens 1.6× only in the 1f-armed cells.  On the wire it is NEUTRAL
  (5.631 → 5.543 matched-epoch, −1.6 %) and on truth channels ~flat (code-bias +3–4 %, inside
  the C5Q heavy tail; clock contrast +13 % alone / −13 % on top of 1g).
- **1g owns the product-quality win, and the channel is the CLOCK.**  Datum-free clock contrast
  rms 4.157 → 0.435 m alone (**~10×**, max |err| 17.8 → 1.0 m), 4.688 → 0.378 m on top of 1f;
  field pivoted wire step 5.631 → 1.532 alone (−73 %), 5.543 → 3.935 on top of 1f (−29 %).
  Mechanism: a coupling term whose column was RE-REFERENCED folds a retired-frame `s_prev` at
  full weight, and the injected error lands in the emitted clock/phase products; the divert
  removes exactly that.
- **The §13.8 quality attribution is CORRECTED**: the fld50 wire −21 % was implicitly credited
  to 1f (the promoted knob); the isolation shows the wire/clock gain is **1g's**, while 1f's
  quality contribution is the datum check plus emitting 20 more epochs at no wire cost.
  (§13.8's note that the **150-sta** rung's gains are 1f-alone stands — withholds/R are 1f's
  instruments, and 1g was inert there.)
- **Over-retention ADJUDICATED (the lane-A/B suspicion pattern):** 1g-alone's very quiet wire
  under a breached gauge (R 1.0e+04) looked like the "quiet prior / illegitimate pin" shape.
  The sim truth grade refutes that: the improvement holds on the **datum-free clock contrast**,
  which a pinned frame cannot fake.  The quiet is real product accuracy, not over-retention.
- **1g alone remains contraindicated**, now with measured teeth: it recovers zero epochs, and on
  the field eq34 lineage its unheld diverted mass makes the gauge breach **6.6× hotter**
  (R 1.557e+03 → 1.030e+04) while departed terms balloon 14 989 → 72 019.  Arm it WITH 1f.
- Caveats: the clock-truth instrument exists only in sim (one lineage, pa_carry_reref); the
  field eq34 lineage has no clock truth and its 1g R-worsening is lineage-specific (sim R was
  ~unchanged, 7.12e+06 → 6.91e+06).  1g stays SCOPED.

**Scale addendum (2026-08-17): the FULL 2×2 at 150 stations on eq34** (`_s150eq34_{00,1f,1g,11}`
— the `_s150` roster with the datum swapped prioritized→eq34, every cell certified single-knob
against its §13.8 parent; same snapshot build; the 00/1g cells emit the IDENTICAL 101-epoch
set and the 1f/11 wire numbers below are MASKED to that same set, so every wire comparison is
matched-epoch; all four solution CSVs pairwise distinct):

| cell | withheld | inv-#7 R max | departed | diverted (1g) | held (1f) | pivoted wire, matched 101 ep |
|---|---|---|---|---|---|---|
| 00 | 20/121 | 6.446e+03 | 98 866 | — | — | 10.788 cyc/ep |
| 1f | **0** | healthy | 5 644 | — | 3 272 | 10.633 |
| 1g | 20/121 | **1.473e+04** | 168 346 | 65 988 | — | 9.328 |
| 11 | **0** | healthy | 67 966 | 61 445 | 33 723 | **7.592** |

What replicates, what changes vs 50 stations:

- **1f's half replicates exactly**: withholds → 0, gauge healthy, departure cycle broken
  (98 866 → 5 644), and wire-NEUTRAL alone (−1.4 % here, −1.6 % at 50).
- **1g's with-1f wire contribution is SCALE-STABLE: −29 % at both scales**
  (50 sta: 5.543 → 3.935; 150 sta: 10.633 → 7.592).  That is the number to quote for the
  pair's quality win — not the 1g-alone figure, which is scale-dependent (−73 % at 50,
  −13.5 % at 150) because the eq34 baseline itself degrades with station count
  (10.788 vs 5.631 cyc/ep, R 6.4e+03 vs 1.6e+03, erases 3 740 vs 1 599) and the
  retired-frame population 1g removes becomes a minority of the churn.
- **The 50-sta anomaly does NOT replicate**: at 50, 1g-alone's wire (1.532) was quieter than
  the pair's (3.935) — under a breached gauge.  At 150 the pair is the QUIETEST cell
  outright (7.592 < 9.328), i.e. with the gauge held honestly there is nothing left that
  the breached-gauge quiet was "adding".  This strengthens the pair doctrine and retires
  any reading that 1g-alone's extreme quiet is desirable.
- **1g alone stays contraindicated at scale**: zero epochs recovered, R 2.3× hotter
  (6.6× at 50), departed +70 %.
- The divert→hold coupling between the cards is visible in the counters: 11's held count is
  ~10× 1f-alone's (33 723 vs 3 272; 24× at 50 sta) — 1g's diverted terms create the
  truncations 1f then holds.

Also noted in passing: on the IDENTICAL roster the eq34 datum baseline breaches ~8× hotter
than the prioritized datum (R 6.446e+03 vs §13.8's 7.807e+02) and carries a ~2× noisier
pivoted wire (10.788 vs the prioritized lineage's ladder regime) — a datum comparison, not a
knob verdict.

**Multi-constellation coverage arm (2026-08-19): GPS+Galileo at 50-eq34 — PASS, and the
ORPHAN channel is nonzero for the first time.**  `_fld50ge_{00,11}` = the §13.9 corner cells
plus `Galileo` in `active_systems` / `signal_priorities` ("CBXIQ") and nothing else, same
snapshot build; pair A/B (both knobs together — the per-knob attribution is §13.9's, this arm
is coverage).  This is the FIRST arm in which the Card-1f family machinery runs with two
(constellation,kind) family sets, i.e. the first execution of the branch the §1 mechanism's
own Galileo Σ_E = −1 example describes.

| cell | withheld | inv-#7 R max | departed | diverted (1g) | held (1f) | ORPHANED | pivoted wire, matched 101 ep |
|---|---|---|---|---|---|---|---|
| ge_00 | 20/121 | 4.132e+02 (CLK\|G) | 21 366 | — | — | — | 4.815 cyc/ep |
| ge_11 | **0** | healthy | 84 673 | 82 226 | 32 066 | **62** | **3.383 (−29.7 %)** |

- **PASS on every primary gate**: withholds → 0, gauge healthy, eq34 pin audit COMPLETE for
  all 10 Galileo signal groups (declared == folded == landed, every epoch), diverted/held live
  at multi-GNSS volume, §14 counter 0 alongside 0 v2-shifts (door unarmed, posture
  comparable).
- **The pair's wire contribution is CONSTELLATION-STABLE: −29.7 % matched-epoch**, the third
  lineage in a row at −29 % (50-GPS −29.0 %, 150-GPS −28.6 %).  Quote −29 % as the pair's
  quality number across every regime tested.  The multi-GNSS baseline itself is quieter than
  GPS-only (4.815 vs 5.631) and breaches softer (4.1e+02 vs 1.6e+03) — more satellites
  constrain both the frame and the datum directions.
- ⚠ **62 ORPHANED family sums — first nonzero in any arm** (every GPS-only cell measured 0).
  Structurally a multi-constellation phenomenon: a coupling row's Galileo family can lose ALL
  its present columns while its GPS family survives, leaving truncated mass with no present
  recipient; GPS-only rows in that state are simply not built.  This is the documented
  gauge-dirty residue the card counts honestly and cannot remove — at 62 / 32 066 held
  (0.19 %) it does not trip invariant-#7 here (0 withholds, no breach), but the channel is now
  MEASURED nonzero.  OWED before any multi-GNSS promotion tightening: per-epoch/per-family
  attribution of the orphans, and a decision on whether the residue needs a recipient
  fallback (e.g. holding against the row's OTHER surviving family is NOT obviously correct —
  the sums are per-constellation by construction because the datum realizes per
  constellation).
- Still owed for full multi-constellation coverage: per-knob isolation at multi-GNSS
  (this arm is pair-vs-off), GLONASS/BeiDou regimes (each has its own known hazards —
  see project_glonass_ifb_network, project_beidou_network_clock_divergence), and the
  sim-truth clock instrument with a second constellation. — INERT-BY-CLOSURE on every roster tested (2026-08-17)

§13.8's other open item ("C-GF was not varied in any A/B"), closed with four arms on the same
build as §13.9: `_fld50_cgf{00,11}` (field eq34) and `_s24cpl2_cgf{00,11}` (sim), each the
matching §13.9 cell plus `network.shared_carry_gauge_free: true` and nothing else.

**Result: all four C-GF arms are BYTE-IDENTICAL to their parents** (products.txt and
network_solution.csv), and the mechanism is visible in C-GF's own counters: the closure test
DECLINED the (single) declared CLK|G direction essentially every epoch — field 100/101 and
120/121 emitted epochs, sim 115/116 and 119/120.  A roster's carried clock set essentially
never spans the full registered constellation (some registered satellite is always unobserved
or departed), full-support closure therefore never holds, and per §13.6-defect-1's fix the
group folds PLAINLY on decline — which byte-identity proves is a true no-op.  The earlier
50-station wiring probe already showed the same signature (10 declines / 11 epochs) and it
went unread.

Three consequences:

- **The knob is SAFE and NULL as built**: proven solve-inert in four arms across two lineages.
  It IS fingerprinted (`cta:gf` / `cta:gf+cl` via the resolved `carry_gauge_free` predicate,
  NetworkRunContext.cpp:680), so arming it splits a warm-resume lineage while changing
  NOTHING — the worst combination.  Registered **KEEP_OFF**: nothing to gain, a fingerprint
  split to lose, and a knob that reads as armed while doing nothing is posture noise.
- **The refusal is CORRECT, not a bug** (the run-end message states the theory): restricted to
  a carried subset, the constellation-sum direction is no longer the datum's functional, and
  downdating it would delete an estimable carried-vs-uncarried CONTRAST rather than gauge.
  The design gap is real though: a gauge-free treatment that engages on realistic rosters must
  re-derive the datum functional WITHIN the carried subset (datum pins + carried span), which
  is a new derivation card, not a gate tweak.
- **Counter-semantics wart (minor, invariant #4)**: "N clock prior key(s) gauge-downdated"
  counts keys processed through the gauge-free-capable path, not keys whose information
  changed — next to universal declines it reads as armed-and-working when the net effect is
  nil.  The decline count printed beside it is the honest number; read them together (the
  message's own closing sentence says exactly this).

Note the within-epoch RAIM replay gauge-flag fix that shipped WITH the C-GF card (§13.6, the
deliberate byte-identity exception) is unconditional and unaffected by this verdict — it is
live regardless of the knob.


## 14. FIXED — the eq34 D4 S-transform now re-bases `coupling[].s_prev` (filed 2026-08-15, fixed 2026-08-17)

**The defect as filed.**  `CoupledShared::s_prev` and `CarryPrior::value` are **literally
the same number** for the same identity — both are assigned `x_s(j)` at harvest.  The D4
carry S-transform in `NetworkChainDatum.cpp` shifted `lc.value_abs` and `cp.value`, and
left `lc.coupling[].s_prev` untouched.  A coupling row's RHS is
`y = (value_abs − x_lin) + Σ_j M_j·ŝ_prev,j`, so under a known frame step the row's two
carried references moved **inconsistently** and the block injected precisely the frame
step as row bias.

For a gauge-invariant row the correct net y-change under a KNOWN step is the exact
cancellation of those two moves.  (Not identically zero in general: eq34 realizes its
datum with 1e10-weighted pin **rows** rather than Dirichlet elimination, so the residual
is a bounded pin-leverage term — which is itself a free diagnostic of how much `M`
truncation has eaten.)

It was **latent-and-LIVE, not latent-and-harmless**: at the population site the erase and
shift sets are mutually exclusive by construction —
`if (!k) eq34_reref.insert({sat, grp_sig}); else if (s != 0.0) shift_sat[{sat, grp_sig}] = s;`
— so the shifted keys are exactly the ones whose carries **survive** the erase.  What kept
it dormant is only that the shift door (`eq34_shift_on`, which needs `!carry_invariant`)
is armed together with `carry_conditional_coupling` in essentially no live config;
`config/archive/_dcmp_50sta_10ep_eq34x.json` does arm the full firing set.

**The fix (2026-08-17)** is the one the filing prescribed, in the Card-6 precedent's shape
(`NetworkRelevel.cpp` eph cutover: *the row's functional `u = r + Mᵀs` is frame-consistent
only if ALL carried references move together*):

- `rebaseCouplingPhaseBiasSprev` (`filter/NetworkCoupling.{hpp,cpp}`): applies the SAME
  `+s` the `prev_carry` walk applies to `cp.value` to every `PhaseBias`-kind coupling term
  whose (sat, signal) is in `shift_sat`, with the same zero / non-finite guards.  Nothing
  else moves — `coeff` and `p_prev` are frame-invariant.
- Called in the D4 shift block over **all three** coupling-bearing stashes —
  `local_carry`, `rpb_carry`, `zwd_carry` (the same trio precedent Card-6 walks for clock
  columns) — hoisted **out** of the per-carry `d != 0.0` guard: a carry whose own (sat,σ)
  did not move can still hold coupling terms on other satellites' shifted columns.
- Receiver-side shift maps (`shift_rxg`, `shift_rpb`) need no coupling image:
  `SharedParamKind` spans satellite-side + SH columns only.
- Not erased and not marked departed, exactly as filed: erasing discards the information
  the shift exists to preserve, and departing over-marginalizes a known, exactly-computed
  step.
- Counter `eq34_cpl_sprev_total` (invariant #4), reported in the run-end Card 3/eq34
  line — on an eq34+coupling lineage **0 re-based alongside nonzero v2-shifts is the
  wiring-inert alarm**, not a clean bill.
- Unit tests `NetworkCouplingTest.Eq34Rebase*`: kind/sat/signal selectivity, the exact
  `+s` sign, coeff/p_prev untouched, zero/NaN guards, empty-input no-ops.

⚠ **A/B methodology (same class as §13.6's replay exception):** the change is
solve-affecting on any lineage arming the eq34 shift door together with
`carry_conditional_coupling`, and **no fingerprint token discriminates old from new
behaviour** — an A/B spanning this commit on such a lineage is not a single-variable
comparison; re-baseline it.  In-tree only the archive config above arms the full firing
set, so no live lineage is invalidated.  With coupling off the stashes are empty and the
walk is structurally byte-identical.  The §13.8 ladder rungs remain valid A/Bs — the one
eq34 rung (`fld50`) carried this defect in BOTH arms, so the 1f/1g comparison was still
single-variable — but their absolute numbers will not reproduce on a post-fix build;
re-baseline before quoting any of them against a new run.
