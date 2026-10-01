# LGM: a per-line-of-sight Gauss-Markov phase nuisance state (the stations' coloured phase error, modelled)

**STATUS**

- **id:** LGM
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (the ranking recorded after cards SOI, I2S, LSR, SFR and WUP: the per-line-of-sight nuisance state first, built through card PRM's registry, the phase-weight inflation as its one-knob control arm). The field's post-fit ionosphere-free phase residual is about 90 % a Gauss-Markov process (card SFR section 1); the network states every phase row white, so each station's coloured error enters the satellites' phase-visible clocks as persistent steps (card FWS), which the re-based sim now reproduces out to an hour (card SFR section 5.2). This card models the error: one band-common, phase-only Gauss-Markov state per station and satellite, carried epoch to epoch. Control arm C0 (the phase sigma times 2.2, the effective-sample-size correction) is predicted in section 3 and runs first; the build (section 4) follows card PRM's phase 0 and phase 1 and adds the family as the registry's first new kind; gates in section 5, truth-graded on the sim fixture's own colour realization. BUILT AND READ 2026-10-01 (sections 4.1, 5.1 to 5.3), default off, NOT ARMED: with the posture's white sigma the state passes G0, G4 and G5 (field golden consumer hour 1 0.559 to 0.440 m) and moves G1 to G3 the right way but not far enough; with the white sigma at the white level it whitens the residual completely (G2) and costs the sim consumer up to 14 cm. The state cannot separate the colour from the carried ambiguity, which moves 3 to 7 mm per epoch per line of sight (the float vehicle loses information every epoch); open behind card AIL.
- **source:** [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md) sections 1, 4 and 5.2; [FWS](FWS_field_walk_share_sources_2026-10-01.md) sections 2 to 5; [LSR](LSR_long_scale_remainder_2026-10-01.md) section 7; [SOI](SOI_second_order_ionosphere_leak_2026-10-01.md) section 6 (the fallback named there); [PRM](PRM_parameter_registry_2026-09-29.md) sections 4 and 8; [PVN](PVN_phase_visible_nees_2026-09-29.md) section 5; [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md) (the increment reads)
- **knobs / posture:** C0 is config only (`filter.measurement_noise.phase_std_m`); the build adds a default-off block (names fixed in section 4.1 before code); no golden, ledger or posture change in this card
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** exe `build/x64-Release/gnss_processor_lgm0.exe` (main `17fbb0c`, sha256[:16] `0601c7d08d4f0ed6`); arms `runs/_rap_lgm_*`; the sim colour truth `runs/simnet60_quiet_col_24h/<station>/truth_phase_colour.csv`

## 1. Why

Three readings of 2026-10-01 put the cause in one place.

- **The error is coloured.** Card SFR section 1 (`phase_acf.py`, 6 h): per station, the
  ionosphere-free post-fit phase residual on the field is 5 to 7 mm rms with lag-1 autocorrelation
  0.61 to 0.74, about 90 % a Gauss-Markov process with a post-fit time constant of 80 to 120 s over
  about 2 mm of white noise. The estimator states every phase row independent (2 mm per band on the
  field, 3 mm on the sim, elevation power 1).
- **The colour is what leaks.** Card FWS: the field's walk share is larger than the sim's because
  its steps persist, not because they are larger, and the persistence reaches a satellite in
  proportion to how few stations see it. Card SFR section 5.2: once the sim fixture carries a
  calibrated colour (300 s, 6.0 / 4.9 / 4.4 mm by latitude class, capped at 20 degrees of
  elevation), the sim's walk-share persistence ratio over 6 h reads 1.58 / 1.75 / 1.48 / 1.18 at
  5 / 20 / 60 / 120 epochs against the field's 1.45 / 1.51 / 1.54 / 1.53 and the white fixture's
  1.05 / 0.93 / 0.68 / 0.54. About a third of the field's 2 h excess is left unexplained.
- **Nothing else in the model absorbs it.** The error is band-common (it does not enter the
  geometry-free combination, so the slant ionosphere cannot take it), line-of-sight specific (the
  station's wet delay and clock cannot take it), and phase-only (no code row sees it). The carried
  float ambiguities take its mean over an arc; what varies inside the arc is left in the phase rows,
  stated white, and the solve spreads it into whatever border parameter the few stations of a thinly
  seen satellite share.

A filter that states correlated errors as independent overcounts their information by the
effective-sample-size factor. For a Gauss-Markov process with lag-1 correlation phi the variance of
an n-sample mean is about sigma^2 (1 + phi) / ((1 - phi) n): at phi 0.74 (tau 100 s at 30 s epochs)
the factor is 6.7, at 90 % coloured share about 6.1, a sigma factor of about 2.5. The ranking named
2.2.

## 2. The model

Per station r and satellite s, one state x_rs (metres):

- **Observation.** +1 on every phase row of (r, s), both bands; 0 on code rows. It enters the
  ionosphere-free phase with coefficient 1 and the geometry-free phase with 0.
- **Dynamics.** x_k = phi x_(k-1) + w_k, phi = exp(-dt / tau), var(w_k) = sigma(e)^2 (1 - phi^2),
  stationary variance sigma(e)^2. The elevation law is the fixture's (card SFR section 4.2):
  sigma(e) = sigma_zen / sin(max(e, e_floor)), e_floor 20 degrees.
- **Birth.** At an arc's first epoch, x ~ N(0, sigma(e)^2). After a gap longer than a few tau the
  state is re-born (its carried value has decayed to nothing).
- **Identification.** Within one epoch x_rs is confounded with the band-common direction of the two
  float ambiguities of (r, s) and is identified only by its prior. Across epochs the constant
  ambiguity takes the arc mean and x the deviation from it, on the time scale tau. No new datum
  direction: the state is station-local and its prior is proper.
- **Parameters.** The estimator needs the process's own parameters, not the post-fit ones. The
  post-fit time constant (80 to 120 s) is shorter than the process's because the estimator absorbs
  the slow part; the fixture's calibrated recipe (tau 300 s, sigma 4.4 to 6.0 mm by class)
  reproduces the field's post-fit colour in every class (card SFR section 4.2), so it is the first
  candidate on both beds: sigma_zen 5 mm, tau 300 s, e_floor 20 degrees.

## 3. Control arm C0: the phase sigma inflated (predicted before it runs)

One knob: `filter.measurement_noise.phase_std_m` times 2.2 on each bed's own value (field 0.002 to
0.0044 m, sim 0.003 to 0.0066 m). It accounts for the overcounting on average and models nothing.
Eight 2 h arms from exe `_lgm0`: the program posture (the golden plus the nine knobs of the
clock-level program: the exact coupling update, the smoothed block hold and the monitors; bases
`runs/_rap_wup_prog_f/arm.json` and `runs/_rap_sfr_col6_p/arm.json`) and the golden posture, each bed,
each with its own control (`_rap_lgm_{ctl,rinf}_{pf,ps,gf,gs}`); the sim from 00:30:15.

Predictions (each read against its own control):

- **C0-1, colour.** The post-fit ionosphere-free phase residual's lag-1 autocorrelation moves by
  less than 0.05 in every station class (`phase_acf.py`): inflation does not model the colour.
- **C0-2, persistence.** The walk-share persistence ratio at 5 and 20 epochs (`wsf_persist.py`, the
  program arms; a 2 h arm reads clone ages 30 min to 1.5 h) moves by less than 0.15: the correlation
  of the steps is a property of the data.
- **C0-3, consistency.** Card PVN's NIS mean (`pvnees.py`) falls on both beds (the stated update
  variance grows faster than the update); card FXA's increment lag-1 correlation of the revision
  (`fxa_attrib.py`) moves by less than 0.05.
