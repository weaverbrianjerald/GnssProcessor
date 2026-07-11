# GOLDEN baseline — live coupling carry (Cards 1 + 1e + 1d), 2026-07-10

Recovery anchor for the Card-1e/1d session (see
`docs/HANDOFF_carry_timeupdate_card1d.md` header and memory
`project_card1e_gauge_amputation`).  If a future session needs to re-verify or
resume this state, everything required is below.

## Tree state

UNCOMMITTED on top of `d003698` ("Freeze decomposition baseline").  The session's
changes: Card 1d (`network.carry_coupling_time_update` + gauge re-projection +
reinit history + eph-cutover ŝ_prev re-level), Card 1e (harvest keeps pinned
columns, p_prev = 0), coupling-carry volume counters + one-shot debug dumps,
estimability dump = first 6 gate evaluations, 2 new unit tests + fixed
contraction-trace arm.  Suite: **606 pass / 1 skip**.  Default-OFF byte-identity
proven (8 artifacts SHA-identical with toggles absent).  Consider a checkpoint
commit — this baseline doc assumes the tree as of tonight.

## Golden configs (`config/test_suite/`)

| config | what it is |
|---|---|
| `_golden_carrylive_off_3h.json`  | Card 1+1e armed, time-update OFF (raw live rows) — the A/B reference arm |
| `_golden_carrylive_tu_3h.json`   | + `carry_coupling_time_update: true` — the VALIDATED clean arm (200 emitted-epoch window) |
| `_golden_carrylive_tu_12h.json`  | same, 43200 s — the decisive long-horizon instrument (currently diverges at hour 4, see below) |

Common base: 50-sta GPS+Galileo 2024-129, decim 2 (60 s), SH deg-4 treated pin
(σ_rep 5 TECU, row decimation 3), phase-leveling zero-q + LOW floors
(pl 4e-5 m², pb 3e-7, cb 3e-5), `shared_carry_full_cov` + `carry_conditional_coupling`
+ `code_datum_time_linked` ON, `carry_coupling_joint_whiten` OFF, MW detect ON.

## Expected verification numbers (fresh run of the 3h pair)

- Both arms: `producer complete: 195 epochs emitted, 5 skipped` (pre-1e the rows
  arm gave 13/30-style gate cycling — if you see mass `gate_withheld`, Card 1e
  regressed).
- Run-end `Network (Card 1/1d): coupling-carry volume` — OFF: ~23.3M present /
  0 shrunk; ON: ~23.3M present / ~23.2M kappa-shrunk; 0 EMPTY-regression,
  0 harvest failures on both.
- Step forensics (`scratchpad t7d_step_analysis.py`, or re-create from
  `t7_step_analysis.py` in the Card-1c session scratchpad): ON step p50 ≈ 0.103
  cyc / σ slope ≈ −0.59 / dd_sigma p50 ≈ 0.196; OFF step p50 ≈ 0.550 / slope
  ≈ −0.31.  Fractions (grade_t5.py, 3600 s tail): ON σ_med ≈ 0.119 cyc,
  mean|f| ≈ 0.245 (uniform), a00 ≈ 34.8 ± 1.3 TECU.
- Tonight's artifacts: `runs/t7d_off_byteid` (md5 dd_frac bc078d94…),
  `runs/t7d_tu` (19477cc8…), `runs/t7d_tu_12h` (70b2ae8f…).

## Open question the 12h instrument answers

`_golden_carrylive_tu_12h`: clean hours 0–3, **diverges at hour 3.90** (a00
30 → −75 TECU in 13 min, then to ~±1900; steps p50 27 cyc; LOS>3σ 0.99) while
the LOCAL carry σ stays honest.  Discriminators ALL NEGATIVE (identical cliff):
pb/cb floors ×10 (`runs/t7e_pbcbfloor_12h`), raw rows without 1d
(`runs/t7d_off_12h`), SH couplings fully marginalized
(`runs/t7f_shmarg_12h`, Card 1d-SH toggle `carry_coupling_sh_marginalize` —
kept in tree, conservative).  C1 non-PD folds at hr 4.77/7.85 are symptoms.
Dead-carry T7 was gradual ⇒ live rows necessary.  LEADING HYPOTHESIS: the PL
var floor sets the arming time (4e-5 caps row weight only below σ≈0.033 cyc ≈
hr 3.9; full 4e-4 caps at 0.105 cyc ≈ hr 1 and was always stable).  Next-session
test pair: 12h @ pl_floor 4e-4 + 1d ON (predict no cliff) vs pl_floor 1e-5
(predict earlier cliff ~hr 2-2.5).  If confirmed: replace the hand floor with a
stability-derived cap or q_amb>0, and/or build the shared-local cross-block
time-linking (audit finding [0]) as the rigorous cure.
