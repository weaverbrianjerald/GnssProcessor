# AIL: the float vehicle's ambiguity information loss (why a carried ambiguity is not phase-stiff)

**STATUS**

- **id:** AIL
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (card LGM section 5.3): the float vehicle's carried ionosphere-free ambiguity moves by 6.6 mm per epoch per line of sight on the golden posture and 3.3 mm on the program posture (station-common and satellite-common parts removed), decaying with the arc's age (12.7 mm at 5 to 10 epochs, 1.6 mm at 160 to 400 on the program posture); held at phase precision it would move 20 to 40 times less. That loose ambiguity is what keeps card LGM's colour state from carrying the colour. A trace-class instrument (`network.trace_amb_if`) reads, per line of sight and epoch, the variance of the ionosphere-free ambiguity before the solve (the block the inject folded) and after it (the harvest block), conditional on the border, with and without the station's common mode; the question is whether the information is lost at the inject (the time update) or never gained in the solve. READ 2026-10-01 (section 4): the instrument is trace class (identity on all arms); the golden posture's inject discards a young arc's information every epoch (75 to 157 mm below 20 epochs, card CTU's legacy clock q in card 1d's kappa) and loses little at maturity; the program posture (the exact update) loses 2 to 3 mm per epoch steadily, the exact conditional growth of the anchored clock's walk through each row's border regression; the estimator states the looseness it shows. Neither posture keeps the ambiguity stiff enough for card LGM's state. Section 4.1: with the anchor's rate near zero on the sim the program posture's loss falls 8 to 10 times and the state carries the colour (10.0 against 12.0 mm, G1 0.71 on mature arcs, no consumer penalty) while the clock level suffers; the anchor's walk is the channel.
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

## 4.1 The anchor's walk as the program posture's loss (2026-10-01)

The read owed by section 4: the program posture's sim arms with card Q3's anchor rate at 1e-14
instead of 2.9e-8 m2/s (the satellite clock a constant in the fold), exe `_ail`, 2 h:
`_rap_ail_q0_ps` (the control), `_rap_lgm_pcq0_ps` (card LGM's state), `_rap_lgm_pcwq0_ps` (the
state with the white sigma 0.0005 m).

| reading | `_rap_ail_ps` (2.9e-8) | `_rap_ail_q0_ps` (1e-14) |
| --- | --- | --- |
| demeaned loss at the inject, 80 to 160 / 160 and over epochs | 2.6 / 1.8 mm | 0.28 / 0.23 mm |
| demeaned posterior sd, 80 to 160 / 160 and over epochs | 9.1 / 7.8 mm | 3.0 / 2.5 mm |
| demeaned loss at the inject, under 20 epochs | 4.7 mm | 0.34 mm |
| consumer p50 hours 1 / 2 | 0.232 / 0.098 m | 0.226 / 0.099 m |

The steady loss is the anchor's walk: with the clock a constant it falls by a factor of 8 to 10 at
every age, and the carried ionosphere-free ambiguity at maturity is known to 2.5 mm (the
one-line-of-sight filter's ideal is about 3 to 4 mm against the sim's colour).

Card LGM's state with the ambiguity stiff:

| reading | state (3 mm white) | state, anchor 1e-14 | state + 0.5 mm | state + 0.5 mm, anchor 1e-14 |
| --- | --- | --- | --- | --- |
| G1 tropical, arcs at least 10 min: estimate rms against truth 12.0 mm; ratio; corr | 2.5 mm; 0.901; +0.57 | 4.6 mm; 0.813; +0.64 | 4.5 mm; 0.854; +0.55 | 10.0 mm; 0.856; +0.58 |
| G1 tropical, arcs at least 80 min: ratio; ratio de-meaned; corr | 0.873; 0.730; +0.61 | 0.774; 0.652; +0.69 | 0.809; 0.654; +0.61 | 0.710; 0.542; +0.71 |
| post-fit acf(1), tropical | 0.53 | 0.62 | -0.01 | +0.07 |
| consumer p50 hours 1 / 2 (control 0.232 / 0.098 m) | 0.232 / 0.096 | 0.226 / 0.099 | 0.372 / 0.115 | 0.251 / 0.097 |
| sim datum-free level at 16 / 60 / 120 epochs (control 0.09 / 0.04 / 0.03 m) | | | | 0.15 / 0.13 / 0.12 |
| anchor trips; withheld epochs | | | | 42; 4 |

With the clock a constant, the state with the white sigma carries the colour at close to its full
size (10.0 mm rms against 12.0, correlation +0.71 on mature arcs, G1's 0.7 bar reached in the
tropical class on mature arcs) and the consumer penalty of section 5.3 of card LGM is gone (0.251 /
0.097 against 0.372 / 0.115 m). It disturbs the clock level instead (0.12 against 0.03 m at 120
epochs, 42 anchor trips): with the colour modelled and the white sigma at the white level, the
phase rows hold the level the anchor no longer lets move. The walk-share instrument does not apply
at this rate (the clone's walk is the anchor's walk, about 0.001 mm per epoch).

**Reading.** The anchor's rate is the satellite clock's allowed walk, and through the conditional
representation it is also each carried ambiguity's per-epoch dilution and the channel through which
a station's coloured phase reaches the border: with it near zero the colour stays in the station
(the state carries it), with the field's 2.9e-8 it is shared between the state, the ambiguity and the
clock. On the field the walk is physical (the a priori clock product's error, card QPV), so the
dilution there is real information, not a representation defect; a better-determined clock walk
(more stations per satellite, or a better a priori product) is what would stiffen the ambiguity.

Owed, the author's decisions:
- the golden posture's young-arc loss (section 4, card CTU's root) is removed by card CTU's exact
  update, which is built and not armed (its gates G4 and G5 open); this card adds a reason to
  close them;
- card LGM's state stays default off; on a posture where the clock's walk is small against the
  colour it carries the colour, and on the field posture it is limited by the anchor's walk.
