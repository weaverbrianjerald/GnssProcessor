# §18.13/§18.14 — E2 and the dense border across the seam — pre-registered, then its verdicts

**STATUS**

- **id:** §18.13/§18.14
- **topic:** resume-seam
- **status:** closed
- **summary:** E2 and the dense border across the seam — pre-registered, then its verdicts
- **dates in the text:** 2026-09-05
- **knobs / posture:** `network.readmit_seed_horizon_s`, `network.readmit_coupling_marg_ep`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/camp60wu_quiet12h_frwriter_sticky`, `runs/camp60wu_quiet24h_sticky`, `runs/frwriter_sticky_v3c`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — 18.13, 18.14

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
### 18.13 E2 + THE DENSE BORDER ACROSS THE SEAM — BUILT 2026-09-05, PRE-REGISTERED

**Two increments in one build, both default-OFF and byte-identical OFF.**

**(a) E2, seed-first re-admission (§18.4's sketch, resolved).**  The §18.12 residual settled what
the card is: G20's shared columns were `col_removed` at 305220 (eclipse exclusion), `col_added` at
306210 with an INCUMBENT-LESS cold re-pin of its phase-bias family at a new anchor station, and the
Q3 anchor — whose per-satellite carry was 33 epochs old — tripped `gap` at 306240, re-drew the clock
LOOSE on a code-only border and re-coupled on that re-draw at 306330 (`reseed_var` 2.7e-4).  That IS
the T3 mechanism of §18.1 and the F3 episode mechanism of SPEC_amb_iono §6.9; it is not a property of
the restored anchor.  The §18.4 "hard part" (the stored realization lives in the emitted frame)
dissolves once the seed source is the INTERNAL-frame D2 carry itself: `network.readmit_seed_horizon_s`
PARKS a departing satellite's clock + code-bias `CarryPrior` (stamp `t_park_s`; walked by the
eq34/relevel re-bases that walk `prev_carry`; dropped at a clock-datum change of its constellation;
never serialized — a seam inside an absence is today's cold return) and on re-registration inside
the horizon folds it as the column's prior at `var + q·gap`, with the Q3 fold's staleness bound
raised to the horizon for that seed (`clockAnchorFoldVariance(…, seed_horizon_s)`) so an ANCHORED
satellite folds anchored over its true gap and a LOOSE one inflates over the true gap.  The guard is
unchanged and is the mark-if-moved: a `nis`/`abs` trip within 300 s of a seed is attributed
(`;readmit_seed_moved`) and counted.  The phase-bias family is NOT seeded (its frame is re-realized
at the cold re-pin; the leveling carries do not survive the absence at the 120 s leveling gap) — the
incumbent/pin-target parking that would make the PB frame continuous is deferred, with data: on this
posture no arc survives a 55 min absence, so it could not act.  Companion knob
`network.readmit_coupling_marg_ep`: the block/row re-coupling edge (SPEC_amb_iono §6.10).

**(b) §18.12 open item 2 — `prev_carry_cov` across the seam.**  Snapshot format v3 gains `QK`
(dense-carry identity keys in row order) and `QP` (lower triangle per row, explicit length) records,
written under the SAME write knob (`state_snapshot_coupling_capture`) whenever the C1 block is finite
and complete, restored behind the SAME read knob (`resume_coupling_restore`): keys with no restored D
row are dropped (row and column), the block is symmetrized, and the first warm fold runs the DENSE
path.  Counters `n_resume_cpl_cov_keys` / `_dropped`; the existing `n_resume_cpl_diag_border` is the
readout (376 on arm C).  A partial block is a hard parse failure (the QL rule).  Also in the L row:
an OPTIONAL trailing `var_first` field, written only when the handover ratio ceiling is armed.

**Legs (same v3 `.ep1440` of `runs/camp60wu_quiet12h_frwriter_sticky` for D; a re-run writer for E):**

| leg | knobs | pre-registered |
|---|---|---|
| `cplreg_q12warm_d` | arm C + `readmit_seed_horizon_s 3900` + `readmit_coupling_marg_ep 20` | G20/E31 `gap` trips at 306240/306450 do not fire; their per-sat clock error at 309630 ≤ 0.3 m (C: −2.25 / −1.12); consumer h12 rms ≤ 1.0 m (C 2.05); seam SD unchanged vs C (0.025 G / 0.030 E); `cta_decouple reason=gap` over the leg 15 → ≤ 2 |
| `frwriter_sticky_v3c` | the §18.12 writer on the new build (capture ON, knobs OFF) | products a byte-identical prefix of `runs/camp60wu_quiet24h_sticky/products.txt`; `.ep1440` carries ~376 QK + QP rows |
| `cplreg_q12warm_e` | leg D's knobs on the NEW `.ep1440` | `n_resume_cpl_diag_border` 376 → ≤ 20 at the first fold; first-fold `nis` trips ≤ arm C's 2; seam SD ≤ C; consumer h12 ≤ leg D |

Verdicts land in §18.14 as the legs finish; the predictions above are not edited afterwards.

### 18.14 VERDICTS for §18.13 (2026-09-05/06)

**Leg D (`cplreg_q12warm_d` = arm C + E2 seed 3900 s + coupling window 20, same v3 `.ep1440`).**  Seam epoch
identical to arm C (2 first-fold `nis` trips; between-sat SD 0.025 G / 0.030 E).  `gap` trips 15 → 0; the seeds on
the seam's returning satellites are rejected by the guard within 1–2 epochs at 8–61 cm (G20 8 cm at 306210 and
again at 306540, E31 17 cm at 306480, G12 61 cm at 310860 — the pre-gap realization restored from the snapshot and
the post-return solve disagree by that much).  Per-sat clock vs the continuous run at 309630: G20 −2.255 → −1.683,
E31 −1.125 → −0.204, G12 unchanged; G clock SD 0.413 → 0.310 m (max 2.26 → 1.69), E 0.256 → 0.058 (max 1.13 → 0.21).
**Consumer: leg D ≡ arm C to the digit in every 10-min bin** (12.00 2.22 / 12.17 2.15 / 12.33 2.28 / 12.50 2.06 /
12.67 1.85 … 13.83 1.10 m; arm B 1.53 then 0.35 / 0.28 / 0.32 / 0.32 / 0.27 / 0.22 … 0.05).  The pre-registered
"consumer h12 ≤ 1.0 m" is NOT met — and the §18.12 attribution of that bias to G20/E31 was WRONG: the 2 m bias is
present from the FIRST warm bin (p50 2.14 at 12.00), before either satellite returns (h13.05 / h13.1).

**The seam residual, isolated (the splice instrument).**  A COLD consumer replayed on arm C's wire from h12
(`_seamctl_replay_armc.json`) reproduces the in-process consumer to the digit ⇒ a WIRE property.  Its east error
walks from −1.3 m to +2.3 m over the first 20 min and then stays (a permanent bias; the same cold consumer on the
continuous wire converges to 0.2 m in 20 min).  Splicing ONE record family of the continuous run into arm C's
wire (`_splice_{C,P,B,U}.json`, `scratchpad/arms/splice_wire.py`): **replacing the P (phase-bias) records
restores 1.41 / 0.43 / 0.15 / 0.086 m (= the continuous wire); replacing C, B or U changes nothing** (2.25 / 2.08 /
2.25 …).  So the consumer harm of the coupling-regressor restore lives entirely in the phase biases, although at
the seam epoch they are within 2–3 cm SD of the continuous run and the wire's own phase-side steps are 1.4 cm rms
(0 keys > 0.05 m, no disc).  Quantified: P(L2W/L5Q) SD vs the continuous run 0.031 (302430) → 0.102 (+5 min) →
0.131 (+10) → 0.172 (+20) → 0.223 (+40) m — a smooth, unsignalled, coherent drift that the consumer's stiff
ambiguities (q 1e-12) integrate into a position offset which is then PERMANENT (only satellite turnover erodes
it over hours) — consumer-side invariant #1, the T4 "drifts not steps" lesson.  Arm B's phase biases drift by
comparable amounts (0.050 → 0.309 in 40 min) but as a SIGNALLED storm (31 R entries at the seam), so its
consumer reset and re-converged.  Signalled vs unsignalled re-realization is the whole difference, and the
seam-epoch diff + steps grade of §18.12 cannot see an integrated drift — **grade every seam card with a
cold-consumer replay from now on.**  Mechanism candidate (not proven): the restored rows are border-neutral,
so the phase biases are free to follow the per-station layer's re-convergence (per-station filter state is not
snapshotted); arm B's plain diagonal rows fought the border instead (33 trips, a storm).  Fix direction (own
card): hold or SIGNAL the phase-bias realization for the seam window.  E2 is inert here by construction.

**Leg E (`cplreg_q12warm_e`: leg D's knobs on the re-run writer's `.ep1440` with QK/QP).**  PASS — the seam card closes (00:35 PDT).  The dense border restored 376/376 keys from QK/QP: `n_resume_cpl_diag_border` 376 → 0 (pre-registered ≤ 20); first-fold nis trips 1 (C 2); wire at the seam P(L1C) SD 0.0020 m (E) / 0.0053 (G) vs arm C 0.029 / 0.019 and arm B 9.07 / 8.02, clock SD 0.029 / 0.025 (= C); phase-side steps after the seam 0.8 cm rms (C 1.4, B 11).  CONSUMER (in-process, hourly rms / p50): h12 0.619 / 0.097 (arm C 2.053 / 1.974, arm B 0.672 / 0.311, cold-on-continuous 0.523 / 0.115), h13 0.064 / 0.060 (C 1.340, B 0.141, CONT 0.060), h14 0.135, h15 0.104, h16–h23 0.04–0.14 — the §18.12 stop rule (h12 ≤ 1.0 m) is satisfied.  A COLD consumer replayed on leg E's wire (10-min bins) 1.44 / 0.51 / 0.19 / 0.082 / 0.028 / 0.052 m vs the continuous wire's 1.22 / 0.40 / 0.14 / 0.095 / 0.099 / 0.082 — equal or better from the third bin.  The phase-bias divergence from the continuous run still grows (P(L1C) SD 0.002 → 0.022 → 0.038 → 0.078 → 0.156 m over the first 40 min) but no longer harms the consumer: the coherent, unsignalled re-realization of arm C was the one-epoch DIAGONAL border fold at the seam (the off-diagonals of the 376 border keys dropped for exactly the epoch the restored conditional rows first fold), and restoring the dense border removes it.  G20 gap trips gone (E2), 15 re-admissions / 15 clock + 45 code-bias seeds over the leg.  Verdict: `resume_frame_restore` + `resume_coupling_restore` (with a QK/QP-bearing writer) + `readmit_seed_horizon_s` = a warm restart whose consumer converges like a cold consumer on the continuous wire → PROMOTE the two resume knobs to RECOMMEND_SCOPED for warm-resume lineages (writers armed with state_snapshot_frame_capture + state_snapshot_coupling_capture).

**Writer gate:** `runs/frwriter_sticky_v3c/products.txt` 34,840,533 bytes byte-identical to the continuous
run's prefix; `.ep1440` v3 with 376 QK / 376 QP / 2811 QL records (16.8 MB).