- **C0-4, consumer.** The golden arms' held-out consumer p50 in hours 1 and 2 moves by less than
  3 cm on each bed (the 2 h lottery band).

Reading rule: if C0-2 and C0-3's lag correlation hold, inflation leaves the leak untouched and the
state of section 2 is the remaining lever; if the persistence falls by more than 0.15, inflation does
part of the state's job and the state's gates (section 5) are read against C0 as well as against
the control.

## 4. The build

In the order card PRM section 8 fixes, one commit each, each gated:

1. **PRM P0, the harness.** A same-build A/B script (`scripts/prm_ab.py`): 2 h arms from both
   goldens on two exes, compared on `products.txt` without comments, `closed_loop_trace.csv` and
   `network_epoch_status.csv`. Gate: two builds of the same source are identical.
2. **PRM P1, the registry.** Card PRM sections 4.1 to 4.4 and 6: the parameter kinds, one
   descriptor per kind built once from the config, the single process-noise source, the
   disposition on the carried prior and the "model as armed" table. Gate: byte-identical on both
   beds and both postures; the unit test of the armed values.
3. **The nuisance kind.** The registry's first new entry (kind, descriptor, Gauss-Markov q rule),
   its local column, its carry and its responses, default off. The concrete design (the column's
   place in the station block, the carry's form, the responses to a slip, a gap and a drop, the
   counters at both edges of every event, invariants 1 and 7) is appended as section 4.1 before
   code, from a read of the ambiguity carry's lifecycle.

