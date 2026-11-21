# F4R: a read of the failing two-run check of DOY 146 (2026-11-18)

**STATUS**

- **id:** F4R
- **topic:** network-vehicle
- **status:** closed
- **summary:** A read of bar F4 of NRF section 123 (the two-run check of DOY 146 from two references) under the full adopted configuration. The 15 failing Galileo links reduce to two real one-cycle disagreements, GUAM00GUM-E29 (run `ad` departs) and LMMF00MTQ-E06 (run `adr` departs); the other 13 are echoes of the check's tree walk. The attribution runs of 2026-11-19 show that exact epoch time is the only one of the differences that changes any output (the entry check, the engine build and the dropped pair change nothing), and that the mechanism is a hold one cycle off that leaves 12 to 18 mm on the link, below the 30 mm of the held-link monitor.
- **source:** an extra package of the plan of 2026-11-18, "a read of the failing two-run check of the five-day read"; drafted at `03dfcbc` (branch `chain/audit`) from the run traces of `_rap_{ad,adr,n46c,n46cr}_n146` (runs built from `91f36cf`); commits read for the check: `7fb0737` for network code and main `5ddc422` for user-engine code; the attribution runs read by the managing session on 2026-11-19 (last section). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** Bar F4 fails on DOY 146, but by distinct link only GUAM00GUM-E29 and LMMF00MTQ-E06 disagree, each (-1,-1) on E1 and E5a, with no slip, gap, resume, re-host or release at the link. The attribution runs settle the cause: with exact epoch time off the pair reproduces the morning pair byte for byte; the entry check, the engine build and the dropped pair change nothing. Exact epoch time moves the floats from the first epoch and so which decisions each run takes. The defect is a hold one cycle off leaving 12 to 18 mm, below the monitor's 30 mm; both nights carry it.

Read at commit 03dfcbc (branch `chain/audit`, worktree `D:/GnssProcessorSupport/worktrees/chain`). Code citations are `path:line` at 03dfcbc unless a line names 91f36cf. This document is a read; it decides nothing.

## 1. The runs and the conventions

