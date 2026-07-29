# CARD 1d — Coupling-row border TIME UPDATE (the Card-1c fix)

> **BUILT 2026-07-10 late session — with two structural discoveries (Card 1e).**
> (1) The Card-1 coupling rows had NEVER entered a solved field epoch: the harvest's
> FREE-column restriction amputated the pinned datum columns' coefficients, breaking
> each row's exact gauge-invariance; the W5 null-space gate then (correctly) withheld
> every rows-epoch, and the 180 s withhold cadence exceeded phase_leveling_max_gap_s
> = 120 s, dropping every carry at solved epochs (the 3-cycle that made every
> coupling-carry A/B byte-identical — Cards 1b and 1d both).  Fix (Card 1e): keep
> pinned/gauge-zeroed columns in the coupling with p_prev = 0.
> (2) The v1 per-column κ shrink itself re-breaks the gauge (clock κ ≪ 1 vs pb/cb
> κ ≈ 1), so the shrink is applied only in the gauge-orthogonal complement: each
> (constellation × kind) coefficient sum is restored to its unshrunk value
> (tuGaugeRestore — the projection of the shrink delta onto the group direction
> restricted to the row's support; the variance inflation is kept in full,
> conservative).  Measured null directions (network.trace_estimability, now the
> first 6 gate evaluations): pure per-constellation uniform CLK shifts; a Galileo
> ambiguity's row has Σ_E(shared) = −1 exactly (the E gauge moves E ambiguities +1).
> Validation: 50-sta GPS+Gal 30-min smoke — legacy carry 30/30 emitted; Card 1
> pre-1e 13/30; Card 1+1e (OFF arm) 30/30; +1d with gauge restore (ON arm) 30/30
> with 3.15M κ-shrunk terms in solved epochs.  Run-end "coupling-carry volume"
> counters + one-shot row/solve-checksum dumps added so an inert carry can never
> again masquerade as a healthy A/B.

*Fable-specced 2026-07-10 evening, from the Card-1c verification (see
`docs/HANDOFF_carry_recursion_card1c.md`, VERIFICATION section, and memory
`project_network_ddfrac_rootcause_shvtec` top entry). Card-1c branch verdict:
**(b) structural defect in the harvest→inject loop** — not a q_cond
accumulation stall (q_cond contracts 1/k textbook-style), not row gating, and
not carry-reset churn: the injected coupling rows are themselves inconsistent.*

## The defect (verified 4 ways, file:line)

The Card-1 coupling row injected at epoch k+1

```
r + M_rᵀ·s = (r̂_prev − x_lin) + M_rᵀ·ŝ_prev      weight 1/(var_eff + dep_var)
```

is an EXACT pseudo-observation of the combination `u = r + M_rᵀ·s` **at the
harvest epoch** (`Var(u|D_k) = cond_var` — u is independent of s by
construction of the regression). But between epochs the border moves:
`s_{k+1} = s_k + w`, `w ~ N(0, Q)` (the SAME Q the D2/C1 fold applies at
ApplicationNetwork.cpp:5646-5654 / 5703-5713), so

```
u_{k+1} = u_k + M_rᵀ·w      ⇒      Var must inflate by  M_rᵀ·Q·M_r
```

and exact re-independence from the border prior requires the shrinkage
`M' = (P+Q)⁻¹·P·M_r`. Production applies NEITHER to layout-present columns —
`buildCplRow` (src/core/app/ApplicationNetwork.cpp:4413-4431 scalar,
4375-4411 joint) uses the raw harvested `cs.coeff` at full strength; the
`dep_var = Σ M²·p_prev` marginalization (4420-4422) fires ONLY for columns
absent from the layout (`resolveSharedCol == -1`). A present-but-moving
column is the un-handled middle case; full marginalization is its q→∞ limit.

