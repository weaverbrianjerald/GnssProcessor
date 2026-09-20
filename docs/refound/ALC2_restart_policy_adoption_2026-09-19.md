# ALC2: the level-check restart policy refused, and the translate/level-check pair adopted into the working bases

**STATUS**

- **id:** ALC2
- **topic:** consumer
- **status:** closed
- **summary:** `amb_level_check_restart: inconsistent` is REFUSED on both beds and its branch stays unmerged as the record; `pb_step_translate`, `amb_level_check` and `trace_consumer_sat` are ADOPTED into the two RF-0hv working bases; the pair is re-based on post-ALC-ARC `main` and reproduces the reference pair byte for byte
- **source:** [ALC](ALC_amb_level_check_2026-09-15.md) section 7 item 2, [ALC-ARC](ALC-ARC_reset_generation_2026-09-16.md), [ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md), [ALC-BAND](ALC-BAND_window_scope_review_2026-09-17.md); author's adoption decision
- **knobs / posture:** ADOPTED into `config/refound/{field,sim}_gps_dual_rf0hv.json`: `network.closed_loop.pb_step_translate`, `network.closed_loop.amb_level_check`, `network.trace_consumer_sat`. REFUSED and not set anywhere: `network.closed_loop.amb_level_check_restart` (stays at the loader default `all`). No loader default, no golden and no ledger row changed.
- **dates in the text:** 2026-09-19
- **hashes / run dirs:** new pair `runs/_rf0hv2_field6` and `runs/_rf0hv2_sim6`, exe `build/x64-Release/gnss_processor_rf0hv2.exe` sha256 `A3D62A242C04E545AA2ACD8C3D0747695821A69C216D2BA6D02B1B019C15A9A7`, source `main` `e437b6b`. `products.txt` sha256 FIELD `EDAF96CEDB1262B20CA4FFA57C3CCA1F6E89DCC8B8AB1396AF7073596D660484`, SIM `2149CABD1192E61062FA4AFAAB1A6D6D2995456B32437A6841B05A681DA7F12B`. Refused arms `runs/_alc2_field6` / `runs/_alc2_sim6`, exe `gnss_processor_alc2.exe` sha256 `910122BBBA1EE7C93E3FED6BD4DA4783ED35EDF7F9BA4669FC23E457D2CA443D`. Reference pair `runs/_alc_field6` / `runs/_alc_sim6`, exe `gnss_processor_alc.exe` sha256 `87923C21E731DDC8572D8DF746C2D7CC2089FCD2CB4ECBEF864DA02F383C86B4`. Every `runs/<name>` above resolves under `D:\GnssProcessorSupport\runs\` except the two new ones, which are in the working tree.

## 1. The ALC2 refusal record

The knob was `network.closed_loop.amb_level_check_restart`, an enumerated policy with the
loader default `all` (restart the level-check window on every ambiguity write, which is
[ALC](ALC_amb_level_check_2026-09-15.md) section 1.3's guarantee) and one alternative,
`inconsistent`, which restarts only on a write judged inconsistent with the window already
held. Its motivation was ALC section 7 item 2: the check reaches a full window on only 20 %
of satellite-epochs, so most of the sky is never tested.

The implementation lives on branch `wip/alc2-restart-policy` at `f2b0325`, merge-base
`6f2a5d3`. **That branch is deliberately kept UNMERGED as the record.** Its code conflicts
with the later ALC-ARC work now on `main`, and the knob it adds is refused, so merging it
would import a refused mechanism and a conflict at once. The configuration half of that
commit, and only that half, is adopted in section 2.

Arms `runs/_alc2_field6` (exit 0, wall 256.8 s) and `runs/_alc2_sim6` (exit 0, wall 202.8 s),
exe `gnss_processor_alc2.exe` sha256
`910122BBBA1EE7C93E3FED6BD4DA4783ED35EDF7F9BA4669FC23E457D2CA443D`. Both emit 720 of 720.
`products.txt` is byte-identical to the corresponding reference arm on both beds
(`EDAF96CEDB1262B2...` field, `2149CABD1192E610...` sim), so the refusal is entirely
consumer-side and no wire claim is involved.

Consumer 3D `pos_err_3d_m` p50 by hour, in METRES, re-derived for this card from
`logs/closed_loop_trace.csv` rather than copied:

| bed | arm | h1 | h2 | h3 | h4 | h5 | h6 | epochs < 0.10 m | overall p50 |
|---|---|---|---|---|---|---|---|---|---|
| FIELD | `_alc_field6` (reference) | 0.4312 | 0.1629 | 0.3097 | 0.4892 | 0.6806 | 0.4857 | 9 | 0.4444 |
| FIELD | `_alc2_field6` (refused) | **0.8609** | **0.5777** | 0.2937 | **0.7429** | **0.7329** | 0.4596 | **0** | 0.6096 |
| FIELD | delta | +0.4297 | +0.4148 | -0.0160 | +0.2537 | +0.0523 | -0.0261 | -9 | +0.1652 |
| SIM | `_alc_sim6` (reference) | 0.4600 | 0.2789 | 0.2463 | 0.1513 | 0.1603 | 0.1366 | 81 | 0.1858 |
| SIM | `_alc2_sim6` (refused) | **1.0169** | **0.9800** | **0.2938** | **0.3604** | **0.4708** | **0.2796** | **0** | 0.4713 |
| SIM | delta | +0.5569 | +0.7011 | +0.0475 | +0.2091 | +0.3105 | +0.1430 | -81 | +0.2855 |

p90 by hour, metres: field `3.11 / 0.73 / 0.33 / 0.92 / 0.77 / 0.60` against the reference's
`3.11 / 0.28 / 0.39 / 0.61 / 0.71 / 0.69`; sim `3.10 / 1.11 / 0.70 / 0.49 / 0.60 / 0.37`
against `3.19 / 0.32 / 0.28 / 0.21 / 0.23 / 0.20`. The field loses more than 0.10 m on three
of six hours and more than 0.05 m on four; the sim loses on all six. The sub-decimetre
population is wiped out on both beds.

The instrument moved in the advertised direction and the position went the other way:

| counter | field reference | field refused | sim reference | sim refused |
|---|---|---|---|---|
| samples | 5799 | 5799 | 5387 | 5387 |
| window restarts | 1159 | **188** | 1508 | **2** |
| full-window checks | 1183 | **1740** | 1626 | **3951** |
| coverage (checks / samples) | 0.204 | **0.300** | 0.302 | **0.733** |
| trips wl / if | 27 / 79 | **35 / 122** | 2 / 24 | **5 / 52** |
| refloats | 106 | **157** | 26 | **57** |
| engine ledger against the harness drain | AGREE | AGREE | AGREE | AGREE |

**The reading.** Coverage rises 1.5 times on the field and 2.4 times on the sim, and the
extra detections land almost entirely in the IF channel (+43 of +51 field, +28 of +31 sim),
the channel ALC section 7 item 5 already names as the weaker of the two. A window that
survives more writes also carries a longer history, and the scale the `k = 4` bar is applied
to is the sample scale estimated from that same history.
[ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md) section 2 establishes what that means:
under iid noise and inactive floors the single-channel ratio is a Student t(19) statistic,
stationary code correlation at phi = 0.5 raises the noise-only either-channel rejection rate
to 7.5084 % per independent window, and a persistent code bias produces exactly the same two
statistics as an ambiguity error. Keeping windows open longer therefore makes a weaker,
persistent code-level discrepancy significant at `k = 4` without identifying its cause, and
the response refloats state that was not wrong. ALC-STAT section 5 states the same conclusion
in advance: longer windows can make a persistent nuisance more significant while still
failing to identify it. The measured position cost is that prediction realised.

**A counter defect in the refused arm, recorded and not re-run.** The policy added a
restarts-by-cause ledger, verbatim from the run-end summaries:

```
FIELD  amb level restarts (ALC2) : policy 'inconsistent' -> n_amb_level_restarts_by_cause
       cb 188 / refloat 0 / reset_or_birth 0 / translate 0; sum 188 vs restarts 188 -> CLOSES
