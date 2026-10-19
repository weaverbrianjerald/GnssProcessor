# NETCOSTB: the GPS + Galileo cost card, continued (the legacy reading and two corrections)

**STATUS**

- **id:** NETCOSTB
- **topic:** perf
- **status:** closed
- **summary:** Card NETCOST's continuation (that file is at the 60 KB bar), 2026-10-18. It decides the one reading that section 12.2 of card NETCOST left open: in the 31-epoch `legacy` arm of the 23-arm gate the station-add time (`est.addStation`) read 2.3 s with `network.sparse_station_normals` absent and 4.4 s with it on. The rise is load. The key-on side of that gate ran a loop of 60.4 s against 12.6 s, the sections the key does not touch were stretched 5.2 to 8.1 times and the station adds 1.9 times; in three controlled pairs on one executable the station-add time is 1.84 to 1.94 s with the key absent and 0.54 to 0.57 s with it on, the loop is 11.5 to 16.0 % shorter, and the output is identical. Two wording corrections to card NETCOST section 12 follow: `legacy` arms GPS and Galileo, and the stated cause of the load dependence of the wall ratio contradicts its own direction. Card NETCOST stays open and keeps the owed list (the arming decision, optimizations 2 and 3, a quiet-machine sim pair). CHECKED AGAIN 2026-10-18 (section 5, a fourth build, tree `df73700`): knob off identical to main on the 23-arm gate (24 of 24), the user side (16 of 16) and real time (wall-clock text only); knob on identical on 24 of 24 arms and on four full 2 h pairs; the attribution shares and the sub-section estimates reproduce (four sections 73.2 % of the field excess, 72.2 % on the sim bed); the gain is -13.4 % (field) and -10.5 % (sim); every figure of this card re-reads from the run directories; the card GAD ratios (2.9 and 2.0) trace to a GPS-only loop of 712.7 ms against 327 to 406 ms in every round measured, a load heavier than those rounds (section 5.4).
- **source:** [NETCOST](NETCOST_gps_galileo_cost_attribution_2026-10-18.md) section 12.2 (the `legacy` reading) and section 12.3 (the load dependence of the wall ratio); the gate runs `runs_nom/_rap_g5_{w3base,w3netcost2,w3ncon2}_legacy`.
- **knobs / posture:** none; `network.sparse_station_normals` stays default false, ledger UNVALIDATED, unarmed in both goldens. No source, golden or ledger change.
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** tree `perf/netcost` (code of `1e3e857`; the later commits change documents only); executable `gnss_processor_netcost.exe` under `D:/GnssProcessorSupport/gates/exe/x64-Release` (sha256 `d72d846658bd7a26`..., the bytes of `gnss_processor_netcost_cand.exe`); controlled runs `D:/GnssProcessorSupport/runs_nom/_rap_nc_lg_{off,on}_{1,2,3}`; scratch `D:/GnssProcessorSupport/gates/netcost_fix/` (`lg_setup.sh`, `lg_run.sh`, `lg_run.log`, `lg_read.py`); section 5: tree `df73700`, executable `gnss_processor_netcost_chk3.exe` (sha256 `02a342b392b485c4`...), scratch `D:/GnssProcessorSupport/gates/netcost_chk3/`, arms `_rap_g5_w3netcost3_*`, `_rap_g5_w3ncon3_*`, `_rap_nc_cc1_*`, `_rap_nc_cc3_*`, `_rap_nc_cc4_*`.

**Outcome.** Question: is the sparse station-normal formation slower than the dense one in the `legacy` arm of the gate,
where the station-add time read 2.3 s with the key absent and 4.4 s with it on? Result: no. The key-on run of that gate
arm was stretched by load (loop 60.4 s against 12.6 s, the untouched sections 5 to 8 times, the station adds 1.9 times).
In three controlled pairs the station adds take 0.54 to 0.57 s against 1.84 to 1.94 s and the loop is 11.5 to 16.0 %
shorter, with identical output. Standing: the open reading of card NETCOST section 12.2 is closed, and two wording
corrections to its section 12 are recorded in section 3. A third independent check (section 5) reproduced the identity, the gain and the attribution, and traced the lower card GAD ratios to a GPS-only denominator of a load beyond the rounds measured.

## 1. What this card continues