## 5. Gates for the nuisance state (2 h unless stated; sim from 00:30:15)

- **G0, identity.** Off: byte-identical to the pre-build exe, both beds, both postures.
- **G1, the state carries the colour (sim, truth).** Against `truth_phase_colour.csv`, the pooled
  rms of x_hat - x_true over arcs older than 10 minutes is at most 0.7 of the truth's rms (at least
  half the colour's variance recovered), in every station class.
- **G2, whitening (both beds).** The post-fit ionosphere-free phase residual, formed with the
  state, has lag-1 autocorrelation at most 0.35 in every class (from 0.6 to 0.7).
- **G3, persistence (both beds, program posture).** The walk-share persistence ratio at 5 and 20
  epochs at most 1.2, and at most the control's minus 0.25.
- **G4, consistency (both beds, program posture).** Card PVN's NIS mean within 0.5 to 2 on the sim
  and not below half the control's on the field; card FXA's increment lag-1 correlation at most the
  control's; on the sim, the clock-level grade (`ctugrade.py`, the datum-free level rms at the run's
  end) no worse than the control's by more than 2 cm.
- **G5, consumer (both beds, golden posture).** The held-out consumer p50 in hours 1 and 2 no worse
  than the control by more than 3 cm. Adoption (golden, ledger) is a separate decision on 6 h pairs
  and the multi-site census, not this card's.

## 3.1 C0 read (2026-10-01, exe `_lgm0`, 2 h, the sim from 00:30:15)

| reading | sim control | sim C0 | field control | field C0 |
| --- | --- | --- | --- | --- |
| post-fit IF phase acf(1), tropical / mid / high (golden) | 0.73 / 0.68 / 0.66 | 0.68 / 0.61 / 0.58 | 0.60 / 0.56 / 0.53 | 0.67 / 0.61 / 0.58 |
| post-fit IF phase rms, tropical (golden) | 7.50 mm | 9.13 mm | 4.66 mm | 5.59 mm |
| walk-share persistence at 5 / 20 / 60 / 120 epochs (program) | 1.56 / 1.77 / 1.61 / 1.28 | 1.22 / 1.39 / 1.27 / 1.11 | 1.48 / 1.73 / 1.89 / 2.02 | 1.64 / 2.20 / 2.70 / 2.93 |
| rms one-epoch walk-share step; median walk share at 1 h (program) | 0.366 mm; 3.0 mm | 0.318 mm; 2.1 mm | 0.530 mm; 6.1 mm | 0.269 mm; 3.6 mm |
| PVN NIS mean; within-arc NEES (program) | 0.82; 0.3 | 0.74; 0.3 | 0.12; 0.5 | 0.10; 0.5 |
| FXA increment NIS; lag-1 correlation, ignition clones (program) | 0.877; +0.102 | 0.833; +0.095 | 0.350; +0.414 | 0.340; +0.425 |
| consumer p50 hours 1 / 2 (golden) | 0.734 / 0.084 m | 0.846 / 0.120 m | 0.559 / 0.059 m | 0.525 / 0.170 m |
| consumer p50 hours 1 / 2 (program) | 0.232 / 0.098 m | 0.221 / 0.107 m | 0.116 / 0.020 m | 0.101 / 0.051 m |

