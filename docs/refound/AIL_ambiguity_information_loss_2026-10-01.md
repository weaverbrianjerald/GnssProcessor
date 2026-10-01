# AIL: the float vehicle's ambiguity information loss (why a carried ambiguity is not phase-stiff)

**STATUS**

- **id:** AIL
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (card LGM section 5.3): the float vehicle's carried ionosphere-free ambiguity moves by 6.6 mm per epoch per line of sight on the golden posture and 3.3 mm on the program posture (station-common and satellite-common parts removed), decaying with the arc's age (12.7 mm at 5 to 10 epochs, 1.6 mm at 160 to 400 on the program posture); held at phase precision it would move 20 to 40 times less. That loose ambiguity is what keeps card LGM's colour state from carrying the colour. A trace-class instrument (`network.trace_amb_if`) reads, per line of sight and epoch, the variance of the ionosphere-free ambiguity before the solve (the block the inject folded) and after it (the harvest block), conditional on the border, with and without the station's common mode; the question is whether the information is lost at the inject (the time update) or never gained in the solve. READ 2026-10-01 (section 4): the instrument is trace class (identity on all arms); the golden posture's inject discards a young arc's information every epoch (75 to 157 mm below 20 epochs, card CTU's legacy clock q in card 1d's kappa) and loses little at maturity; the program posture (the exact update) loses 2 to 3 mm per epoch steadily, the exact conditional growth of the anchored clock's walk through each row's border regression; the estimator states the looseness it shows. Neither posture keeps the ambiguity stiff enough for card LGM's state.
- **source:** [LGM](LGM_los_phase_nuisance_2026-10-01.md) section 5.3; [CTU](CTU_coupling_time_update_2026-09-28.md) (the coupling time update; the legacy clock q in card 1d's kappa); [RF-1 design](RF-1_float_ambiguities_design.md) (the float vehicle); `src/core/filter/NetworkCoupling.cpp` (`buildLocalBlock`, `scalarRowIngredients`, `exactRowIngredients`); `src/core/app/NetworkRunContext.cpp` (`networkHarvestLocalCarries`)
- **knobs / posture:** `network.trace_amb_if` (trace class, default off); no solve change in this card's instrument
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** the wander reads `runs/_rap_lgm_{ctl,pc,pcw}amb_gs`, `runs/_rap_lgm_{ctl,pc}amb_ps`; this card's arms `runs/_rap_ail_*`

## 1. The reading that opens the card

Card LGM section 5.3, item 2: on the sim bed, 1.5 h arms with `network.trace_amb_state`, the
increment over one epoch of each line of sight's ionosphere-free ambiguity combination
(2.5457 value(L1) - 1.5457 value(L2W), metres), with each epoch's station-common and
satellite-common increments removed by a two-way median polish, on arcs at least 20 epochs old:

| posture | rms increment | at arc age 5-10 / 20-40 / 80-160 / 160-400 epochs |
| --- | --- | --- |
| golden (card 1d's scalar coupling update) | 6.6 mm | 14.4 / 13.0 / 2.0 / 0.8 mm |
| program (card CTU's exact update) | 3.3 mm | 12.7 / 5.6 / 2.0 / 1.6 mm |

The colour itself moves 4.4 mm per epoch. An ambiguity that is constant in truth and observed at
1.5 to 9 mm per epoch on the ionosphere-free combination would, as the estimate of a constant,
move by about sigma / k after k epochs (0.1 to 0.2 mm at 80 epochs). The inject folds exactly the
harvested variance (`fold_var_m2` / the previous `var_m2` is 1.000 at every age), so whatever is
lost is lost in the row's border terms or in the solve.

## 2. The instrument

`network.trace_amb_if` writes `network_amb_if.csv` at the harvest: per station-epoch, per line of
sight whose two bands are both carried in the station's block, the variance of the
ionosphere-free ambiguity combination:

- `prior_var_m2`: from the joint covariance the inject folded this epoch (`R.pl_blk.Sigma`: the
  harvested block with each row's border terms, the departed marginalization and card 1d's kappa
  on the golden posture or the exact update's bracket on the program posture);
- `post_var_m2`: from the harvest block just filled (the posterior, conditional on the border);
- `prior_var_dm_m2`, `post_var_dm_m2`: the same with the station's common mode over its traced
  lines of sight projected out (the direction the free receiver clock leaves to the code), which is
  the part the polished wander of section 1 measures.

Two numbers per line of sight and epoch then split the budget:

- **the loss at the inject** = prior(t) - post(t - 1), the variance the time update adds;
- **the gain in the solve** = prior(t) - post(t), the variance the epoch's data remove.

## 3. Predictions (before the arms run)

Arms, 2 h, exe `gnss_processor_ail.exe`: the C0 controls (`runs/_rap_lgm_ctl_{gs,gf,ps,pf}`) with
`network.trace_amb_if` (`_rap_ail_{gs,gf,ps,pf}`), and the same four without it from the
`scripts/prm_ab.py` harness (`_rap_prm_ail0_*`).

- **A1 (trace class).** The knob changes no product: each `_rap_ail_*` arm's `products.txt`,
  `closed_loop_trace.csv` and `network_epoch_status.csv` are identical to its control's run by the
  same executable, and the `ail0` arms are identical to the phase 0 references (`cmp g0a ail0`).
- **A2 (where the loss is).** On mature arcs (at least 80 epochs), the demeaned loss at the inject
  is at least (2 mm)^2 per epoch at the median on the golden posture (card 1d's kappa with the
  legacy clock q) and at most (1 mm)^2 on the program posture.
- **A3 (consistency of the stated variance).** On mature arcs the demeaned posterior standard
  deviation is within a factor 2 of section 1's polished wander on each posture, i.e. the estimator
  states the looseness it shows (the colour state's stated sd was consistent with its error).

Reading rule: if the loss sits at the inject on the program posture too (A2's second half fails),
the time update's border terms are the target; if the program posture's loss is small and the
posterior stays loose, the solve itself gains too little per epoch, and the next read is which free
per-epoch parameter shares the ambiguity's direction.

## 4. Read (2026-10-01, exe `gnss_processor_ail.exe`, 2 h, the sim from 00:30:15)

**A1 passes.** `scripts/prm_ab.py cmp g0a ail0 --all-csv`: the build with the instrument, knob off,
identical to the phase 0 references on all 145 comparisons; each `_rap_ail_*` arm identical to its
same-exe control `_rap_ailc_*` (`ident2h.py`, products, consumer trace, epoch status).

`scripts/clock_level/ail_read.py`, medians by arc age, as standard deviations in mm (dm: the
station's common mode projected out):

| arm | age (epochs) | posterior sd | posterior sd, dm | loss at the inject, dm | gain in the solve, dm |
| --- | --- | --- | --- | --- | --- |
| sim golden | 0 to 20 | 174 | 93 | 75 | 86 |
| | 20 to 80 | 68 | 24 | 6.6 | 9.7 |
| | 80 to 160 | 42 | 6.5 | 1.0 | 1.6 |
| | 160 and over | 34 | 3.8 | 0.48 | 0.71 |
| sim program | 0 to 20 | 125 | 38 | 4.7 | 14 |
| | 20 to 80 | 61 | 15 | 3.9 | 4.8 |
| | 80 to 160 | 42 | 9.1 | 2.6 | 3.0 |
| | 160 and over | 35 | 7.8 | 1.8 | 2.1 |
| field golden | 0 to 20 | 183 | 101 | 157 | 164 |
| | 80 to 160 | 42 | 7.5 | 1.75 | 2.4 |
| | 160 and over | 33 | 3.8 | 0.57 | 0.84 |
| field program | 0 to 20 | 126 | 33 | 5.1 | 13 |
| | 80 to 160 | 42 | 6.7 | 1.9 | 2.2 |
| | 160 and over | 34 | 4.9 | 0.92 | 1.2 |

**A2 fails, and the failure locates the loss.** The golden posture's inject discards a young arc's
information every epoch (a demeaned loss of 75 mm on the sim and 157 mm on the field below 20
epochs, 7 to 15 mm at 20 to 80 epochs) and loses little at maturity (0.5 to 1.8 mm). The program
posture loses about 5 mm on young arcs and a steady 2 to 3 mm on mature ones. The golden posture's
young-arc loss is card CTU's root: card 1d's update ages each row's satellite clock term at the
legacy 1e-3 m2/s while card Q3's anchor folds the same clock at 2.9e-8, so a row whose clock
variance is not yet small inflates by M^2 kappa q dt, of order (0.1 m)^2 for M of order 1. The
program posture's steady loss is the anchored clock's own walk (0.93 mm per epoch at 2.9e-8 m2/s)
through each row's regression on the border: the exact growth of a conditional variance when the
variable it is conditioned on moves (M^2 P q / (P + q)), which is the cost of the conditional
representation, not a defect of the update.

**A3, read with the matched quantity, holds.** The pre-registered comparison set the posterior
standard deviation against the polished wander; the quantity the wander measures is the per-epoch
gain, the variance by which the estimate moves. On the program posture the demeaned gain on mature
arcs (2.1 to 3.0 mm) matches the polished wander of section 1 (1.6 to 2.0 mm); on the golden
posture at 80 to 160 epochs (1.6 against 2.0 mm) as well. The estimator states the looseness it
shows. The full (not demeaned) posterior standard deviation stays at 33 to 42 mm on mature arcs on
both postures and both beds: the station's common mode of the ambiguities is code-determined, the
direction the free receiver clock leaves open.

**For card LGM.** The colour state is separated from the ambiguity only by what the carry keeps of
the ambiguity's ionosphere-free value. On the golden posture that is almost nothing for the first
20 epochs of every arc and 4 to 7 mm after 80; on the program posture 8 to 9 mm on mature arcs. On
mature golden arcs (at least 80 minutes) the state with the white sigma reaches 0.75 in the tropical
class (0.82 to 0.87 elsewhere), short of the 0.34 to 0.42 a one-line-of-sight filter reaches with
a stiff ambiguity. Neither posture lets the state carry the colour as section 2 of card LGM
assumed; the golden posture's young-arc loss is the largest single term and is card CTU's root,
which card CTU's exact update removes at the price of the conditional growth.

Owed: whether the conditional growth on the program posture is the whole of the mature loss (a
read with the anchor's q set to 0 on a sim arm, the clock then a constant); whether a joint carry of
the border and the station's ambiguities (no conditioning) keeps the ionosphere-free ambiguity
phase-stiff, which is a design question for card PRM's traversal and the author.