Card NETCOST reached 60 971 bytes with its second independent check (section 12), so what follows is recorded here (the
precedent of cards AGT and AGTB, FCF and FCFB). Section 12.2 of that card lists the sum of thread time of
`est.addStation` per gate arm, key absent against key on, and says that in `legacy` it reads 2.3 against 4.4 s, "which
the concurrent loads of the two sides do not decide". Every other arm that adds stations reads lower with the key on
(`f2` 133.8 to 68.0 s, `gate` 30.7 to 0.3 s). A formation that is slower in a small arm would matter for the arming
decision, so the reading is decided here.

## 2. The gate reading is load

The run-end timing lines of the `legacy` arm on the three sides of the gate (milliseconds over the 31-epoch run; the
control is the executable of main `30ca49e`, key absent):

| section | control | key absent | key on | key on over key absent |
| --- | --- | --- | --- | --- |
| epoch-loop wall | 43 089 | 12 554 | 60 369 | 4.81 |
| `est.addStation` | 9 233 | 2 346 | 4 372 | 1.86 |
| covariance inverse | 284 | 128 | 1 031 | 8.05 |
| reduce | 5 738 | 1 263 | 9 991 | 7.91 |
| other | 23 563 | 7 802 | 40 253 | 5.16 |

The sections of the key-on side that the key does not touch are 5.2 to 8.1 times those of the key-absent side, and the
loop is 4.8 times; the station adds are 1.9 times, less than half of that stretch, and their share of the loop falls from
18.7 % to 7.2 %. The three sides of the gate did not see the same machine (each side runs three jobs at a time on the 20
logical processors that other sessions use as well). A rise of the station-add time smaller than the stretch of the code
that did not change does not indicate a slower formation; the controlled pairs below decide it.

## 3. Three controlled pairs, and two corrections

**Pairs.** One executable (`gnss_processor_netcost.exe`) and the `legacy` arm of the gate, with the key absent and with
`network.sparse_station_normals` true; the two configs differ in that key and in the run directory paths only (compared as
parsed JSON). Run with `scripts/cost_attrib/run_timed.ps1` at above-normal priority with 18.5 to 18.7 other logical
processors busy throughout. Pairs 1 and 2 ran at the same time (launch order swapped); pair 3 one after the other.
Milliseconds over the run:

| pair | `est.addStation` key absent | key on | loop key absent | key on | loop change |
| --- | --- | --- | --- | --- | --- |
| 1 | 1 936 | 571 | 9 991 | 8 395 | -16.0 % |
| 2 | 1 857 | 566 | 9 380 | 8 298 | -11.5 % |
| 3 | 1 841 | 543 | 9 164 | 7 998 | -12.7 % |

The station adds are 3.3 to 3.4 times shorter with the key on in each pair. The sections the key does not touch read
alike on the two sides (reduce 1 065 to 1 200 ms, covariance inverse 90 to 124 ms, other 5 480 to 5 940 ms), and
`nom_td_gate.py --whole` reports IDENTICAL for each pair. The station-add time therefore falls in the `legacy` arm as
well, in 22 of the 22 arms that add stations.

**Correction 1.** Section 12.2 calls `legacy` "a small GPS arm". Its config arms GPS and Galileo
(`processing.active_systems`, `frequency_mode` dual), with 58 stations, 31 loop epochs and 638 station adds.

**Correction 2.** Sections 1 and 2 of card NETCOST and its Owed list say that the GPS-only loop "is stretched more by
load, so an idle machine reads lower". Section 12.3 shows that the observation does not support the cause. The direction
is also inverted: a denominator stretched more by load would put the ratio higher on an idle machine, not lower. The
lower idle ratio comes from the numerator. Field loop, ms per epoch, at 16 to 18, 5.5 to 11 and 2 foreign logical
processors: GPS-only 398, 330 and 370 (a spread of 20 % without a trend in the load), GPS + Galileo 2 117, 1 492 and
1 548. Those
sections of card NETCOST are not edited (the record is annotated, not rewritten); section 12.3 and this paragraph are
the annotation.

## 4. Reading

Nothing was armed, no golden was edited and no source changed. The owed items stay on card NETCOST: the arming decision,
optimizations 2 and 3, and a quiet-machine sim pair.

## 5. Third independent check of 2026-10-18 (gates w3netcost3 and w3ncon3)