- **C0-1 fails narrowly, in opposite directions.** The lag-1 autocorrelation moves by -0.05 to
  -0.08 on the sim and by +0.05 to +0.07 on the field; the residual stays 0.5 to 0.7 correlated at
  one epoch on both beds. Inflation does not whiten it, as predicted; the size of the move was
  under-predicted.
- **C0-2 fails, in opposite directions.** The sim's persistence falls by 0.34 and 0.38 at 5 and 20
  epochs; the field's rises by 0.16 and 0.47 and by 0.8 to 0.9 at 60 and 120. On both beds the
  steps shrink (sim by 13 %, field by half) and the walk share at an hour shrinks with them. A
  lower phase weight changes how the estimator filters the coloured error, and the two beds'
  errors are filtered differently at hour scales: the field's long-lag persistence is not the
  sim's colour alone (card SFR section 5.2 left a third of the 2 h excess unexplained).
- **C0-3 holds.** NIS falls on both beds; the revision increments' lag-1 correlation moves by less
  than 0.01. The field's increment correlation (+0.41 in the ignition clones, +0.72 after) is
  card FXA's constellation-common drift; the sim's (+0.10) is near white although the sim now
  carries the colour, so this instrument does not read the per-satellite colour.
- **C0-4 fails.** Hour 2 is worse on both beds and both postures (golden +3.6 cm sim, +11.1 cm
  field; program +0.9 cm sim, +3.1 cm field).

Reading: the effective-sample-size inflation is not a substitute for the state. It costs the
consumer in hour 2 on both beds, it does not whiten the residual, and its effect on the leak's
persistence has opposite signs on the two beds. The state of section 2 remains the lever. The gate
set is revised before the build in one respect: the increment lag correlation (G4, second clause)
does not see the colour on the sim and is kept only as a no-harm check.

## 4.1 The design (fixed before code, 2026-10-01)

From a read of the ambiguity carry's lifecycle on the golden posture. The float vehicle resolves
`phase_leveling_local_block` on, so each station's carried ambiguities, and the receiver code bias on
the program posture, already travel as one jointly whitened block with their harvested conditional
cross covariances. The state joins that block, so its strong posterior correlation with the same line
of sight's ambiguities (the two share the phase rows) is carried, not dropped.

**Knobs** (default off; off is byte-identical): `network.phase_colour_state`;
`network.phase_colour_sigma_zen_m` (0.005), `network.phase_colour_tau_s` (300),
`network.phase_colour_elev_floor_deg` (20), `network.phase_colour_max_gap_s` (900);
`network.trace_phase_colour` (the per-line-of-sight trace `network_phase_colour.csv`). The loader
refuses the state without the network mode, the float vehicle, `phase_leveling` and
`carry_conditional_coupling`, and with `carry_coupling_joint_whiten` or `carry_invariant`.

**The column.** Each station's `StateManager` gains a per-satellite map (label `PCOL:<sat>`),
allocated in `MeasurementModel::ensureRequiredStates` for every satellite with an uncombined phase
observation, seeded 0 and pruned when the satellite leaves the station. `buildJacobianRow` puts +1 on
every phase row of the satellite and nothing on code rows. The network never writes the solution back
into the station's state, so the linearization point stays 0 and the carried value is the estimate
itself. `networkSplitStation_` routes the column into the local block without change.

**The kind.** The registry's first new entry, `NetworkParamKind::PhaseColour`: q rule Gauss-Markov
(sigma and tau), route local block, birth variance sigma(e)^2.

**Inject (PASS B).**

1. *Birth row.* A column with no live carry this epoch gets one pseudo-observation row (x = 0 at
   weight 1 / sigma(e)^2) with the station's pin rows, before the coupling rows: the station block is
   positive definite before its test, and the row survives the within-epoch RAIM replay (which re-adds
   the assembled rows but not the pre-test diagonal priors). A live carry is an entry within the
   maximum gap on a station-epoch where the leveling carry injects.
