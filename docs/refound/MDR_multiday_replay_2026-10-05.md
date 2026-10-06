# MDR: the field golden replayed on four more days (DOY 128, 130, 131 and 132 of 2024)

**STATUS**

- **id:** MDR
- **topic:** validation
- **status:** open
- **summary:** Every field number of the re-founding program came from one day, DOY 129. The field golden, as generated at HEAD (`3959f24`), is replayed for 6 h on DOY 128, 130, 131 and 132, and on DOY 131 from 18 UT (the G5 storm's onset), and graded on the WTZZ00DEU consumer by hour and the 16-site consumer census, against DOY 129 on the same executable. Hours 3 to 6 transfer: census medians 0.037 to 0.066 m on every day against 0.038 to 0.049 m on DOY 129, the in-storm window (DOY 132, 00 to 06 UT) included. Three findings: DOY 128's first two hours are bad network-wide (a data suspect: no DOY 127 products; CONFIRMED 2026-10-06, §4: with them DOY 128 is census 0.283 0.058 0.045 0.045 0.034 0.037 m); ARHT00ATA is the worst census site on four of five days and diverges to 166 m on DOY 131; the WTZZ consumer drifts to 0.08 m in hours 4 to 6 of DOY 130 while the census does not.
- **source:** the author's direction of 2026-10-03 (multi-day testing before the clock-level theory); [LAH](LAH_level_aware_hold_2026-09-29.md) (the census tool and the named regressions ARHT00ATA, KOKB00USA)
- **knobs / posture:** none; the field golden `config/golden/golden_field_refound.json` moved to each day by `scripts/mday_golden_config.py`
- **hashes / run dirs:** exe `gnss_processor_prm3d.exe` `80db53d9733d3f2f` (main `3959f24`); wires `runs/_rap_mday{128,130,131,132}_f6`, `runs/_rap_mday131late_f6`, DOY 129 `runs/_rap_prm_s3ah6_gf`; census replays `runs/_rap_mdc_<doy>_<site>`; the DOY 128 rerun with DOY 127 products `runs/_rap_mday128p_f6`, census `runs/_rap_mdcp_128_<site>` (2026-10-06, same exe)
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

## 4. 2026-10-06: DOY 128 rerun with DOY 127's products

`PathResolver` reads SP3 and CLK from the previous, the observation and the next day's `products/`
(`src/core/products/PathResolver.cpp`, the 3-day window), so the DOY 128 wire had loaded two days
(69696 orbits) where DOY 129 and 130 loaded three (104544). CODE's MGEX final SP3 and CLK for DOY 127
(`COD0MGXFIN_20241270000_01D_05M_ORB.SP3`, `_30S_CLK.CLK`, the same CCRNXC V5.5 run family as DOY 128)
were fetched from the CDDIS product archive (week 2313) into `data/2024-127/products/`, and DOY 128 was
rerun on the same executable from the original arm, the product output path the only difference
(`runs/_rap_mday128p_f6`; census replays `runs/_rap_mdcp_128_<site>`, `mday_census.py --prefix mdcp`).

| DOY 128 | first-epoch clock-Δ rms | WTZZ h1 to h6 | census h1 to h6 | worst census sites (max p50, h2 to h6) |
| --- | --- | --- | --- | --- |
| two-day products (§2) | 4.152 m | 1.336 0.155 0.027 0.029 0.038 0.021 | 1.498 0.207 0.051 0.037 0.038 0.040 | ARHT 4.50, WUH2 1.84 |
| with DOY 127 | 0.805 m | 0.080 0.023 0.014 0.014 0.024 0.016 | 0.283 0.058 0.045 0.045 0.034 0.037 | ARHT 54.09, WUH2 0.26 |

The hypothesis of §3 holds: the DOY 128 hours 1 and 2 were the missing pre-midnight interpolation
nodes, not the posture. With them the day's hour-1 census (0.283 m) sits with DOY 130 to 132 (0.21 to
0.29 m) and its hour 2 (0.058 m) is the best of the five days. RAIM attributions fall 68 to 38 and
clock-anchor events 196 to 166; IF-phase slips (47) are unchanged.

ARHT00ATA improves in hours 1 to 4 (1.98 0.59 0.27 0.28 m, against 18.4 4.5 2.2 1.1 m) and then jumps
to 99.7 m at 03:36 UT (54.1 and 28.4 m p50 in hours 5 and 6): the same consumer divergence as on
DOY 131, now on a second day. Separately, the wire's emitted clock-Δ rms reads 12 to 13 m at the
sampled epochs 480 and 540 (03:59 and 04:29 UT) and 11 to 12 m at 720, against 0.8 to 1.9 m elsewhere,
in both DOY 128 runs alike: a satellite-level clock departure on this day that the product margin does
not touch. Whether it is what ARHT's consumer fails on is the ARHT read, still owed.

**Correction to §1.** "300 station-days, none failed" is wrong. Three station-days (COCO00AUS on
DOY 128, LCK400IND and MAC100AUS on DOY 132) are absent from the CDDIS daily archive (HTTP 404), and
`fetch_global_obs.py` saved the archive's HTML error page under the `.rnx` name: `curl` ran without
`-f` and the Hatanaka decompressor passes a non-gzip file through unchanged. The producer rejected
those files ("Failed to parse RINEX"), so those wires ran on 56 stations. MAC100AUS on DOY 128 is a
real file but its receiver logged no satellites before 10:30 UT and the parser rejects it, so DOY 128
also ran on 56. Every graded number above stands, being what the producer actually ran. The script
now fetches with `curl -f`, requires a RINEX header line and moves a non-RINEX file aside as
`*.not_rinex.html`; the three pages on D: are renamed that way.