A fourth build of the code, a fresh set of gate arms and fresh timing rounds, run after the commit of this card
(`df73700`). Tree under test: `perf/netcost` at `df73700`, checked out clean in worktree `netcost_chk3` (branch
`chk/netcost3`); `git diff 6b52c23 df73700` touches four Markdown files and no other path, and `git diff 1e3e857 df73700`
touches only `docs/` and `scripts/cost_attrib/`, so the code is that of card NETCOST sections 8, 11 and 12. Release
and Debug builds from scratch (862 steps each) finished with no error. Executable `gnss_processor_netcost_chk3.exe` under
`D:/GnssProcessorSupport/gates/exe/x64-Release` (sha256 `02a342b392b485c4`..., 21 326 848 bytes, the size of every
earlier build and other bytes; the gate side `w3netcost3` runs a byte copy). Control: side `w3base` (the executable of
main `30ca49e`, still the tip of `main`; the four real-time arm configs print no difference against it). New labels so
that earlier output stays where it is cited: G5 sides `w3netcost3` (key absent) and `w3ncon3` (key true in every arm),
user-side tag `w3netcost3`, real-time labels `w3netcost3_dr` and `w3netcost3_cl`. Scratch, logs and tables:
`D:/GnssProcessorSupport/gates/netcost_chk3/`; arms `_rap_g5_w3netcost3_*`, `_rap_g5_w3ncon3_*`, `_rap_nc_cc1_*`
(attribution), `_rap_nc_cc3_*` (knob pairs) and `_rap_nc_cc4_*` (sub-sections).

### 5.1 Knob off, identical

- **G5, 23 arms against `w3base`:** 24 IDENTICAL lines (23 arms and the snapshot bytes), summary
  `D:/GnssProcessorSupport/gates/g7/summary_w3netcost3.txt`, detail `D:/GnssProcessorSupport/gates/g5/gate_w3netcost3/`.
  Exit codes equal arm by arm on the two sides (`zero`, `joint`, `a4` and `gate` exit 1 on both); `products.txt` of `f2`
  (113 902 lines) and `s2` (80 704 lines) are byte-identical.
- **User side (`uom_gate.py`, quick set):** 16 of 16 IDENTICAL against `w3base`, every arm exit 0 (`uom_w3netcost3.log`).
- **Real time against `w3base`:** every artifact identical except wall-clock text, read line by line. Replay arms
  (`sbf`, `sbf_bssd`, `sbf_te`): 96 identical artifact lines; the one differing line per arm is the "Real-time session
  ended" line of `console.log`, equal in every character but the wall time (1 677, 2 263 and 3 741 ms on the control;
  1 637, 1 256 and 1 215 ms here). Closed-loop arm (`cl`): 37 identical artifact lines; four lines each in `console.log`
  and `main_log.log` differ: RINEX ingestion (7.765 against 2.238 s), the epoch-loop wall (1 509 003.8 against 34 108.0
  ms; the control ran with the machine at 100 % CPU), the round-trip self-check (2.04 against 0.12 s) and the cost tail
  of the ARC LEDGER line (audit 2 128.9 against 90.8 ms).
- **Debug build of the same tree:** the five tests of `NeqSparseStationNormalsTest` pass (10.4 s); the full suite is 3 343
  tests in 450 suites, 3 313 passed, 30 skipped (fixture-gated), 0 failed (880 s under load).
- **Scripts on the checked tree:** `docs_index_check.py`, `ledger_render.py --check`, `make_golden_config.py --verify`
  (76 recommended knobs armed, 2 documented exceptions, 0 failures per golden), `config_posture_check.py --verify-loader`
  (0 coverage gaps), `nom_epoch_lint.py` and `nom_n4_verify.py --whole` pass.

### 5.2 Knob on, identical, and the gain

