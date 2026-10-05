# MDR: the field golden replayed on four more days (DOY 128, 130, 131 and 132 of 2024)

**STATUS**

- **id:** MDR
- **topic:** validation
- **status:** open
- **summary:** Every field number of the re-founding program came from one day, DOY 129. The field golden, as generated at HEAD (`3959f24`), is replayed for 6 h on DOY 128, 130, 131 and 132, and on DOY 131 from 18 UT (the G5 storm's onset), and graded on the WTZZ00DEU consumer by hour and the 16-site consumer census, against DOY 129 on the same executable. Hours 3 to 6 transfer: census medians 0.037 to 0.066 m on every day against 0.038 to 0.049 m on DOY 129, the in-storm window (DOY 132, 00 to 06 UT) included. Three findings: DOY 128's first two hours are bad network-wide (a data suspect: no DOY 127 products); ARHT00ATA is the worst census site on four of five days and diverges to 166 m on DOY 131; the WTZZ consumer drifts to 0.08 m in hours 4 to 6 of DOY 130 while the census does not.
- **source:** the author's direction of 2026-10-03 (multi-day testing before the clock-level theory); [LAH](LAH_level_aware_hold_2026-09-29.md) (the census tool and the named regressions ARHT00ATA, KOKB00USA)
- **knobs / posture:** none; the field golden `config/golden/golden_field_refound.json` moved to each day by `scripts/mday_golden_config.py`
- **hashes / run dirs:** exe `gnss_processor_prm3d.exe` `80db53d9733d3f2f` (main `3959f24`); wires `runs/_rap_mday{128,130,131,132}_f6`, `runs/_rap_mday131late_f6`, DOY 129 `runs/_rap_prm_s3ah6_gf`; census replays `runs/_rap_mdc_<doy>_<site>`
- **dates in the text:** 2026-10-05

## 1. The data

Observations for the 58 roster stations, the WTZZ00DEU consumer and the 16 census sites on each day were
fetched from the CDDIS daily archive (`scripts/fetch_global_obs.py`, its new `--root`): 300 station-days,
none failed, in `D:/GnssProcessorSupport/data_mday/<YYYY-DOY>/obs/` (11 GB; the runs were made with the
roster on `C:/GnssProcessor/data` and the records repointed after the move). DOY 128 and 130 lacked the
broadcast navigation, CODE's MGEX bias and the GIM, now in `data/<YYYY-DOY>/products/`; CODE's MGEX ERP
is not archived under those days, so both use the DOY 129 file, a 3-day solution covering MJD 60437.0 to
60439.5 (DOY 128 00 UT to DOY 130 12 UT). DOY 127 (the pre-midnight interpolation margin of DOY 128) is
not on disk.

## 2. The grades

WTZZ00DEU p50 of the 3-D consumer error by hour, and the census (median over the 16 held-out sites of the
site p50), m:

| day | window (UT) | WTZZ h1 to h6 | census h1 to h6 | worst census sites (max p50, h2 to h6) |
| --- | --- | --- | --- | --- |
| 129 (reference) | 00 to 06 | 0.108 0.024 0.016 0.020 0.030 0.026 | 0.415 0.110 0.042 0.049 0.038 0.044 | KOKB 1.48, ARHT 0.84 |
| 128 | 00 to 06 | 1.336 0.155 0.027 0.029 0.038 0.021 | 1.498 0.207 0.051 0.037 0.038 0.040 | ARHT 4.50, WUH2 1.84 |
| 130 | 00 to 06 | 0.062 0.025 0.027 0.054 0.079 0.071 | 0.290 0.094 0.053 0.049 0.047 0.045 | WUH2 1.49, ARHT 0.44 |
| 131 | 00 to 06 (before the storm) | 0.090 0.028 0.019 0.030 0.040 0.030 | 0.216 0.065 0.057 0.056 0.047 0.047 | ARHT 166.22, BAKE 0.32 |
| 131 | 18 to 24 (the storm's onset) | 0.09 0.04 0.02 0.02 0.02 0.01 | not run | |
| 132 | 00 to 06 (in the storm) | 0.089 0.026 0.022 0.017 0.027 0.015 | 0.211 0.067 0.066 0.051 0.058 0.030 | ARHT 0.61, WUH2 0.55 |

The storm shows in the slip responses, not in these consumers: on DOY 131 from 18 UT the producer made
145 RAIM attributions and 405 IF-phase slip flags in 6 h, against 18 to 68 and 47 to 220 on the quiet
windows. The census sites are mid- and low-latitude except the two Antarctic ones; a high-latitude
storm grade needs a site selection of its own.

## 3. Findings (owed reads)

- **DOY 128, hours 1 and 2.** Network-wide: the census and WTZZ both, the producer's first-epoch clock
  spread 4.15 m against 0.88 to 0.96 m on DOY 129 and 130. DOY 127's orbit and clock products (the
  pre-midnight interpolation nodes) are absent; the data is the first suspect, not the posture. The test:
  fetch DOY 127's SP3 and CLK and rerun.
- **ARHT00ATA.** The worst census site on four of five days (card LAH's named regression), and 166 m on
  DOY 131: a consumer divergence, not a degradation. KOKB00USA, the other named site, is bad only on
  DOY 129.
- **WTZZ00DEU on DOY 130.** 0.054 to 0.079 m in hours 4 to 6 while the census median holds at 0.045 to
  0.049 m: a site-and-day event at the consumer the goldens grade on.
- **Hour 1** on the other days (census 0.21 to 0.29 m) is better than DOY 129's 0.415 m; the reference
  day's ignition is not typical.