SIM    amb level restarts (ALC2) : policy 'inconsistent' -> n_amb_level_restarts_by_cause
       cb 0 / refloat 0 / reset_or_birth 2 / translate 0; sum 2 vs restarts 2 -> CLOSES
```

The same two runs report 157 and 57 refloats, and a trip clears the window in both modes, so
the refloat bucket cannot legitimately be zero. The closure `sum == restarts` holds only
because refloat-caused clears sit outside the population the restart total counts, which
means the ledger cannot attribute the very response the policy changes. Any future restart
policy needs that attribution fixed before its coverage number is readable.

**Superseded on both sides.** [ALC-ARC](ALC-ARC_reset_generation_2026-09-16.md) replaces the
restart question with explicit ambiguity generations, so the criterion is the actual lifecycle
event rather than a heuristic judgement of consistency, and it clears the window immediately
on an explicit notification including in epochs that produce no sample.
[ALC-BAND](ALC-BAND_window_scope_review_2026-09-17.md) refuses the other half of the same idea
for the same reason: both WL and IF samples depend on both selected arcs, so a narrower
restart scope invalidates histories it keeps, and paired windows are retained. Between them,
nothing in the refused knob survives as a live proposal.

## 2. The adoption

Three keys are adopted into `config/refound/field_gps_dual_rf0hv.json` and
`config/refound/sim_gps_dual_rf0hv.json`, the WORKING BASES from which every new vehicle arm
is copied:

| key | section | what it is |
|---|---|---|
| `network.closed_loop.pb_step_translate` | consumer | card PBT: the consumer translates every phase-bias product step it applies |
| `network.closed_loop.amb_level_check` | consumer | card ALC: the consumer re-validates its float ambiguities against their own code level and refloats at `k = 4` |
| `network.trace_consumer_sat` | trace | card CST: the per-satellite closed-loop consumer trace |

All three are consumer-side or trace-only. `products.txt`, `network_epoch_status.csv` and
`network_dd_fractional.csv` are byte-identical with and without them, so the wire and every
solve artefact are untouched, which is why the adoption is legal on a working base without a
golden re-base.

What is NOT adopted, explicitly: `network.closed_loop.amb_level_check_restart` is absent as a
JSON key from both files and stays at the loader default `all`. The goldens in
`config/golden/` are untouched, the loader defaults are untouched, and
`docs/CONFIG_promotion_ledger.json` gains no row; `python scripts/make_golden_config.py
--verify` passes unchanged. The golden re-base remains DEFERRED to the author
([RF-0hv](RF-0hv_working_posture_2026-09-15.md) section 7 item 1).

The three ALC tuning knobs (`amb_level_check_window_ep` 20, `_k` 4.0, `_mode` `act`) stay at
their defaults. ALC section 7 item 4 records that they have had one arm each, and ALC-STAT
section 5 blocks any tuning of them until the null and power calibration is done; adopting
the knob at its default is not a calibration claim.

## 3. The post-ALC-ARC re-baseline

`main` gained ALC-ARC after the reference pair was measured (`6ae2b5a`, the only commit in
`6f2a5d3..e437b6b` touching `src/` or `include/`). It changes WHEN the level-check window
restarts, through persistent ambiguity generations in `StateManager`, and its six-hour
accuracy validation was deferred. This section is that validation, and at the same time it
re-bases the adopted pair onto current `main`.

Arms: `runs/_rf0hv2_field6` and `runs/_rf0hv2_sim6`, each a copy of its adopted base with
ONLY `files.product_output_path` changed (verified: a one-line diff per arm). Exe
`build/x64-Release/gnss_processor_rf0hv2.exe` sha256
`A3D62A242C04E545AA2ACD8C3D0747695821A69C216D2BA6D02B1B019C15A9A7`, a renamed copy of the
`x64-Release` build of `main` `e437b6b`. Both launched detached with the run directory as the
working directory, so the traces cannot collide. Exit 0 and 720 of 720 on both; wall 282.3 s
(field) and 222.4 s (sim), run concurrently at 20 threads each on a 20-thread box.

### G0, wire identity

| bed | new arm | reference arm | verdict |
|---|---|---|---|
| FIELD | `EDAF96CEDB1262B20CA4FFA57C3CCA1F6E89DCC8B8AB1396AF7073596D660484` (5 297 595 B) | same | **PASS, whole file, no stripping needed** |
| SIM | `2149CABD1192E61062FA4AFAAB1A6D6D2995456B32437A6841B05A681DA7F12B` (4 160 397 B) | same | **PASS, whole file** |

Stronger than the gate asked for: on BOTH beds `logs/network_epoch_status.csv`,
`logs/network_dd_fractional.csv`, `logs/network_events.csv`, `logs/closed_loop_trace.csv` and
`logs/closed_loop_sat_trace.csv` are all byte-identical to the reference arm as well
(field `939727D60970F6B5` / `805D8B21F3A7F9C4` / `D78C33BAE14AE5AB` / `EE254B2E01B932A8` /
`C84647CA904A960B`; sim `E4FC04F44D51F1EE` / `8E3F6AA14565E52D` / `3766CFB3C4A6E306` /
`9CC3F566BED54963` / `60ECF98805E7392F`, the first four of which are the values ALC quoted).
The consumer trace being identical is the load-bearing one: it means the position, the
ambiguity resets and the applied corrections did not move at all.

### G1, the cold consumer

`pos_err_3d_m` in METRES, from `scripts/rf2d_grade.py` and `scripts/cc_arm_grade.py` (both
delegate to `refound_grade.consumer_table`), and independently re-read from
`logs/closed_loop_trace.csv` by a separate reader written for this card. All three agree to
four decimal places.

| bed | arm | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| FIELD | reference `_alc_field6` p50 | 0.4312 | 0.1629 | 0.3097 | 0.4892 | 0.6806 | 0.4857 |
| FIELD | **new `_rf0hv2_field6` p50** | **0.4312** | **0.1629** | **0.3097** | **0.4892** | **0.6806** | **0.4857** |
| FIELD | new p90 | 3.1123 | 0.2816 | 0.3934 | 0.6079 | 0.7104 | 0.6929 |
| SIM | reference `_alc_sim6` p50 | 0.4600 | 0.2789 | 0.2463 | 0.1513 | 0.1603 | 0.1366 |
| SIM | **new `_rf0hv2_sim6` p50** | **0.4600** | **0.2789** | **0.2463** | **0.1513** | **0.1603** | **0.1366** |
| SIM | new p90 | 3.1917 | 0.3216 | 0.2778 | 0.2070 | 0.2269 | 0.2041 |

Every per-hour delta against the reference pair is exactly 0.0000 m, inside the expectation
of plus or minus 0.10 m (field) and plus or minus 0.05 m (sim) by the whole of it. There is no
hour to report as a finding.

Per-30-min p50, twelve bins from the consumer's own first epoch, metres:

| bed | bins 1 to 12 |
|---|---|
| FIELD, new and reference alike | 1.30 0.28 0.16 0.17 0.25 0.33 0.43 0.51 0.67 0.70 0.66 0.45 |
| SIM, new and reference alike | 1.60 0.13 0.23 0.30 0.26 0.19 0.15 0.15 0.16 0.16 0.12 0.16 |

Availability and the remaining bars: emitted **720 / 720** with 0 withheld on both beds;
epochs under 0.10 m **9** (field) and **81** (sim), both unchanged; overall run p50 0.4444 m
(field) and 0.1858 m (sim). Field IF-correction dispersion p50 / p90 / max 0.055 / 0.201 /
0.68 m, worst G26 +0.68 / G27 -0.63 / G17 -0.31, and the declaration census
`continuous 0 marked / 139 unmarked, cold 12 marked / 0 unmarked, CLK_STEP 0,
cta_decouple 189`, all identical to the reference arm's published values.

SIM ambiguity integers, `scripts/simnet_amb_integer.py --ref-free` against the fixture's known
integers: **WL(1,2), last 25 % of epochs, `round(e) = 0` at 99.1 %** over n = 48 816, with
med|e| 0.029 and p90|e| 0.130 CYCLES. `round(e) = 0` is a percentage of chords at their own
true integer; `med|e|` and `p90|e|` are cycles, not metres. The value reproduces the reference
exactly, which it must, the DD trace being byte-identical.

Unit cross-check, done by reading raw rows rather than any grader. Field hour 2, ten
consecutive rows of `logs/closed_loop_trace.csv` give east / north / up of
`-0.0958 / 0.2520 / -0.3009` m down to `-0.0667 / 0.2088 / -0.2529` m with
`pos_err_3d_m = 0.4040 ... 0.3347`, and the sorted hour's median is **0.1629**, matching the
grader's 0.16. Sim hour 6, the same read gives components of order 0.06 to 0.20 m and a
median of **0.1366**, matching the grader's 0.14. The quadrature of the three components
reproduces `pos_err_3d_m` to the fourth decimal, so the column is a 3D error in metres and
the grader's p50 row is metres.

### G2, the run-end counters

| counter | field reference | field new | sim reference | sim new |
|---|---|---|---|---|
| ALC samples | 5799 | 5799 | 5387 | 5387 |
| ALC window restarts | 1159 | **1163** | 1508 | 1508 |
| ALC full-window checks | 1183 | **1145** | 1626 | 1626 |
| coverage (checks / samples) | 0.204 | **0.197** | 0.302 | 0.302 |
| ALC trips wl / if | 27 / 79 | 27 / 79 | 2 / 24 | 2 / 24 |
| ALC refloats | 106 | 106 | 26 | 26 |
| engine ledger against harness drain | AGREE | AGREE | AGREE | AGREE |
| CST `refloat_level` cell-epochs | 212 | 212 | 52 | 52 |
| PBT declared / undeclared | 326 / 1358 | 326 / 1358 | 715 / 1989 | 715 / 1989 |
| PBT sum abs translate, m | 100.927 | 100.927 | 575.557 | 575.557 |
| PBT max single translate, m | 6.320 | 6.320 | 27.045 | 27.045 |
| PBT skipped eps (declared but empty) | 9894 (64) | 9894 (64) | 8054 (16) | 8054 (16) |
| PBT no_pair / closure | 40 / HOLDS | 40 / HOLDS | 38 / HOLDS | 38 / HOLDS |

**The one measured effect of ALC-ARC on this posture is four extra window restarts and
thirty-eight fewer legal checks, on the FIELD bed only, with the response population and the
position untouched.** Coverage falls 0.204 to 0.197. The sim counters do not move at all. The
trip census is identical in every cut that was taken: 106 trips (27 wl / 79 if) on the field
over the same 17 satellites with the same per-satellite counts
(`G05 5, G06 4, G10 1, G11 3, G12 4, G13 6, G14 4, G15 15, G17 10, G19 16, G22 6, G23 10,
G24 14, G25 4, G28 1, G30 2, G32 1`), the same ratio distribution (min 4.01, p25 4.25, p50
5.58, p90 11.17, max 15.99) and the same CST cross-check at 212 cell-epochs; 26 trips
(2 wl / 24 if) on the sim with 52 cell-epochs.

So the thirty-eight suppressed checks were all non-trips. ALC-ARC's correction is real and
visible in the instrument, and on this posture it removes only checks that would have passed:
it restarts on lifecycle events the old variance fallback missed, including in epochs that
produce no sample, and each such restart costs up to a full window of checks. The deferred
six-hour accuracy validation therefore closes as a null result on both beds, which is the
outcome ALC-ARC's own scope section asked for (identical producer products, consumer changes
and restart counts reported, no threshold moved to recover a score).

## 4. The new baseline pair

**Every following consumer or vehicle card is a delta against this pair, not against
`_alc_field6` / `_alc_sim6` and not against `_rfpdfix_*_both`.**

| | FIELD | SIM |
|---|---|---|
| run dir | `runs/_rf0hv2_field6` | `runs/_rf0hv2_sim6` |
| config | `config/refound/field_gps_dual_rf0hv.json` | `config/refound/sim_gps_dual_rf0hv.json` |
| exe | `gnss_processor_rf0hv2.exe` sha256 `A3D62A242C04E545AA2ACD8C3D0747695821A69C216D2BA6D02B1B019C15A9A7` | same |
| source | `main` `e437b6b` | same |
| `products.txt` sha256 | `EDAF96CEDB1262B20CA4FFA57C3CCA1F6E89DCC8B8AB1396AF7073596D660484` | `2149CABD1192E61062FA4AFAAB1A6D6D2995456B32437A6841B05A681DA7F12B` |
| consumer p50 by hour, m | 0.43 / 0.16 / 0.31 / 0.49 / 0.68 / 0.49 | 0.46 / 0.28 / 0.25 / 0.15 / 0.16 / 0.14 |
| consumer p90 by hour, m | 3.11 / 0.28 / 0.39 / 0.61 / 0.71 / 0.69 | 3.19 / 0.32 / 0.28 / 0.21 / 0.23 / 0.20 |
| epochs under 0.10 m | 9 | 81 |
| emitted | 720 / 720 | 720 / 720 |
| IF dispersion p90, m | 0.201 | 0.051 |
| WL(1,2) ref-free last 25 %, round(e)=0 | not gradable (no truth) | 99.1 % |
| ALC checks / trips / refloats | 1145 / 106 (27 wl, 79 if) / 106 | 1626 / 26 (2 wl, 24 if) / 26 |
| PBT declared / undeclared / sum abs, m | 326 / 1358 / 100.927 | 715 / 1989 / 575.557 |

The numbers the pair carries are the ALC pair's numbers, because the wire and the consumer
trace are byte-identical; what changes is that they now stand on current `main` with the
three keys in the base rather than in an arm, and with the check count corrected by ALC-ARC.

Hour 1 is still an ignition transient on both beds (field p90 3.11 m with a 20.79 m maximum at
TOW 259230, sim p90 3.19 m): ALC section 7 item 6 records that the level-check window cannot
be full at epoch 2, so the pair inherits that open item unchanged.

## 5. Junctions in the working tree

`runs/` was relocated to `D:\GnssProcessorSupport\runs\` on 2026-09-19 and the sim base
references its fixture by absolute path. One directory JUNCTION was created so the path
resolves, and no other:

```
C:\GnssProcessor\runs\simnet60_quiet_wu93_24h  ->  D:\GnssProcessorSupport\runs\simnet60_quiet_wu93_24h
```

61 station directories behind it, probed on `S01000SIM` and the consumer `SZZ000SIM`. Nothing
else needed one: the field bed reads `C:/GnssProcessor/data/2024-129/`, which was not
relocated, and `scripts/simnet_amb_integer.py` reads the truth files through the same junction.

Both `runs/_rf0hv2_field6` and `runs/_rf0hv2_sim6` are real directories in the working tree,
created fresh; `runs/` itself is git-ignored.

**Warning for anyone tidying the tree.** `robocopy /MOVE` follows junctions, so a move of
`C:\GnssProcessor\runs` would walk into the junction and relocate the fixture out from under
every stored sim configuration. Delete the junction before moving anything, or exclude it.

## 6. What stays open

1. **ALC tuning and promotion are blocked on the calibration order of
   [ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md) section 5**: the null and the accepted
   false-refloat rate, observation-only telemetry on every eligible check, the covariance model
   tested against known-null ensembles, and calibration of the complete two-channel overlapping
   monitor. Nothing here calibrates any of that, and adopting the knob at its default is not a
   calibration claim.
2. **The next producer-to-consumer card is the frame-transfer contract of
   [PBT-AUDIT](PBT-AUDIT_translation_premise_2026-09-16.md) section 6**, the RF-3 wire contract:
   declare old and new frame identity, affected signals, the signed transform including coupled
   clock and code changes, and whether the offset is known or estimated; translate only the
   declared known frame component. PBT-AUDIT falsified the blanket translation premise the
   adopted `pb_step_translate` rests on, so the adopted knob is an empirical comparator, not a
   settled transition model.
3. **The restarts-by-cause attribution defect** recorded in section 1 belongs to any future
   restart-policy work, refused knob or not.
4. **The coverage question ALC section 7 item 2 raised is still open and is now narrower.**
   ALC-ARC moved coverage to 0.197 on the field, ALC-BAND refused the band-scoped answer, and
   ALC2 shows that simply holding windows open costs position. Raising coverage needs a
   different observable with its own calibration, not a looser restart rule.
5. **The ignition transient** at hour 1 on both beds, inherited unchanged.