- **G5 with `network.sparse_station_normals` true in every candidate arm, against `w3base` (key absent):** 24
  IDENTICAL lines (`D:/GnssProcessorSupport/gates/netcost_chk3/summary_w3ncon3.txt`), exit codes equal arm by arm
  (`zero`, `joint`, `a4` and `gate` exit 1 on `w3base`, `w3netcost3` and `w3ncon3` alike). The arm configs of the two
  sides differ by that one key in all 24 arm directories (`armdiff_w3ncon3.txt`: 24 arms, 0 unexpected); the
  comparison is the G5 comparison without its arm-config part. **This gate reads identity, not speed.** The key-on
  side was not run at the same time as the key-absent side (18:07 to 20:14, with other sessions' gates and this
  check's sub-section runs on the machine; its 2 h arm `f2` took 3 183 s against 727 s), so the summed thread time of
  `est.addStation` is load-confounded: it is lower with the key on in 13 of the 22 arms that add stations (for example
  `wkroci` 8.5 to 1.2 s, `a4` 6.1 to 0.45 s, `gate` 4.2 to 0.41 s) and higher in nine (`f2`, `s2`, `pdx`, `vce`,
  `dec`, `snapw`, `snapr`, `hand`, `legacy`), which this gate cannot decide; the add counts are equal on the three
  sides in all 22 arms. That the formation runs under the key is read from the pairs below, which share one executable
  and one load.
- **Four full 2 h pairs** (label `cc3`; key absent against key on, each pair run at the same time, launch order
  swapped in the second pair of each bed; the partner of a pair counts in the foreign load, 13.7 to 17.5 processors):
  `scripts/nom_td_gate.py --whole --no-arm-check` gives RESULT IDENTICAL for all four. Loop ms per epoch, key on
  against key absent: field 1 906 against 2 200 (-13.4 %) and 1 803 against 2 084 (-13.5 %), mean 1 855 against 2 142;
  sim from 00:30:15 974 against 1 089 (-10.6 %) and 969 against 1 080 (-10.2 %), mean 971 against 1 085. The assembly
  wall falls from 591 to 311 ms (field) and from 261 to 153 ms (sim); every other section moves by 5.1 ms or less on
  the field bed (the covariance consumers, inside its repeat spreads of 20 and 27 ms) and by 1.4 ms or less on the sim
  bed. The bar of more than 5 % on the field golden is met at 13.4 %. Tables `table_cc3_field_on.txt` and
  `table_cc3_sim_on.txt`.

### 5.3 The attribution reproduces

One round of the four arms of card NETCOST section 1 per bed in the order `ge g g ge` (`abba.sh`, label `cc1`, nothing
else of this check running), at 9.4 to 11.3 (field) and 5.6 to 9.6 (sim) foreign logical processors; no quiet reading
(2 or fewer) was available. Share of the total excess of `ge` over `g`, with the card:

| section | field `cc1` | field card | sim `cc1` | sim card |
| --- | --- | --- | --- | --- |
| assembly wall | 28.1 % | 28.0 % | 27.3 % | 25.9 % |
| covariance consumers | 20.6 % | 22.3 % | 16.7 % | 17.3 % |
| frame-diagnostic observers | 13.4 % | 12.1 % | 16.8 % | 16.4 % |
| reduce | 11.1 % | 11.1 % | 11.4 % | 12.4 % |
| solve glue | 10.9 % | 10.8 % | 6.8 % | 7.4 % |
| leveling harvest | 5.9 % | 6.1 % | 8.6 % | 8.3 % |
| the first four rows | 73.2 % | 73.5 % | 72.2 % | 72.0 % |

The six largest shares agree within 1.7 points on the field bed and 1.4 on the sim bed, and the four-section total
within 0.3. Loops, ms per epoch: field GPS-only 387 and 387 (foreign 9.4 and 9.8), GPS + Galileo 1 951 and 1 901 (11.3
and 10.3), wall ratio 4.98 and CPU ratio 2.04; sim GPS-only 270 and 268 (7.5 and 6.7), GPS + Galileo 961 and 923 (9.6
and 5.6), wall ratio 3.50 and CPU ratio 1.58. The GPS-only loop does not follow the load: 387 ms at 9.4 foreign
processors here, 388 ms at 16.3 in the instrumented run below, 327 and 332 at 5.7 and 370 and 384 at 1.9 and 7.1 in
card NETCOST. The GPS + Galileo field loop does: 1 901 to 1 951 ms here at 10 to 11, 2 084 and 2 200 in the pairs of
5.2 (14.4 and 16.9 foreign, the partner included), and 1 421 to 1 548 at 2 to 5.5 in card NETCOST. The field wall
ratio over all rounds is 4.2 (2 foreign), 4.5 (5.5 to 11), 5.0 (9 to 11), 5.1 (16 to 18) and 5.3 (17 to 18); the sim
ratio reads 3.5 to 3.7 at 6 to 17 (about 2.6 for the single run at 5.2 in card NETCOST section 12.3).

**Sub-sections** (label `cc4`, the scratch timing executable of card NETCOST section 3, sha256 `e528ce9ea733446d`...,
15.7 to 17.8 foreign processors, loops 1 951 and 388 ms per epoch on the field bed, 966 and 262 on the sim bed). The
station dimensions reproduce (field `m` 56.4 and 93.0, `n_s` 213.9 and 418.3, `n_r` 28.4 and 48.5; sim 49.4 and 89.3,
156.9 and 279.0, 27.4 and 49.1). Field bed, GPS + Galileo, ms per epoch, with the figures of card NETCOST section 3
and of section 12.5: shared product 349.3 (370.9; 367.9), symmetry check 50.3 (69.2; 83.4),
`pureConditionalLocalVarianceDiag` 192.2 (212.1; 206.6), `conditionalLocalVarianceDiag` 113.7 (140.5; 130.3) of which
the copy 58.7 (75.1; 66.6), reduce subtract 171.1 (179.5; 181.6), local solves 47.6 (50.0; 50.1), redundancy census
194.7 (201.1; 212.0), post-fit sigma0 and the RHS trip 59.2 (63.0; 63.8), joint conditional covariance 23.2. As a
share of the loop the work addressed by change 1 (shared product and symmetry pass) is 20.5 %, by change 2 (the pure
term and the covariance copies) 12.9 % and by change 3 (the reduce subtract, the census and the joint conditional
covariance) 19.9 %, against the estimates of 18 to 21 %, 12 to 13 % and 8 to 12 % of card NETCOST section 5. On the
sim bed the same work is 7.0 % for change 2 and 21.6 % for change 3 (the border is 279 columns), so the estimate of
change 2 is a field figure.

### 5.4 The statements of this card, checked

- **Section 2 (the `legacy` arm on the gate sides):** the fifteen figures re-read from the run-end lines of
  `_rap_g5_{w3base,w3netcost2,w3ncon2}_legacy` agree with the table, and so do its ratios (4.81, 1.86, 8.05, 7.91, 5.16).
- **Section 3 (the controlled pairs):** the twelve figures of the table and the three ranges of the sections the key
  does not touch, re-read from `_rap_nc_lg_*` (D0 lines and `timing.txt`), agree with the text; the loop changes
  recompute to -16.0, -11.5 and -12.7 %, and the other-load figure is 18.5 to 18.7 throughout. The two arms of a pair
  differ in `network.sparse_station_normals` and the run-directory paths alone (parsed JSON); the config arms GPS and
  Galileo (`frequency_mode` dual) for 58 stations, as correction 1 states.
- **The 22 arms:** the sums of `est.addStation` thread time on the three gate sides give the key-on figure below the
  key-absent one in 21 of the 22 arms that add stations, the exception being `legacy`, which section 3 decides by the
  pairs; the add counts are equal on the three sides in all 22.
- **Correction 2:** the loop figures it quotes (GPS-only 398, 330, 370 and GPS + Galileo 2 117, 1 492, 1 548 ms per epoch
  at 16 to 18, 5.5 to 11 and 2 foreign processors) re-read from the run directories, and 5.3 adds a fifth load level
  that agrees with it.
- **Code lines of card NETCOST section 12.6:** the goldens set `eigen_threads` 20 at field line 38 and sim line 37;
  `ApplicationNetwork.cpp` pins Eigen to one thread at line 303 and restores it at lines 333 and 351; the sparse
  formation refuses any other thread count at `NormalEquations.cpp` line 130.
- **Executable of section 1:** `gnss_processor_netcost.exe` and `gnss_processor_netcost_cand.exe` are the same bytes
  (sha256 `d72d846658bd7a26`...).
- **Card GAD ratios (a statement the rounds neither support nor refute).** Section 2 of card NETCOST puts the lower
  ratios of card GAD (2.9 and 2.0) down to a GPS-only denominator stretched by five arms sharing the machine. The numbers
  fit the direction: card GAD read the GPS-only loop at 712.7 ms (field) and 649.6 ms (sim), against 327 to 406 ms and
  258 to 299 ms in every round of card NETCOST and of this check, while its GPS + Galileo loop (2 083.8 ms field) equals
  the 2 084 to 2 200 ms read here at 14 to 17 foreign processors. The GPS-only loop shows no stretch over 2 to 18 foreign
  processors, so the card GAD denominator belongs to a heavier load than any round measured (five concurrent arms) or to
  a different GPS-only configuration; a GPS-only arm run next to four others is the missing test.

### 5.5 Reading

- **Pass criteria.** Everything off is identical to `w3base` on the three gates (24 of 24, 16 of 16, real time up to
  wall-clock text), the key on is identical on 24 of 24 arms and on four full 2 h pairs, the attribution shares and the
  sub-section estimates reproduce, and the gain on the field golden is 13.4 % against the bar of 5 %.
- **Owed, unchanged:** the arming decision, optimizations 2 and 3, and a quiet-machine pair on the sim bed (no reading at
  2 or fewer foreign processors was available in this check; the machine held 5.6 or more throughout).
