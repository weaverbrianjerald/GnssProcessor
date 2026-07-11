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
Dead-carry T7 was gradual ⇒ live rows necessary.

**RESOLVED 2026-07-11 (roadmap M3): the floor hypothesis is FALSIFIED.**
The three-point ladder (runs/t7g_fullfloor_12h = 4e-4, runs/t7d_tu_12h = 4e-5,
runs/t7g_lowfloor10_12h = 1e-5; 40× swing, knob demonstrably active — clean-
window formal dd_σ 0.178/0.181/0.203 monotone in floor) shows all arms clean
hr 0–3 and ALL cliffing at content-hr 4 with identical signatures (DD-step p50
27–29 cyc/ep, a00 → −3000..−3400).  The floor modulates carry weight but does
not move arming by even one hour.  The divergence is EVENT-DRIVEN: the 04:00
UTC PB-set datum reshuffle (03:46 E14-out/G31-in) → silent re-level → stale
coupling-row references integrate → runaway (the t7h offset verdict; memory
card1e-gauge-amputation).  Cure path: the event response — datum-continuous
PB handover, and structurally the invariant-function carry
(docs/HANDOFF_card4_invariant_carry.md), under which a PB-set reshuffle cannot
poison anything the filter remembers.  The LS-VCE card
(docs/HANDOFF_lsvce_carry_q_m2.md) survives only as a formal-σ honesty
instrument — the 14% formal-σ spread across the ladder with identical actual
behavior shows today's formal numbers are floor artifacts.