**Magnitude in the T7 lineage** (clock_model OFF ⇒ clocks in the C1 border
with `sat_clock_q = 1e-2 m²/s`, dt_eff = 60 s ⇒ `q·dt = 0.6 m²/epoch`;
`M_clk ≈ −2` per the trace): each row is wrong by `M²·q·dt ≈ 1.2 m²`
(σ ≈ 1.1 m ≈ 5.8 L1 cyc) while its claimed σ² is floored at
`phase_leveling_var_floor_m2 = 4e-5 m²` — a ~1.5e4× weight overstatement,
re-injected EVERY epoch, with weight growing ~k as q_cond contracts
(positive feedback). Field signature (T7 12h forensics): per-arc DD steps
flat-with-age at 5.5-7.8 cyc/epoch, values trending (median 106-cyc drift
per arc ≈ 20 formal σ), formal σ flat ~5 cyc, step p50 growing 2.3→11 cyc
over 12 h, blow-up onset bracketed by the two C1 non-PD fold warnings.

**Why Opus's symmetric toy hid it**: with identical stations the two
stations' M_r are identical and the between-station DD cancels the poison
EXACTLY; any asymmetry (station weights, geometry, per-station SH mapping)
leaks it in proportion. Opus's own correlated-iono probe (map_r 1.0 vs 1.4)
leaked ±5 m into the DD at 30-50× overconfidence — that IS this defect
through the SH column (q_sh large), not "σ-incest physics". The field's 50
heterogeneous stations are maximally asymmetric.

**Fix validated batch-exact** (NumPy, scratchpad card1c_replicate.py /
card1c_falsify.py): matrix form makes the 8-epoch recursion equal the exact
batch to 4 decimals (jsig 0.1500 = batch 0.1500; val_err equal; perfect
1/√k through 50 epochs). Scalar per-column form is consistent (never
overconfident, max |err|/σ 2.2) but conservative while border
cross-covariances are strong (σ lags batch 2.75× @8ep → 1.64× @50ep).
Falsifier experiments (card1c_falsify.py): E3 asymmetric-station DD err/σ
250× (production) → ≤1.6 (fix); E4 = Opus's correlated-iono probe, 32.5×
overconfident (production, reproducing the "σ-incest" signature) → ≤1.23
(fix), honest DD σ contracting with NO meter-scale floor — the probe's ±5 m
was this defect, not iono physics.

Known bounded conservatism (both forms): the time update creates a
cross-STATION conditional correlation `Cov(x_r, x_r' | s⁺) =
M_rᵀ(P⁻¹+Q⁻¹)⁻¹M_r'` that independent per-station rows drop — fix σ sits
above batch (e.g. 0.148 vs 0.107 @8ep in E4) but never below truth
(max |err|/σ ≤ 1.8 across all fix arms/horizons). Recovering minimal
variance would need joint cross-station carry rows (v3, only if needed).

## Build

### v1 — scalar per-column κ (production-implementable now)

Toggle `network.carry_coupling_time_update` (default OFF, byte-identical;
requires `carry_conditional_coupling`; GnssConfigLoad pattern + mutex/log
per repo convention). In `buildCplRow` (both scalar and joint paths), for
each PRESENT CoupledShared `cs` with gap-scaled process noise
`qj = q_kind(cs) · gap`:

```
κ_j   = cs.p_prev / (cs.p_prev + qj)                    // 1 when qj = 0
coeff = κ_j · cs.coeff                                  // into A_s AND the RHS term
σ²   += cs.coeff² · cs.p_prev · qj / (cs.p_prev + qj)   // = M²·κ_j·qj
```

