# LSR: the long-scale remainder of the field's walk-share persistence

**STATUS**

- **id:** LSR
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (the outside review's note after cards FWS and SOI). With the wind-up staircase removed, the field's per-satellite walk share stays persistent at an hour and two hours (ratio 1.54 and 1.53 against the sim's 0.68 and 0.54); card SOI ruled out the second-order ionospheric term. Two candidates are left: station-set changes as satellites rise and set (a new line of sight's ambiguity converging drags a thinly observed satellite's clock), and daytime tropospheric variability at the tropical stations that the estimator's wet-delay random walk (`troposphere_q` 1e-9 m2/s, 1.9 mm per root hour) cannot follow, whose station-common, elevation-mapped misfit leaks into thinly observed satellites. Two trace reads and two one-change 6 h field arms, predicted before they run.
- **source:** [FWS](FWS_field_walk_share_sources_2026-10-01.md) sections 2.1, 4 and 5; [SOI](SOI_second_order_ionosphere_leak_2026-10-01.md); the sim fixture's `runs/simnet60_quiet_wu93_24h/network_fixture.json` (its wet delay)
- **knobs / posture:** arms only, from `runs/_rap_fws_wuacc6_f` (field 6 h, accumulated wind-up with its restart response): `filter.process_noise.troposphere_q`, `estimation.estimate_tropo_gradients`; nothing armed
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** exe `gnss_processor_rev1.exe` `a59231c8802a14f8` (the base's); arms `runs/_rap_lsr_*`; scripts `scripts/clock_level/wsf_persist.py` (lag 120 added), `scripts/clock_level/lsr_reads.py`

## 1. The remainder

`wsf_persist.py` (ratio at 5 / 20 / 60 / 120 epochs; rms one-epoch step):

| run | ratio | step |
| --- | --- | --- |
| field 6 h control `rev1_blk6_f` | 1.54 / 1.83 / 1.98 / 1.84 | 0.59 mm |
| field 6 h, accumulated wind-up `fws_wuacc6_f` (the base) | 1.45 / 1.51 / 1.54 / 1.53 | 0.54 mm |
| sim 6 h `rev1_blk6_s` | 1.05 / 0.93 / 0.68 / 0.54 | 0.87 mm |

The sim is not a troposphere-free null: its fixture draws each station's wet delay as a 1 cm,
1 h Gauss-Markov process (`zwd_rw_sigma_m` 0.01, `zwd_rw_tau_s` 3600), whose short-term rate
(2 sigma^2 / tau = 5.6e-8 m2/s) is 56 times the estimator's random walk. It bounds what a 1 cm,
1 h troposphere does at the sim's coverage (about 15 stations per satellite).

## 2. The trace reads

On the base and the sim (`lsr_reads.py`), over block-clone rows of age at least 30 minutes:

- **R1, the troposphere's signature.** Per station and epoch, the slope c of the ionosphere-free
  post-fit phase residuals on the wet mapping function (1 / sin e) less the station's epoch mean,
  in zenith metres: the misfit the wet-delay state did not follow, after the receiver clock took the
  common part. Its 20-epoch block rms by station class (tropical, |latitude| below 23.5 degrees,
  and the rest). Then T2's form (card SOI): the satellite-mean, over the lines of sight present at
  both ends, of the change of c times (1 / sin e - the station's mean), against the walk share's
  change at 60 and 120 epochs, both de-commoned per epoch.
- **R2, station-set changes.** Per clone window of 60 and 120 epochs, the number of stations that
  start or stop observing the satellite inside the window (`network_ionosphere.csv` station sets);
  the walk share's squared change regressed on that count (the intercept is the variance with no
  change, the slope the variance per change), on both beds.

## 3. The arms

One change each, 6 h, the base's exe, against the base:

| arm | change |
| --- | --- |
| `lsr_trq_f` | `filter.process_noise.troposphere_q` 1e-9 -> 3e-8 m2/s (about 1 cm per root hour) |
| `lsr_grad_f` | `estimation.estimate_tropo_gradients` true (the default gradient q) |

## 4. Predictions, written before any number was computed

If the tropical troposphere carries the remainder:

- **L1 (R1).** The block rms of c at the field's tropical stations is at least 1.5 times the
  non-tropical stations' and at least 1.5 times the sim's; the R1 satellite-level regression has r
  at most -0.2 at 120 epochs on the field and |r| below 0.1 on the sim.
- **L2 (the arms).** `lsr_trq_f` or `lsr_grad_f` lowers the base's ratio at 60 and 120 epochs by at
  least a quarter of its excess over 1 (to at most 1.40 and 1.40), and `lsr_trq_f` lowers the
  tropical stations' c block rms by at least a third.

If the station-set changes carry it:

- **L3 (R2).** On the field the variance per change is at least twice the sim's, and the no-change
  intercept is within 1.5 times the sim's at 120 epochs (the excess sits in the windows with
  changes).

The arms' consumer effect is not read here (the phase-clock hold does not pass the walk share; card
FWS's census of the wind-up arm was flat to the millimetre). If neither L1 with L2 nor L3 holds, the
remainder is not either candidate, and the per-line-of-sight nuisance state of card SOI section 5 is
the estimator-side response, as registry work.