- `_rap_ad_n146` (default reference) and `_rap_adr_n146` (WTZR reference): tonight's adopted configuration in full, run on `gnss_processor_cha_fp1.exe`, built from 91f36cf (NRF part 18, the five-day read paragraph; part 17).
- `_rap_n46c_n146` and `_rap_n46cr_n146`: this morning's pair (section 118), same two references, which passed F4.
- Configuration differences between tonight and this morning, from `arm.json`: three, not four. `network.ring_fix.entry_revalidate` on, `processing.exact_epoch_time` on, and the `E:L1X/L5X` pair dropped. The Galileo ring-fix row counts are identical in both nights (957 348, all L1C/L5Q), so the dropped pair matched no row (inferred). Between the `ad` and `adr` arms the only difference besides paths is `network.phase_datum_eq34_receiver`.
- Hour convention used throughout: hour of day = (tow - 172800)/3600, GPS time, DOY 146. The first epoch, 172980, is hour 0.05. The five-day read's hours equal hour of day + 0.95.
- Attribution runs, as the first stage used them (the record does not define them; inferred from their stated predictions): V1 = adopted with the entry check off (exact epoch time on); V2 = adopted with exact epoch time off (entry check on); V3 = both off (this morning's configuration). Each from both references. Folders `_rap_v1_n146`, `_rap_v2_n146`, `_rap_v3_n146` and their `r` versions exist and were not opened. The build they ran on is not known to this read.

## 2. The 15 links

Integers are the held L1C/L5Q values in the order `ad`, `adr`, `n46c`, `n46cr`. "Tree" marks a value pinned by the chain's tree. Values that differ by one between same-reference runs (for example PARK 28 against 29) are whole-receiver shifts that the per-receiver integer absorbs.

| Cluster | Link | Audit failing epochs (hour of day) | Held integers: ad; adr; n46c; n46cr | Real disagreement | Departing run |
|---|---|---|---|---|---|
| A | GUAM00GUM-E29 | 2.275 to 5.000 (314 epochs; real disagreement to 6.083) | -53/-120; -103/-44; -52/-119; -103/-44 | yes | ad |
| A' | GUAM00GUM-E04 | 5.008 to 6.083 | 94/33; 138/213; 94/33; 138/213 | no | none |
| A' | GUAM00GUM-E19 | 5.008 to 6.083 | 131/161; 9/-33 (tree at end); 131/161; 9/-33 | no | none |
| A' | GUAM00GUM-E28 | 5.008 to 6.083 | 161/154; 87/117; 161/154; 87/117 | no | none |
| A' | GUAM00GUM-E33 | 5.008 to 6.083 | 0/0 tree; 38/63; 0/0 tree; 38/63 | no | none |
| A' | GUAM00GUM-E26 | 5.008 to 6.025 | -28/-174; -10/-14 tree; -29/-174; -10/-14 tree | no | none |
| A' | GUAM00GUM-E31 | 5.008 to 5.667 | 114/20; 71/105; 114/20; 71/105 | no | none |
| B | LMMF00MTQ-E06 | 20.842 to 23.992 (379 epochs; real disagreement from 20.758) | 90/-54; -18/78; 90/-54; -17/80 | yes | adr |
| C | DARW00AUS-E29 | 3.100 to 3.208 | 28/105; -158/-104; 28/105; -158/-104 | no | none |
| C | KOKV00USA-E29 | 3.100 to 3.208 | 56/139; -64/-2 tree; 57/139; -65/-2 tree | no | none |
| C | MAJU00MHL-E29 | 3.100 to 3.208 | 0/0 tree; -253/-80; 0/0 tree; -253/-80 | no | none |
| C | PARK00AUS-E29 | 3.100 to 3.208 | 28/88; -95/3; 29/89; -96/3 | no | none |
| C | SOLO00SLB-E29 | 3.100 to 3.208 | 0/0 tree; -170/-13; 0/0 tree; -170/-13 | no | none |
| C | WARK00NZL-E29 | 3.100 to 3.208 | -78/-133; -7/136; -78/-133; -7/136 | no | none |
| C | MAC100AUS-E29 | 3.100 to 3.108 | -1/19 tree; -6/2; -1/19 tree; -6/3 | no | none |

How this was established (shown):
- A fit of one integer per receiver and one per satellite to each of the six run pairs, keeping the values most links agree with, absorbs every Galileo link except the two marked "yes". A four-link loop-closure scan of every E1 link at every epoch of the `ad`/`adr` pair gives the same two links with a nonzero mode, and their E5a closures agree. All six cross pairs place the departure on `ad` at GUAM and on `adr` at LMMF; the two morning runs disagree nowhere.
- All four runs hold every one of the 15 links at every failing epoch.
- The 13 others are echoes of GUAM-E29. The audit builds its spanning tree over links in sorted order and counts a failure on whichever link closes a loop (`D:/GnssProcessorSupport/worktrees/irdu/scripts/nrf_two_run_audit.py:180-205`). In the 14 epochs of cluster C, GUAM-E29 is a tree edge of the audit, so its one-cycle error lands on E29's other links; the audit's GUAM-E29 count is short by exactly those 14. From 190830 (hour 5.008) GUAM-E29 is again a tree edge, so GUAM's other links fail with mixed signs while GUAM-E29 still disagrees to 194700. A true whole-receiver or whole-satellite shift cannot fail this audit.
- The per-link and per-epoch counts of the audit therefore depend on its walk. Only "two links, not zero" is a property of the runs.

## 3. What happened first in each cluster

### Clusters A, A' and C: GUAM00GUM-E29

- [corrected 2026-11-19: see Corrections, item 6] No slip flag, gap, arc resume, re-host, tree role or release on GUAM-E29 in any of the four runs between 172980 and 195000; no chain-edge row names the edge. The satellite-wide E29 chain events up to 175140 are identical in `ad` and `n46c`.
- The three agreeing runs decided early: `adr` at 175770 (hour 0.825; -103/-44, frac_if 0.102, sigma 0.142), `n46cr` at 175800 (same integers, frac_if 0.133), `n46c` at 176850 (hour 1.125; -52/-119, frac_if 0.190, log-odds 22.7, after persistence from 176580).
- `ad` shares `n46c`'s reference and starts from the same float frame, but its float sits apart from the first epoch: the f1 floats of all 750 ring-fix rows at 172980 differ between `ad` and `n46c` (mean 0.043 cycle, maximum 0.18), and at GUAM-E29 the difference is -0.24 cycle at 175500. (The first stage dated the split from hour 0.75; the skeptic's whole-row comparison shows it is present at the first epoch. The skeptic's reading is kept.)
- [corrected 2026-11-19: see Corrections, item 3] 175770 to 176040: `ad` refused for the odds while its fraction was inside the bound. At 176130 (hour 0.925) its fraction reached 0.215, above the 0.20 bound (`include/network/ar/RingFix.hpp:70`), so the 10-epoch persistence count (`RingFix.hpp:77`) never started.
- [corrected 2026-11-19: see Corrections, item 4] `ad` then refused for the fraction for 4 830 s while its float walked: -52.46 at hour 1.52, -52.66 at hour 1.85, -52.78 at hour 2.18. Its candidate moved from -52/-119 to -53/-120 at 178410 (hour 1.558).
- [corrected 2026-11-19: see Corrections, item 5] At 180990 (hour 2.275) `ad` decided -53/-120: frac_if 0.181, sigma_if 0.042, log-odds 176.9 / 232.7, event `RINGFIX,hold`. The wide lane is 67 in both runs, so this is a one-cycle N1 difference with the same wide lane. Each run held its value until 194700 (`n46c`) or the gap release at 195660 (`ad`).
- No held value slips. The departure is an entry on the other integer after a float walk of about 0.6 cycle.

### Cluster B: LMMF00MTQ-E06

- All four runs decided E06 at 244620 (hour 19.950), agreeing under the frame integers: 89/-55 (`ad`, `n46c`), -18/78 (`adr`), -18/79 (`n46cr`).
- Around 245940 to 246030 a misfit appears at LMMF: the held E06 residual rises from 0.005 to 0.031 m, almost identically in `ad` and `adr`; block-Baarda outliers fire at 245700 and 246000 on E06, E26, E28, E29 and E33 in both runs; no slip, gap or resume at LMMF on E06 or E29.
- In `ad`, `n46c` and `n46cr`: LMMF-E29 was a held link (decided at 226170, frac_if 0.195 to 0.199) and LMMF-E26 a tree edge. The monitor released E29 at 246060 (hour 20.350), then E06 at 246180 (`n46c`, `n46cr`) or 246270 (`ad`). E06's float walked from 89.29 to 89.86 and was re-decided one cycle up with wide lane 144 unchanged: at 247530 (hour 20.758; `n46c`, `n46cr`) and 247830 (`ad`).
- [corrected 2026-11-19: see Corrections, item 7] [corrected 2026-11-19: see Corrections, item 8] In `adr` the roles were swapped from 231150 (hour 16.158): LMMF-E29 was handed to the tree (`RINGFIX to_tree`, `PHASE_DATUM eq34_reattach POVE00BRA->LMMF00MTQ`, `CHAIN rehost join`), and LMMF-E26 was a held link. A tree edge cannot be released by the monitor: the monitor counter runs only for held links (`src/core/network/ar/RingFix.cpp:467-474`), and a held link entering the tree is released with cause Chain (`RingFix.cpp:599`). At the misfit `adr` released E26 at 246210; E06's monitor count reset twice on epochs at 0.0270 and 0.0291 m (bound 0.03 m over 4 epochs, `RingFix.hpp:81,83`; `RingFix.cpp:469-474`), never tripped, and E06 stayed at -18/78 to 259170.
- First difference at this link: the 231150 tree handover in `adr`, which `n46cr` (same reference) did not make. What drove it is not traced.

## 4. Which integer is right: the first stage and the skeptic disagree

The first stage treated the run departing from three agreeing runs as probably off. The skeptic rejects the majority rule and reads the sign of the held residual instead. Half-hourly mean held `resid_if_m`:

| Link | Run(s) | Held integer | Mean residual | Reading (inferred) |
|---|---|---|---|---|
| GUAM-E29, hours 1 to 5.5 after first epoch | n46c, adr, n46cr | -52 / -103 | -0.004 to -0.015 m, every half hour negative | one cycle off |
| GUAM-E29, same hours | ad | -53 | +0.0002 to +0.0025 m | right |
| LMMF-E06, 20.0 to 23.5 h after first epoch | adr | -18/78 (first decision) | +0.0147 to +0.0172 m, every half hour positive | one cycle off |
| LMMF-E06, after the re-hold | ad, n46c, n46cr | one up | -0.0009 to +0.0018 m | right |
| LMMF-E06, first hold, 19.5 to 20.5 h | all four | first decision | +0.018 m | the shared first decision was off |

- A (1,1) error is about 10.9 cm in the ionosphere-free combination; a one-signed 1 to 1.7 cm left on the link after the network absorbs the rest is what such an error looks like (inferred). On this reading `ad` holds the right integer at GUAM and the majority holds the off one; at LMMF the majority is right and `adr` kept the off first decision.
- The first stage's remark that `adr`'s LMMF residual "fell to 0.0025 m, so the network absorbed its held integer without strain" is refuted: the dip to 0.004 m at 246360 followed the E26 release, and the residual returned to the one-signed +0.015 m level.
- One observation is consistent with the skeptic at GUAM: `n46c`'s hold showed a fold residual statistic of 40.0 on L1C (e = -0.169 m) at 176880, against 12.0 with e near 0 for `ad`'s hold at 181110.
- Without truth on observed data neither reading is proven. The residual test is better evidence than a vote; it remains inferred.

## 5. Hypotheses, ranked

[corrected 2026-11-19: see Corrections, item 9] **H1 (leading). Near-bound holds one cycle off that the monitor cannot see; exact epoch time changes which run lands on them.**
- [corrected 2026-11-19: see Corrections, item 9] Mechanism: the gate decides on the fraction and the formal sigma (`RingFix.cpp:220-231`, `:512-514` at 91f36cf), so a float still walking is decided at 0.18 to 0.19. A one-cycle error leaves 1 to 1.7 cm, below the 0.03 m monitor bound, and the hold persists for hours. At LMMF the off value was released only where a neighbouring link pushed the total past the bound; in `adr` the tree handover changed which neighbour that was.
- Why exact epoch time: it is the only one of the three differences that moves inputs before any hold exists (the floats differ at 172980). Off, calendar times carry a sawtooth of up to about 20 microseconds with a 675 s period (NRF 80; `include/math/TimeUtils.hpp:539-555`), worth millimetres to about 1.5 cm on double differences (inferred). The effect is common to both references, so it cannot pick a reference by itself; it moves the decision path, and a pair that agreed along one path need not agree along another. On DOY 146 the switch is not a small perturbation for GPS levels either (NRF 124).
- [corrected 2026-11-19: see Corrections, item 1] Fits: one link per cluster, no event at the link, onset at a plain decided hold, (-1,-1) on both bands, a one-signed residual on the off run.
- Predicts: V1 fails at GUAM-E29 and LMMF-E06, and is byte-identical to the adopted run of the same reference if the build is the same (zero refusals). V2 passes and matches this morning's pair in `network_ring_fix.csv` if the build and the inert dropped pair hold. V3 passes.
- Under H1 the switch is the trigger, not the defect: the defect is the unseen one-cycle hold, which any small change of path can expose.

**H2. The same latent sensitivity, triggered by the reference alone.**
- Fails without either switch, at whichever links happen to sit near the bound on the day.
- Predicts: V2 and possibly V3 fail at other links. This morning's pair agreed on DOY 146, which argues against it.

**H3. A build difference.** Relevant only if the V runs ran on a build other than 91f36cf (for example one carrying S8 (a) and (b)). Predicts V3 failing, which would place the cause in the engine change, not in the switches.

**Excluded for these runs: the entry check (mechanisms E1 and E2 of the first stage).**
- In 91f36cf the check acts only on the cut path (`RingFixSeams.cpp:675-700` at 91f36cf) and changes the result only by refusing. Run end: `ad` 24 cuts checked, 24 known, 0 refused; `adr` 26, 26, 0 (`main_log.log:14560` of each run).
- The keep-carry check analysed by the first stage (`src/core/network/solve/ChainDatum.cpp:1650-1670`, `chainEntryStep` at `RingFix.cpp:737-745`) is not in 91f36cf: 8d71321 (S8 (a)) and 12ecc70 (S8 (b)) are not its ancestors, and the run-end line for kept re-hosts is absent from both logs.
- The check's only other effect, the averaged code residuals (`RingFix.cpp:476-483`), is read by nothing but the verdict (`RingFix.cpp:713-723`).
- The first stage's own condition for clearing the check without V1 (zero refusal counters) is met.

## 6. Shown and inferred

Shown (from the traces, the logs or a code line):
- Two links disagree; their integers, onsets and departing runs; the 13 others are walk artefacts of the audit.
- [corrected 2026-11-19: see Corrections, item 1] [corrected 2026-11-19: see Corrections, item 2] No event at either link before its departure; each departure begins at a plain decided hold at frac_if 0.18 to 0.19.
- The floats of the two same-reference runs differ at the first epoch.
- Zero entry-check refusals in both adopted runs; the keep path absent from the build that ran.
- The monitor's 0.03 m and 4-epoch rule and its reset pattern on `adr`'s LMMF-E06; tree edges are not monitor-released.
- The one-signed held residuals tabled in section 4.

Inferred:
- That the dropped `E:L1X/L5X` pair is inert (identical row counts).
- [corrected 2026-11-19: see Corrections, item 10] That exact epoch time is the source of the first-epoch float difference (the only remaining input change).
- Which integer is right at either link (the residual-sign reading).
- That near-bound holds on walking floats are the common mechanism.
- The V1, V2, V3 definitions and the predictions written for them.

## 7. What to look at first when V1, V2 and V3 are graded

1. The exe and its build commit for each V run. Every comparison below assumes one build; a different build changes the meaning of V3 (H3).
2. The entry-check run-end line of V1 and V2 (cuts, known, refused).
3. V1 against the adopted run of the same reference: a byte comparison of `logs/network_ring_fix.csv`. Identical confirms the entry check is inert here.
4. V2 against `_rap_n46c_n146` and `_rap_n46cr_n146`: the same byte comparison. Identical supports H1 and confirms the dropped pair is inert; any difference from the first epoch means another input moved.
5. The F4 count of each V pair by distinct link, with the per-receiver and per-satellite fit or the closure scan, not the audit's per-epoch count.
6. Where any V pair disagrees: the held-residual sign and size on the disagreeing link, and whether its decision was taken at frac_if above about 0.15 on a walking float.
7. A count, in every run, of holds decided at frac_if above 0.15 that keep a one-signed mean residual above 1 cm for an hour. This sizes the unseen-hold population independently of the two-run audit.

## 8. Runs a later design would need (listed, none requested)

- The V1, V2, V3 pairs already exist; grading them is a read.
- If H1 holds, a later design for a monitor on the one-signed mean residual over a window, or a gate requiring a stationary float, would need DOY 146 from both references and the five days under the same build, plus a same-build comparison with the new behaviour off (identical output).

## Decisions for the author

- Whether the two-run audit's disagreements are read by the held-residual sign instead of the three-against-one majority.
- Whether bar F4 is counted by distinct disagreeing link instead of failing checks, since the check count depends on the audit's tree walk.
- Whether a one-cycle Galileo narrow-lane hold that leaves 1 to 1.7 cm, unseen by the 0.03 m and 4-epoch monitor, is recorded as a gap under invariant 1 of CLAUDE.md, with a design for a windowed one-signed residual monitor or a stationarity condition at decision.
- Whether the V runs are graded only against runs of the same build, and the morning pair re-run on that build if it differs.

## Not checked

- [corrected 2026-11-19: see Corrections, item 10] The V1, V2 and V3 folders, their build and their results.
- What drove `adr`'s handover of LMMF-E29 to the tree at 231150.
- The source of the GUAM-E29 float difference beyond the first-epoch comparison; that exact epoch time causes it is inferred.
- The E5a closure scan for links other than the two disagreeing ones.
- Which integer is right at either link (no truth on observed data).
- Whether this morning's pair ran on the same exe as tonight's.
- Whether the slip layer's gap pair names match the relabelled family pair at combined-mode receivers (relevant only once the entry check refuses).
- The GPS failures at MBAR00UGA and the other bars of the five-day read.

## 2026-11-19: corrections from the check against the code

The independent check read the network code at `7fb0737` (`D:/GnssProcessorSupport/worktrees/read_7fb0737`), user-engine code at main `5ddc422`, and older commits by `git show`; it opened the small trace extracts of `results/f4_scratch/` and no run folder. The markers in the body point to these items; the original words are left in place.

1. **No event at either link** (Summary, Outcome, section 5 "Fits", section 6). The draft says each departure is a plain decided hold with no event at the link. The extracts show no slip, gap, resume, re-host or release (confirmed), but measure-only block-Baarda outliers recur at GUAM-E29: 28 in each of `adr`, `n46c` and `n46cr` against 4 in `ad`; at LMMF-E06, 25 in `adr` against 9 or 10 in the other runs (`f4_scratch/timeline.txt`). Corrected wording: no slip, gap, resume, re-host or release at the link; measure-only block-Baarda outliers recur there, far more often in the runs holding the off value.
2. **Decision fraction at LMMF** (Outcome, section 6). The draft says both departures begin at a hold decided at frac 0.18 to 0.19. True at GUAM (`ad`, 0.181). At LMMF the value `adr` keeps was decided at 244620 with frac 0.151 (`ad` 0.111, `n46c` 0.102, `n46cr` 0.105); the re-decisions of the other runs were at 0.137 and 0.177 (`f4_scratch/lmmf_e06_*`).
3. **GUAM odds and fraction epochs** (section 3). The draft says `ad` refused for the odds from 175770 to 176040 and reached frac 0.215 at 176130 (hour 0.925). The extract shows odds refusals from 175740 to 176070 and the first fraction refusal at 176100 (hour 0.917) with 0.2093 (`f4_scratch/guam_e29__rap_ad_n146.txt`).
4. **Length of the fraction refusals** (section 3). The draft says 4 830 s. The fraction refusals run from 176100 to 180690 (about 4 600 s), followed by nine persistence epochs 180720 to 180960; 4 830 s includes those (`f4_scratch/timeline.txt`).
5. **End of the GUAM holds** (section 3). The draft says `n46c` held to 194700. `n46c` was also released by a gap, at 195630; 194700 is only the end of the compared window. Both held their values to gap releases (`n46c` 195630, `ad` 195660).
6. **E29 chain events before 175140** (section 3). The draft says they are identical in `ad` and `n46c`. They follow the same sequence, but their values differ: at 174810 the SYOG00ATA L5Q re-host has target_cyc 51 in `ad` against 50 in `n46c`, and every legacy_pinmove differs (`f4_scratch/guam_e29_*`).
7. **LMMF-E26 in `adr`** (section 3). The draft calls it a held link from 231150. E26 was decided at 244410 (released at 246210, held_s 1800).
8. **Hour of the 231150 handover** (section 3). The draft gives hour 16.158. (231150 - 172800)/3600 = 16.208; 16.158 is the hours-after-first-epoch value.
9. **H1 mechanism, "near-bound holds"** (section 5). The draft says the gate decides on the fraction and the formal sigma, so a walking float is decided at 0.18 to 0.19. At `91f36cf` (`src/core/network/ar/RingFix.cpp:220,230-231,512-514`) under `gate=odds` the 0.10 sigma bound is not applied, and sigma enters only through the log-odds (bound ln 1e6, `include/network/ar/RingFix.hpp:114` at `91f36cf`). Under the residual reading of section 4 the off holds were decided at frac 0.102 to 0.190 with sigma_if 0.117 to 0.142 (log-odds 18 to 28), the right holds at sigma_if 0.042 to 0.075 (log-odds 63 to 177): in these seven decisions the pattern separates on formal sigma, not on the fraction (inferred). The heading reads better as "One-cycle holds left in place by the monitor". The population count of 2026-11-19 (next section, subsection 4) qualifies this: no hold decided at sigma 0.08 cycle or less carries the one-signed signature, but 86 % of all holds are decided above 0.08, and half of the 42 signed holds were decided at log-odds above 30.
10. **Attribution runs** (sections 5 to 7, section 6 "Inferred", "Not checked"). The draft did not open the V folders. The check records that V1 equals the adopted run for both references (entry check inert, now shown) and V2 equals the morning run for the default reference (exact epoch time alone separates `ad` from `n46c`; dropped pair inert; builds equivalent there). "Exact epoch time is the source of the first-epoch float difference" moves from Inferred to Shown. The remaining runs (V2r, V3, V3r) are in the next section: every V run is byte-identical to its predicted partner, so the prediction of H1 holds for both references and H2 and H3 get no support.
11. **Hour convention** (section 1; not marked in the body). Section 1 says hour of day is used throughout, but the section 4 table uses hours after the first epoch.

### Line numbers that moved

| Draft citation | Line at `7fb0737` |
|---|---|
| `RingFix.cpp:467-474` at `03dfcbc` (monitor counter, held links only) | `src/core/network/ar/RingFix.cpp:470-477` (inside `if (L.held)`) |
| `RingFix.cpp:599` at `03dfcbc` and `91f36cf` (held link entering the tree, cause Chain) | `src/core/network/ar/RingFix.cpp:602` (`tree_now`) |
| `chainEntryStep` at `RingFix.cpp:737-745` | `src/core/network/ar/RingFix.cpp:740-748` |
| averaged code residuals `RingFix.cpp:476-483`, verdict `:713-723` | `src/core/network/ar/RingFix.cpp:478-486`, verdict `:716-726` |
| monitor reset `RingFix.cpp:469-474` | `src/core/network/ar/RingFix.cpp:472-477` |
| fraction bound `RingFix.hpp:70` | same line at `7fb0737` and `03dfcbc`; `include/network/ar/RingFix.hpp:68` at `91f36cf` |
| sawtooth `include/math/TimeUtils.hpp:539-555` | `include/math/TimeUtils.hpp:540-555` (main: the same text from line 541) |

Confirmed at the cited lines: the persistence count (`RingFix.hpp:77`; refuse lambda `RingFix.cpp:495`), the monitor bound (`RingFix.hpp:81,83`), the entry check acting only on the cut path at `91f36cf` (`RingFixSeams.cpp:624,669-713`), `8d71321` and `12ecc70` not ancestors of `91f36cf`, the keep-carry check at `src/core/network/solve/ChainDatum.cpp:1650-1670`, and the check's tree walk (`git show 72b7b3b:scripts/nrf_two_run_audit.py:179-205`). The averaged code residuals are also read by nn_guard and nn_monitor when those are armed.

### What stands

- Two real disagreements (GUAM-E29 in `ad`, LMMF-E06 in `adr`), 13 echoes of the check's walk, and the entry check excluded. The V1 byte equality settles the exclusion.
- H1 stands first as far as the trigger goes: exact epoch time moves the decision path (V2 = `n46c`, and, from the next section, V2r = `n46cr`). The "near-bound fraction" part of H1 does not follow from the draft's own records (item 9).
- Decision 1 (read by residual sign, not by majority) stands. The block-Baarda asymmetry (item 1) is further evidence on the same side; it remains inferred, with no truth.
- Decision 2 (count by distinct link) stands as a reporting choice. It does not change F4, whose pre-registered bar is 0 failing checks; two links still fail.
- Decision 3 gains a third design option, a sigma or odds floor at decision, beside the windowed residual monitor and the stationarity condition. The population count of the next section argues against a stricter decision rule (many good holds lost for each one avoided) and estimates a windowed monitor offline.
- Decision 4 (grade the V runs only against runs of the same build) stands; the V runs show the two builds output-identical on DOY 146.

### Left unconfirmed

- The run-end entry-check line (`ad` 24/24/0, `adr` 26/26/0); the line exists at `91f36cf` `RingFixSeams.cpp:1772-1777`, the logs were not opened. (V1 = adopted makes the point moot.)
- That the 13 other links are walk echoes: consistent (all four runs hold all 15, the 13 show no pair residual), tree membership per epoch not reproduced.
- The E06 monitor reset values 0.0270 and 0.0291 m.
- The half-hourly mean residuals of the section 4 table (spot samples have the stated signs); the next section recomputes them over whole holds.
- The first-epoch f1 difference over 750 rows (mean 0.043, maximum 0.18).
- `arm.json`, the 231150 handover, LMMF-E29's decision fraction at 226170, and the +0.95 hour offset of the five-day read.

## 2026-11-19: the attribution runs and the population of one-signed holds

Copied from the note of the managing session, `D:/GnssProcessorSupport/gates/manager_2026-11-19/ATTRIBUTION_two_run_check.md`, sections 2 to 7. Its numbers were computed from the run folders under `D:/GnssProcessorSupport/runs_nom` with the scripts of that folder (`resid_sign.py`, `decision_split.py`, `window_monitor_replay.py`). Section numbers inside this section refer to its own subsections; "the check of the draft" is the check whose corrections precede this section.

### 2. The attribution runs

| run | configuration (from its `arm.json`) | byte comparison (sha256 of `logs/network_ring_fix.csv` and `products.txt`) |
|---|---|---|
| `v1_n146`, `v1r_n146` | adopted, minus the entry check | identical to `ad_n146` and `adr_n146` |
| `v2_n146` | adopted, minus exact epoch time | identical to `n46c_n146` |
| `v2r_n146` | the same, second reference | see section 6 (added when the run ended) |
| `v3_n146`, `v3r_n146` | the morning configuration with the adopted pairs list, on the new engine | see section 6 |
| `v1_n143` | DOY 143, adopted minus the entry check | see section 6 |

Read:

- The entry check changes nothing on DOY 146 (no refusal in either run, so no path differs).
- With exact epoch time off, the run on the new engine with the entry check on and the pair dropped reproduces the
  morning run byte for byte. So the engine build, the dropped pair and the entry check are all without effect here.
- Exact epoch time is the one setting that moves the output. It changes the float values from the first epoch
  (a few hundredths of a cycle), and with them which decisions each run takes.

### 3. Which run holds the off value

There is no truth on observed data. The evidence is the residual a held link keeps (`resid_if_m` of the ring-fix
trace, the ionosphere-free misfit of the held value). Script `resid_sign.py`.

| link, hours of day | run | held integers (E1, E5a) | mean residual | epochs on the positive side |
|---|---|---|---|---|
| GUAM00GUM with E29, 2.28 to 6.08 | `ad_n146` | -53, -120 | +1.3 mm | 273 of 457 |
| the same, 1.0 to 6.08 | `adr_n146` | -103, -44 | -12.1 mm | 11 of 611 |
| the same, 1.13 to 6.08 | `n46c_n146` | -52, -119 | -11.9 mm | 15 of 595 |
| the same, 1.0 to 6.08 | `n46cr_n146` | -103, -44 | -12.1 mm | 8 of 611 |
| LMMF00MTQ with E06, 19.96 to 20.4 (first hold) | `ad_n146`, `n46c_n146`, `n46cr_n146` | (89, -55) or (-18, 79) | +17.8 to +18.4 mm | every epoch (51 to 54 of 51 to 54) |
| the same, 20.8 to 24.0 (after release and a new decision one cycle up) | `ad_n146`, `n46c_n146`, `n46cr_n146` | one up | +0.2 mm | about half |
| the same, 19.96 to 24.0 (never released) | `adr_n146` | -18, 78 | +15.9 mm | 485 of 485 |

Read:

- At GUAM the run that departs from the other three (`ad_n146`) is the one with a residual centred on zero. The other
  three, both morning runs among them, carry a one-signed 12 mm for five hours. The morning pair passed the two-run
  check with both runs holding the same value: a pair that agrees is not thereby right.
- At LMMF every run first decided the same value (the same in the frame of each reference), which left a one-signed
  16 to 18 mm. Three runs released it after 25 minutes (the release came through a neighbouring disturbance) and
  decided one cycle up; the second-reference adopted run kept it for four hours.
- A hold one cycle off leaves 12 to 18 mm on the link. The monitor of a held link releases at 30 mm on four
  consecutive epochs, so it does not see it.
- The two-run check counts 1 502 failing checks per band because its walk carries one link's disagreement to the
  other links of the same receiver or satellite. By distinct link the adopted pair disagrees on two links
  (confirmed by `runs-reads` with its own loop check).

So the adopted switches did not create the failure. Both nights hold the same kind of error; the check sees it only
where the two runs happen to differ, and exact epoch time moved where they differ.

### 4. How many holds look like this

Script `decision_split.py` over the six adopted day runs and the morning pair (31 795 holds lasting an hour or more).
Signature: a mean residual of at least 8 mm with at least 90 % of the epochs on one side. A typical hold has a mean
residual of 0.3 mm (99 % of holds below 2.7 mm), so the signature is far outside the ordinary.

| run | holds of an hour or more | with the signature | where |
|---|---|---|---|
| `ad_n143` | 4 003 | 3 | COCO00AUS (2), YKRO00CIV |
| `ad_n144` | 3 950 | 9 | CUSV00THA (4), KIT300UZB (3), LMMF00MTQ (2), all after hour 17 |
| `ad_n145` | 3 904 | 16 | FFMJ00DEU (7), SOLO00SLB (5), SGOC00LKA (3), LMMF00MTQ |
| `ad_n146` | 3 936 | 1 | GUAM00GUM with G09 |
| `adr_n146` | 4 027 | 3 | GUAM00GUM with E29 and G09, LMMF00MTQ with E06 |
| `ad_n147` | 3 964 | 6 | MAJU00MHL (3), COCO00AUS (2), SGOC00LKA |
| `n46c_n146`, `n46cr_n146` | 3 943, 4 068 | 2, 2 | GUAM00GUM with E29 and G09 |

Read, with care:

- 42 of 31 795 (0.13 %), 1 to 16 a day. GUAM00GUM with G09 carries it in all four DOY 146 runs, so no two-run check
  can see it.
- The signature is not proof of a wrong integer. Where several links of one station carry it in the same hours with
  both signs (FFMJ00DEU on DOY 145, seven links), a station-level cause (wet delay, multipath, the station's own
  level) is as likely as several wrong integers. The two-run comparison separated the two only for the two links of
  section 3.
- The values at the moment of decision do not separate these holds well. The check of the draft suggested that the
  off holds were decided at a large formal sigma through the odds rule (`gate_rule` odds has no 0.10 sigma bound:
  `RingFix.cpp`, the decision step). Over the population: no hold decided at a sigma of 0.08 cycle or less carries
  the signature (0 of 4 442), but 86 % of all holds are decided above 0.08. By the odds at decision the share with
  the signature is 2.7 % and 0.7 % (Galileo, GPS) at log-odds up to 20, 0.4 % and 0.1 % at 20 to 30, 0.1 % at 30 to
  60, almost none above 60. Half of the 42 were decided at log-odds above 30. A stricter decision rule would
  therefore cost many good holds for each one avoided.

### 5. What a windowed monitor would do (offline estimate, no run)

Script `window_monitor_replay.py`: a held link trips when the mean of its last W residuals reaches T in size.

| window | threshold | of the 42 caught | other holds tripped (of 45 982, all lengths) | minutes from the hold's start to the trip |
|---|---|---|---|---|
| 20 min | 8 mm | 42 | 738 (1.6 %) | 26 |
| 40 min | 6 mm | 42 | 521 (1.1 %) | 40 |
| 40 min | 8 mm | 42 | 182 (0.40 %) | 40 |
| 40 min | 10 mm | 41 | 75 (0.16 %) | 43 |

At 40 minutes and 8 mm the rule would release about 28 links per day run (5 with the signature of section 4, about 23
others, of about 5 700 holds a day). The present monitor releases none of the 42. The estimate cannot show what the
network does after a release; a released hold that was right is decided again within the persistence time.

### 6. Added when the remaining runs ended

- 00:10 on 2026-11-19: `v2r_n146` (adopted minus exact epoch time, second reference) is byte-identical to
  `n46cr_n146` (sha256 of `logs/network_ring_fix.csv` dfc5728795633656 and of `products.txt` 4da678228f69cf1e in
  both). With `v2_n146` equal to `n46c_n146`, the pair without exact epoch time IS the morning pair: 0 failing
  Galileo checks, with both runs carrying the same one-signed value at GUAM00GUM with E29 (section 3).
- `v3_n146` (the morning configuration with the adopted pairs list, on the new engine) is byte-identical to
  `n46c_n146` (7290f233275f47a4, 5cef60ef22555721). The engine build and the dropped pair are without effect.
- 00:15: `v3r_n146` is byte-identical to `n46cr_n146` (dfc5728795633656, 4da678228f69cf1e). The solution trace
  (`logs/network_solution.csv`) was hashed too and agrees in every group. The four groups of identical runs:
  {`n46c`, `v2`, `v3`}, {`n46cr`, `v2r`, `v3r`}, {`ad`, `v1`}, {`adr`, `v1r`}. The attribution on DOY 146 is
  complete: exact epoch time is the only one of the differences that changes any output.
- `v1_n143` (DOY 143, for the hold through a resumed arc at BOGT00COL with G09): added below when it ends.

### 7. What follows

- The two-run check is not a test of correctness: it sees a wrong hold only where the two runs differ. Its count by
  failing checks depends on its own walk; by distinct link it is two.
- The gap is the monitor's blind band below 30 mm. A design is needed, not a switch: a monitor on the windowed mean
  residual of a held link (release, then a new decision), as an event at both edges with counters and identities,
  default off, tested on the simulator first (truth is known there, so the catch rate and the false releases can be
  measured), then on observed data against the counts of section 4.
- Decisions for the author: whether to pre-register that design; whether bar F4 is counted by distinct link; whether
  the population count of section 4 becomes a reported number of every day run.

## 2026-11-20: the three decisions answered; closed

Answered by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); applied by the docs consolidation on branch `docs/consolidate-2026-11-20`:
1. The monitor design was pre-registered and built: the windowed residual monitor of NRF section 132, trace only
   (`network.ring_fix.window_monitor`, default off; commit 3681795).
2. Bar F4 is counted by distinct link from now on, with the count of failing checks shown beside it. Earlier reads
   stand as they were written.
3. The population count of section 4 becomes a reported number of every day run: a tooling item owed by the run and
   grader sessions (recorded in NRF part 20).

Nothing else is owed by this card: closed.