- `q_kind` ladder = EXACTLY the D2/C1 fold's (5646-5648): Clock→q_clock,
  CodeBias→q_cb, PhaseBias→q_pb — plus VtecSh→`q_per_degree/n²` (the SH GM
  carry's per-degree q, 5252-5255; `cs.n` is stashed). Same
  `getProcessNoise()` constants already in scope at the assembly site.
- `gap` = the existing `pl_now_s − last_seen_s` already computed at the
  inject call sites (4468 amb, ZWD analogue).
- Clock under clock_model=ON: the column is carried by the two-state model,
  not D2/C1 — v1 treats it with the same q_clock (conservative; refine later).
- A clock-monitor reinit-flagged sat (the D2 `inflate_q` path): treat as
  q=∞ ⇒ κ=0, σ² += M²·p_prev (mirror the dense fold's decorrelation rule).
- Departed columns: unchanged (they are already the κ=0 limit).
- Joint path (Card 1b, dormant): shrink present columns identically before
  whitening; M_w/white_pw for departed columns unchanged.
- Floors: pl_var_floor applies to var_eff (the conditional part) as today;
  the border inflation adds AFTER the floor.
- No stash/snapshot/wire change: κ needs only `cs.p_prev` (already stashed,
  8117-8119) + q ladder + gap at inject time.

### v2 — matrix-exact (optional, after v1 field validation)

`M' = (P+Q)⁻¹·P·M` and joint row covariance
`R = diag(cond_var_i) + M_i,shrunkᵀ(P⁻¹+Q⁻¹)⁻¹M_j,shrunk` over each
station's rows — this is EXACTLY the Card-1b whitening structure with
`P_w → (P⁻¹+Q⁻¹)⁻¹` and applied to present columns, i.e. Card 1b's real
mechanism (un-retire its plumbing). P source: `prev_carry_cov.P` +
`sharedCarryKey` row map (serial scope; read-only in parallel assembly).
Factor (P+Q) once per epoch. Motivation: scalar conservatism costs ~2.7× in
accumulation time toward AR-grade σ.

## Tests

1. Gate test `ConditionalCarryTimeUpdateRecursiveEqualsBatch`: the Card-1c
   trace fixture **with the card-spec LARGE-q clock border (q=1e4)** —
   recursion(fixed) == batch. NOTE: the existing
   ConditionalCarryRecursiveEqualsBatch passes only because its border is
   quasi-static between epochs — that is the test gap that shipped this.
   The new fixture must keep the large q.
2. q=0 regression: all q_kind = 0 ⇒ κ=1, zero inflation ⇒ rows numerically
   identical to the unfixed path (guards the OFF-adjacent behavior).
3. q→∞ equivalence: scalar path with huge q reproduces the departed-column
   dep_var branch exactly.
4. Extend ConditionalCarryContractionTrace with a fixed arm: val_err/σ ≤ 3
   all epochs, DD σ AND single σ contract, asymmetric-station variant clean.
5. Suite green; byte-identity OFF (SHA vs frozen baseline).

## Falsifier (field, the only grading that counts)

T7 rerun (same config + `carry_coupling_time_update: true`), 12 h:
- per-arc DD steps collapse from 5.5-7.8 cyc/epoch toward √2·honest-σ and
  DECAY with arc age;
- formal dd_sigma_cyc declines hour-over-hour (no 5-cyc plateau);
- no run-level step growth; C1 dense folds stay PD;
- THEN the T6 low-floor 12 h question is finally answerable: if σ → AR-grade
  and fractions cluster ⇒ producer floats healthy ⇒ Card 3 (Eq-34 datum)
  + armed AR. If σ converges honestly but fractions stay uniform ⇒ next
  suspect (code-bias double-datum) per the master trail.

## Known-open (adversarial review wf_879ed034, deferred)

- **Pivot-handover response for coupling rows**: on a clock-datum handover the D2
  fold skips the retired-datum keys, but the coupling rows' ŝ_prev terms still
  reference the OLD frame's solved values (a pre-existing Card-1 gap, not
  1d-specific; the rows stay gauge-INVARIANT structurally, but their VALUE frame
  is stale for one epoch).  Sticky-pivot makes this rare; the clean fix is to
  drop/re-seed the coupling stash on `datum_changed` (mirroring the D2 skip).
- The unit gate tests validate the κ math on a gauge-free fixture; production
  additionally applies tuGaugeRestore, which in a gauge-free system would undo
  the shrink for single-column groups (correct only when the group spans a real
  gauge, as in the field).  The field smoke (30/30 emitted, A/B differing) is the
  acceptance test for the restore itself.

## Sequencing

Card 1d (this) → floors can drop honestly → 12 h fraction readout →
Card 3 Eq-34 datum (per-(receiver,signal) phase-bias states + single-receiver
datum / de Jonge chain pivoting) + self-AR harness. Card 3's spec is
independent of this fix and can be drafted in parallel, but its field
grading is meaningless until the carry is honest.