2. *Carried.* A predicted copy of the entry (the value and every border coefficient times phi,
   phi = exp(-gap / tau)) enters the station's whitened block with variance
   phi^2 var + sigma(e)^2 (1 - phi^2); its cross terms with the other block columns are scaled by phi
   (a new `LocalBlockCol::phi`, default 1, so every existing column is unchanged). The block's
   per-column fallback emits the same predicted copy. A planned carried column that the builder
   refuses gets a birth row as a backstop (counted).

**Harvest.** The columns join the station's harvest block after the receiver code bias and before the
wet delay (which keeps its last-row slot), and are stashed in a new map keyed (station, satellite)
with the estimate, the conditional variance, the border regression and the block row.

**Responses.** No slip response: the state is not an arc quantity, and an ambiguity's drop already
zeroes their cross terms (counted as missing pairs). The RAIM attribution erase and the gross
line-of-sight erase also erase the line of sight's entry, so a faulty line of sight re-starts from its
birth prior. A gap beyond the maximum prunes the entry. Every edge is counted: births, carried folds
(block and fallback), backstop births, erases by cause, prunes, missing pairs; one run-end line.

**Invariants.** #1: the state is mean-reverting with q > 0, not stiff. #7: the decoupling edge is the
birth after a gap or an erase, bounded at the stationary variance; the re-coupling edge is the birth
prior itself.

**Instruments.** `network_phase_colour.csv` (epoch, station, satellite, elevation, estimate, its
standard deviation, born or carried) for gate G1 against `truth_phase_colour.csv`; the post-fit rows
already include the column (`phase_acf.py` reads them unchanged).

## 5.1 The first read (2026-10-01, exe `gnss_processor_lgm1.exe`, 2 h, the sim from 00:30:15)

Built as section 4.1 fixed it (default off). Arms `runs/_rap_lgm_pc_{gs,gf,ps,pf}`: each C0 control
(section 3) with `network.phase_colour_state` and `network.trace_phase_colour` and nothing else.

**G0 passes.** Knob off, `scripts/prm_ab.py cmp g0a lgm1off --all-csv`: all 145 comparisons
identical on the four arms. Unit tests `tests/test_phase_colour_state.cpp` (the state family, the
loader's acceptance on the sim golden and its refusals, the prior law, the registry's new kind) and
`tests/test_network_param_registry.cpp` pass.

**The state is live** on every arm: about 111 000 (field) and 114 000 (sim) carried folds through
the whitened block against 3 600 to 3 700 birth rows, no per-column fallback, no backstop birth row,
no harvest refusal; the field's RAIM attribution erased 12 (program) and 17 (golden) entries; 186 to
192 entries pruned past the maximum gap.

| gate | sim control | sim state | field control | field state |
| --- | --- | --- | --- | --- |
| G1: rms(x - e) / rms(e), tropical / mid / high (golden) | | 0.885 / 0.908 / 0.908 | | (no truth) |
| G1: correlation of x with e; the estimate's rms against the truth's (tropical) | | +0.51; 3.6 against 12.0 mm | | |
| G2: post-fit acf(1), tropical / mid / high (golden) | 0.73 / 0.68 / 0.66 | 0.58 / 0.54 / 0.53 | 0.60 / 0.56 / 0.53 | 0.43 / 0.36 / 0.36 |
| G3: persistence at 5 / 20 / 60 / 120 epochs (program) | 1.56 / 1.77 / 1.61 / 1.28 | 1.31 / 1.33 / 1.13 / 0.95 | 1.48 / 1.73 / 1.89 / 2.02 | 1.30 / 1.39 / 1.60 / 1.73 |
| walk share at 1 h (program) | 3.0 mm | 2.1 mm | 6.1 mm | 4.3 mm |
| G4: PVN NIS; FXA increment lag-1 (program) | 0.82; +0.102 | 0.79; +0.101 | 0.12; +0.414 | 0.12; +0.400 |
| G4: sim datum-free level at 240 epochs; IF code spread, hour 2 (golden) | 0.16 m; 0.143 m | 0.16 m; 0.142 m | | |
| G5: consumer p50 hours 1 / 2 (golden) | 0.734 / 0.084 m | 0.727 / 0.074 m | 0.559 / 0.059 m | 0.440 / 0.049 m |
| G5: consumer p50 hours 1 / 2 (program) | 0.232 / 0.098 m | 0.232 / 0.096 m | 0.116 / 0.020 m | 0.100 / 0.030 m |

G0, G4 and G5 pass; G1, G2 and G3 fail. The state moves every instrument in the right direction on
both beds (C0 moved the persistence in opposite directions), but by too little: G3 misses its 1.2 bar
by 0.1 to 0.2, G2 its 0.35 bar by 0.01 to 0.23, and G1 shows why. The estimate correlates with the
truth (+0.46 to +0.57) and its stated standard deviation is consistent with its error (NEES 0.7 to
1.35), but its amplitude is a quarter of the truth's (3.6 against 12.0 mm in the tropical class):
the state is starved, not wrong.

**The reason is the posture's white phase sigma.** The estimator states 2 mm per band on the field
and 3 mm on the sim, about 6 and 9 mm on the ionosphere-free combination: a size that covered the
colour as if it were white (the sim's actual white noise is 0.5 mm per band since card SFR, and the
field's post-fit white part is 1.1 to 1.6 mm on the combination, about 0.4 to 0.5 mm per band). The
state's process noise per epoch is sigma^2 (1 - phi^2), about (2.1 mm)^2 at 300 s and 30 s epochs, so
against a stated (6 to 9 mm)^2 the per-epoch gain on its innovation is 0.05 to 0.1 and the estimate
cannot follow the colour. With the colour modelled, the white sigma has to be the white part only;
the state and the white sigma are one model, and section 2 named the first without the second.

## 5.2 The white sigma with the state (predicted before it runs, 2026-10-01)

Two one-change arms per posture and bed from the section 5.1 arms, exe `_lgm1`:

- `lgm_pcw_*`: the state with `filter.measurement_noise.phase_std_m` 0.0005 (the sim fixture's white
  phase noise, the field's post-fit white part per band), elevation power unchanged.
- `lgm_w_*`: the same sigma without the state (the control of the sigma cut alone).

Predictions:

- **W1.** On `lgm_pcw`, G1 passes (ratio at most 0.7 in every class, correlation at least +0.7) and G2
  passes on both beds (acf(1) at most 0.35).
- **W2.** On `lgm_pcw`, G3 passes on both beds (persistence at 5 and 20 epochs at most 1.2) and G4 holds.
- **W3.** On `lgm_w` (the sigma cut with the colour unmodelled) the estimator is overconfident: the
  persistence at 5 and 20 epochs rises above its control's on both beds and the consumer's hour 2 is
  worse than its control's by more than 3 cm on at least one bed.
- **W4.** On `lgm_pcw`, G5 holds (hours 1 and 2 within 3 cm of the control or better, both beds, both
  postures).

If W1 and W2 hold and W3 holds, the white sigma and the state are adopted as one change for the 6 h
pair and the census (a separate decision). If W3 fails (the sigma cut alone is as good), the state
is not the lever and the sigma is.

## 5.3 The white sigma read, and the bottleneck it exposes (2026-10-01)

Arms `runs/_rap_lgm_pcw_*` (the state, white phase sigma 0.0005 m) and `runs/_rap_lgm_w_*` (the same
sigma without the state), exe `_lgm1`, 2 h.

| reading | sim control | sim state | sim state + 0.5 mm | sim 0.5 mm alone | field control | field state | field state + 0.5 mm | field 0.5 mm alone |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| emitted epochs (golden) | 241 | 241 | 241 | **0** | 240 | 240 | 240 | 187 |
| post-fit acf(1), tropical (golden) | 0.73 | 0.58 | **+0.02** | (none) | 0.60 | 0.43 | **+0.02** | 0.28 |
| G1 ratio, tropical (golden) | | 0.885 | 0.870 | | | | | |
| persistence 5 / 20 epochs (program) | 1.56 / 1.77 | 1.31 / 1.33 | 0.96 / 0.78 | 1.36 / 1.27 | 1.48 / 1.73 | 1.30 / 1.39 | 1.13 / 1.28 | 1.17 / 1.15 |
| rms walk-share step (program) | 0.37 mm | 0.35 mm | 0.71 mm | 1.14 mm | 0.53 mm | 0.43 mm | 1.36 mm | 1.56 mm |
| consumer p50 h1 / h2 (golden) | 0.734 / 0.084 | 0.727 / 0.074 | 0.905 / 0.177 | (none) | 0.559 / 0.059 | 0.440 / 0.049 | 0.563 / 0.078 | 0.445 / 0.067 |
| consumer p50 h1 / h2 (program) | 0.232 / 0.098 | 0.232 / 0.096 | 0.372 / 0.115 | 0.227 / 0.145 | 0.116 / 0.020 | 0.100 / 0.030 | 0.138 / 0.023 | 0.094 / 0.034 |

- **W1 half holds.** With the state and the white sigma at the white level, the residual is white on
  both beds (acf(1) -0.06 to +0.02 in every class, rms about 1 mm), so G2 passes. G1 still fails
  (0.85 to 0.93).
- **W2 fails.** The persistence falls below 1 on the sim and the steps double on both beds: the
  estimator now over-corrects at the epoch scale.
- **W3 half holds.** The sigma cut without the state withholds every sim golden epoch (the rank gate)
  and 53 of 240 on the field, and costs the program sim's hour 2 4.7 cm; but its persistence falls
  rather than rises.
- **W4 fails.** With the state, the cut costs the sim golden 9.3 cm in hour 2 and the program sim
  14 cm in hour 1.

**Why the state cannot carry the colour.** Three reads, the last one decisive:

1. On the sim the estimate is aligned with the truth (correlation peaks at lag 0) but holds 20 % of
   its amplitude (slope 0.20) and loses its correlation with the truth within 4 epochs: it carries
   the colour's fast part only.
2. Traced with `network.trace_amb_state` (1.5 h sim arms `_rap_lgm_{ctl,pc,pcw}amb_gs`,
   `_rap_lgm_{ctl,pc}amb_ps`), the carried ionosphere-free ambiguity of a line of sight, with each
   epoch's station-common and satellite-common parts removed (a two-way median polish), moves by
   6.6 mm per epoch on the golden posture and 3.3 mm on the program posture, with or without the state;
   the colour itself moves 4.4 mm per epoch. The motion decays with the arc's age (program posture:
   12.7 mm at 5 to 10 epochs, 3.2 mm at 40 to 80, 1.6 mm at 160 to 400). A float ambiguity held at
   phase precision would move 20 to 40 times less. Half of the golden posture's motion goes away with
   card CTU's exact coupling update (the scalar update ages the clock term at the legacy 1e-3 m2/s).
3. A one-line-of-sight filter with the fixture's own model (colour 300 s, white 0.15 of the colour's
   sigma) reaches 0.74, 0.54, 0.42 and 0.34 at 20, 60, 120 and 200 epochs, its stated sd falling with
   it; give its ambiguity a random walk of 3.5 mm per epoch and the ratio stays at 0.78 to 0.80 and
   the stated sd at 0.88 of the prior at every age. That is the network's signature exactly (the
   state's stated sd stays at 0.87 to 1.0 of its prior at every age).

The colour state is consistent and correctly wired; what it cannot do is separate the colour from an
ambiguity that is itself loose. The float vehicle's carried ambiguity loses information every epoch,
more on the golden posture than on the program posture and most in the first half hour of an arc.
That loss caps any per-line-of-sight model, and it is also a candidate share of the walk itself.

**Disposition.** No arm passes the pre-registered gate set; nothing is armed. The state with the
posture's white sigma (section 5.1) is the only no-harm variant (G0, G4 and G5 pass, the field golden
consumer's hour 1 0.559 to 0.440 m, the persistence down on both beds) and stays default off. The card
stays open behind a new card, AIL (the float vehicle's ambiguity information loss), which is
pre-registered next: until the carried ambiguity is phase-stiff on mature arcs, the state's gates are
not reachable on either posture.
