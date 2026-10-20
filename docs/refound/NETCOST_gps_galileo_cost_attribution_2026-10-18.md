# NETCOST: where the GPS + Galileo epoch time goes

**STATUS**

- **id:** NETCOST
- **topic:** perf
- **status:** open
- **summary:** MEASURED and one change BUILT 2026-10-18. The GPS + Galileo epoch loop is 5.3 times the GPS-only loop on the field golden and 3.7 times on the sim golden (above-normal priority on a shared machine; a second round agrees within 0.1 on the ratio). Four sections carry 73 % of the field excess (assembly wall, covariance consumers, frame-diagnostic observers, reduce): dense algebra on the shared border, whose dimension doubles (214 to 418). `network.sparse_station_normals` (default off, UNVALIDATED, not armed) forms the station normals from the nonzero products in Eigen's own summation order: the field loop is 13.3 % shorter and the sim loop 10.6 %, identical on the 2 h runs and on the 23-arm gate with the knob on in every arm (24 of 24 identical). Optimizations 2 and 3 (estimates 12 to 13 % and 8 to 12 %) and the arming decision are owed. CHECKED 2026-10-18 (section 11, a separate rebuild): knob off identical to main on the 23-arm gate (24 of 24), the user side (16 of 16) and real time (wall-clock text only); knob on identical on the same 23 arms (24 of 24) and on four full 2 h pairs; the section shares reproduce (four sections 73.1 % on the field bed) while the wall ratio moves with the load (4.5 and 2.9 at a lighter load); the gain reproduces (field -13.3 %, sim -9.8 %). Correction: the goldens set `processing.eigen_threads` 20, and the sparse formation runs because the parallel network loop pins Eigen to one thread. CHECKED AGAIN 2026-10-18 (section 12, third rebuild, tree `6b52c23`): knob off identical to main on the 23-arm gate (24 of 24), the user side (16 of 16) and real time (wall-clock text only); knob on identical on 24 of 24 arms with the sparse formation shown to run; the section shares reproduce on two more rounds (four sections 73.4 % in a loaded round and 72.2 % in a quiet pair); the gain is -13.8 % (field) and -10.6 % (sim); the wall ratio is a function of the load and it is the GPS + Galileo loop that moves with it (field 4.2 with 2 foreign logical processors, 5.1 with 17).
- **source:** [GAD](GAD_galileo_adoption_2026-10-13.md) section 5 (cost row: field 2.9 and sim 2.0 times, a wall-time mean, per-section attribution owed); [NOM4](NOM4_network_object_model_finish_2026-10-15.md) section "the cost pair" (sim +3.3 %, field +0.7 %); the register item "GPS + Galileo cost" ([OPEN_ITEMS](../status/OPEN_ITEMS.md)).
- **knobs / posture:** new `network.sparse_station_normals` (bool, default false, ledger UNVALIDATED, unarmed in both goldens); no other knob changed.
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** base main `30ca49e`; branch `perf/netcost`; base exe `gnss_processor_netcost_base.exe` and candidate exe `gnss_processor_netcost_cand.exe` under `D:/GnssProcessorSupport/gates/exe/x64-Release`; scripts, tables, logs and gate summaries `D:/GnssProcessorSupport/gates/netcost/`; runs `D:/GnssProcessorSupport/runs_nom/_rap_nc_*`; tools `scripts/cost_attrib/`.

**Outcome.** Question: where does the GPS + Galileo epoch time go, and can part of it be removed without moving a bit of output? Result: the loop is 5.3 times (field) and 3.7 times (sim) the GPS-only loop under this machine's load; 73 % of the field excess is four sections doing dense algebra on the shared border. The first of three optimizations (station normals from the nonzero entries, in Eigen's summation order) shortens the field loop 13.3 % and the sim loop 10.6 %, with no output byte moved (bars: identical, more than 5 % on the field golden: met). Standing: built default off, ledger UNVALIDATED, unarmed; the other two optimizations are owed (section 5), and the NOM4 sim cost of +3.3 % did not reproduce (section 10). A separate rebuild reproduced the identity, the gain and the section shares (section 11), and a second one did again, reading the wall ratio against the machine's load (section 12).

## 1. Question and method

Where does the extra time of a GPS + Galileo golden arm go, and which part of it can be removed without moving a
bit of output? Four arms of 2 h on the Release exe built from main `30ca49e`: the field golden (58 stations, 240
epochs) and the sim golden from 00:30:15 (60 stations, 241 epochs), each as it is (GPS + Galileo, `ge`) and with
`processing.active_systems` `["GPS"]` and nothing else changed (`g`; the Galileo package keys of the golden stay on
and have nothing to act on). Arms are made by `scripts/rapid_arm.py` and run one at a time, in the order `ge g g ge`,
by `scripts/cost_attrib/abba.sh`. The sections are the run-end timing report the goldens already print
(`network.trace_section_timers`): the assembly wall, reduce, solve, cov-inverse and cov-consumers of the D0 table,
then the D0b sub-blocks (each net of the D0 sections already counted); together they tile the epoch-loop wall.
`scripts/cost_attrib/nc_table.py` turns the repeats into the tables of section 2: mean ms per epoch of each variant
with the spread over the repeats, the ratio, the excess of `ge` over `g` as points of the total ratio, and its share
of the total excess.

**The machine.** It was shared with other sessions' gate runs and builds for the whole measurement: the processor
queue held 150 to 180 threads on 20 logical processors, and a normal-priority run of ten epochs took 337 s where
the epoch loop at above-normal priority took 22 s. Every measurement run therefore runs at above-normal priority
(`scripts/cost_attrib/run_timed.ps1`, which also records the process CPU time and the load of the rest of the machine,
16 to 18 logical processors throughout). At that priority the two repeats of a run agree within 1 % (field) and 2 %
(sim) in the loop time. The ratios below are ratios under that load, not quiet-machine figures: the GPS-only loop is
the more serial one and is stretched more by load, so an idle machine reads lower (see section 2).

## 2. The ratio, section by section

Field golden, mean of two repeats, ms per epoch (spread = largest minus smallest of the two):

      g: 2 runs, loop ms/epoch 400 397 (mean 398, spread 3); process CPU s 520 507; other processes' CPU (logical procs) 16.2 15.8
     ge: 2 runs, loop ms/epoch 2128 2107 (mean 2117, spread 21); process CPU s 1198 1183; other processes' CPU (logical procs) 17.9 17.9
    wall ratio (numerator/denominator) = 5.31
    CPU ratio (numerator/denominator) = 2.32
    
    section          g ms/ep (spread)  ge ms/ep (spread)  ratio   excess    pts   share
    assembly-wall       92.3      0.5     573.8      1.8   6.21    481.5   1.21   28.0%
    cov-consumers       39.7      0.4     423.1      2.2  10.65    383.4   0.96   22.3%
    frame-diag          60.5      1.1     267.7      1.2   4.42    207.2   0.52   12.1%
    reduce              38.7      0.4     229.0      0.1   5.92    190.3   0.48   11.1%
    solve-glue          36.4      0.5     222.3      0.4   6.11    185.9   0.47   10.8%
    leveling            39.2      0.1     143.2      3.7   3.66    104.1   0.26    6.1%
    pre-asm             16.3      0.0      65.3      0.0   4.00     49.0   0.12    2.8%
    cov-inverse          4.4      0.0      31.7      0.7   7.15     27.2   0.07    1.6%
    slip-scans          25.0      0.2      49.7      0.7   1.99     24.7   0.06    1.4%
    roster               8.9      0.0      24.0      0.1   2.71     15.2   0.04    0.9%
    emit-store           6.8      0.0      20.9     18.0   3.08     14.1   0.04    0.8%
    stash-status         7.0      0.2      14.2      0.0   2.02      7.2   0.02    0.4%
    solve                1.0      0.0       6.7      0.1   7.02      5.8   0.01    0.3%
    raim                 5.2      0.4      10.9      0.2   2.09      5.7   0.01    0.3%
    sol-prep             3.4      0.1       8.8      0.1   2.56      5.3   0.01    0.3%
    iono-diag            4.7      0.2      10.0      2.3   2.14      5.3   0.01    0.3%
    dd-frac              3.1      0.1       6.7      1.1   2.20      3.7   0.01    0.2%
    unattributed         5.9      0.2       9.1      0.1   1.54      3.2   0.01    0.2%
    t1a-health           0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    gauge-build          0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    netar                0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    install-post         0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    clock-upd            0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    shdev-vtec           0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    dd-build             0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    TOTAL              398.4             2117.1            5.31   1718.7   4.31  100.0%

Sim golden from 00:30:15:

      g: 2 runs, loop ms/epoch 282 283 (mean 283, spread 1); process CPU s 457 468; other processes' CPU (logical procs) 15.7 15.9
     ge: 2 runs, loop ms/epoch 1049 1027 (mean 1038, spread 22); process CPU s 807 796; other processes' CPU (logical procs) 17.2 17.3
    wall ratio (numerator/denominator) = 3.67
    CPU ratio (numerator/denominator) = 1.73
    
    section          g ms/ep (spread)  ge ms/ep (spread)  ratio   excess    pts   share
    assembly-wall       53.0      0.4     248.4      1.9   4.68    195.4   0.69   25.9%
    cov-consumers       28.7      0.5     159.1      8.6   5.54    130.4   0.46   17.3%
    frame-diag          46.3      0.1     170.0      4.0   3.67    123.7   0.44   16.4%
    reduce              22.8      0.2     116.6      0.3   5.11     93.8   0.33   12.4%
    leveling            29.5      0.2      92.5      4.9   3.14     63.0   0.22    8.3%
    solve-glue          17.6      0.0      73.4      0.4   4.17     55.8   0.20    7.4%
    slip-scans          24.4      0.2      47.0      2.1   1.93     22.6   0.08    3.0%
    pre-asm             11.9      0.3      29.3      1.0   2.45     17.3   0.06    2.3%
    roster               7.3      0.1      18.7      0.6   2.57     11.4   0.04    1.5%
    cov-inverse          1.9      0.1       9.6      0.0   4.93      7.6   0.03    1.0%
    emit-store           6.6      0.0      13.6      2.8   2.06      7.0   0.02    0.9%
    raim                 5.4      1.0      11.2      1.1   2.09      5.8   0.02    0.8%
    dd-frac              8.1      0.0      13.5      1.5   1.67      5.4   0.02    0.7%
    stash-status         6.2      0.1      11.3      0.4   1.82      5.1   0.02    0.7%
    iono-diag            4.6      0.1       8.3      0.6   1.82      3.8   0.01    0.5%
    sol-prep             2.9      0.1       6.1      0.0   2.15      3.3   0.01    0.4%
    unattributed         5.3      0.2       7.4      0.5   1.40      2.1   0.01    0.3%
    solve                0.4      0.0       2.0      0.1   4.51      1.6   0.01    0.2%
    t1a-health           0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    netar                0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    gauge-build          0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    install-post         0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    dd-build             0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    clock-upd            0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    shdev-vtec           0.0      0.0       0.0      0.0    nan      0.0   0.00    0.0%
    TOTAL              282.8             1038.0            3.67    755.2   2.67  100.0%

Reading the tables:

- The loop is 5.3 times longer with Galileo on the field bed (398 to 2 117 ms per epoch) and 3.7 times on the sim
  bed (283 to 1 038 ms). Card GAD's 2.9 and 2.0 were read with five arms sharing the machine, which stretched the
  GPS-only arm that is the denominator; the process CPU time ratio, which includes the start-up that does not
  scale, is 2.3 (field) and 1.7 (sim). A statement of the cost needs its load: 2 to 5 times on this machine.
- Four sections carry 73 % of the excess on the field bed: the assembly wall (28 %, of which the serial scatter
  `est.addStation` is 93 %), the covariance consumers (22 %), the frame-diagnostic observers (12 %) and reduce
  (11 %). The sim bed has the same four (26, 17, 16, 12 %) plus the leveling harvest (8 %).
- A second round of the same four arms, hours later (`r2`), reproduces the first: loop 392 and 2 042 ms per epoch on
  the field bed (ratio 5.21), 258 and 953 ms on the sim bed (3.70); the share of each of the six largest sections
  moved by at most 0.4 points on the field bed and 1.5 points on the sim bed (the covariance consumers).
- Only a few sections grow with the satellite count alone: the slip scans (2.0 times), the roster (2.7) and the
  post-solve bookkeeping (about 2). Everything that grows 4 to 11 times works on the shared border (the satellite
  clock and bias columns) with dense algebra.

## 3. Inside the big sections

A second executable carries a scratch timing patch (named wall-clock accumulators in the station add, the
covariance consumers, the reduction and the observer table; it is not in the tree and is kept with the run files).
One run of each variant on each bed gives the size of a station's normal equations and the cost of their parts
(`scripts/cost_attrib/instr_summary.py`; its sections agree with the table above to within the timer overhead).

| mean per station | field G | field GE | sim G | sim GE |
| --- | --- | --- | --- | --- |
| rows `m` | 56 | 93 | 49 | 89 |
| shared columns `n_s` (satellite clocks and biases of the whole network) | 214 | 418 | 157 | 279 |
| local columns `n_r` | 28 | 49 | 27 | 49 |

Field bed, ms per epoch. The first six lines are 66 % of the GPS + Galileo loop.

| part | G | GE | ratio | what it is |
| --- | --- | --- | --- | --- |
| `est.addStation`, `A_sᵀ W A_s` | 53.7 | 370.9 | 6.9 | one dense `n_s` x `m` x `n_s` product per station |
| `est.addStation`, symmetry check of it | 9.9 | 69.2 | 7.0 | one transposed pass over the `n_s` x `n_s` matrix |
| `est.addStation`, the rest (`A_sᵀ W A_r`, `A_rᵀ W A_r`, border update, factorization) | 16.0 | 91.8 | 5.7 | |
| `pureConditionalLocalVarianceDiag` | 17.8 | 212.1 | 11.9 | per local column, a matrix-vector product with an all-zero `n_s` x `n_s` matrix |
| `conditionalLocalVarianceDiag` | 7.1 | 140.5 | 19.9 | of which 75.1 ms is the copy of the shared covariance (2.0 in G) |
| reduce, `N_ss -= N_sr (N_rr⁻¹ N_rs)` | 28.2 | 179.5 | 6.4 | a dense product whose left factor is mostly zero |
| reduce, the local solves | 10.4 | 50.0 | 4.8 | |
| redundancy census (`networkRedundancyCensus`) | 29.3 | 201.1 | 6.9 | measure-only, runs every epoch |
| post-fit sigma0 and the RHS attribution trip | 27.7 | 63.0 | 2.3 | measure-only |

The sim bed has the same structure (`est.addStation` 42.0 to 219.5, the pure conditional variance 11.1 to 69.1, the
reduce product 15.1 to 83.5, the redundancy census 18.4 to 110.0).

## 4. Why the cost grows faster than the satellite count

The shared dimension doubles (214 to 418), so every dense `n_s` x `n_s` object quadruples, and each is touched once
per station and per call; the rows and locals grow 1.7 times on top, which gives the 6 to 7 times of the dense
products. The two largest items grow faster than that, for two reasons seen in the numbers.

- **A copy of the shared covariance leaves the cache and the allocator's fast path.** It is 1.4 MB at GPS +
  Galileo on the field bed and 0.37 MB at GPS only; a copy costs 647 against 17 microseconds (38 times for a
  doubling of the dimension, where the quadrupling accounts for 4). The sim bed (0.62 MB against 0.20 MB) shows 7
  times. The step is consistent with a block of about 508 KB or more being served by the virtual-memory manager,
  with page faults and zero fill on every allocation, which is also why 32 % of the GPS + Galileo process time is
  kernel time (380 s of 1 200 s on the 2 h field arm). The allocator threshold itself was not isolated: the
  standalone copy benchmark was unreadable on the loaded machine.
- **A product with an all-zero matrix.** `pureConditionalLocalVarianceDiag` multiplies each local column's border
  regression by a zero `n_s` x `n_s` matrix (the border is treated as known), 212 ms per epoch to add exactly zero.

The dense products of the station also work on matrices that are mostly zero, but less so than the data rows
suggest. A probe in the real run (field golden, ten epochs) found 57 nonzeros per row of `A_s` on average (4 323 in
75 rows) and a longest row of 152: besides the data rows, which touch 3 to 5 shared columns, the rows appended after
them (the block rows, `networkGrowBlockRows`) are dense across the border. The nonzero products are 13 % of the
dense product's, which sets how fast a sparse formation has to be per term (section 6).

## 5. Three optimizations, with the gain expected on the field golden

Estimates are the measured sub-section times above, as a share of the 2 127 ms loop. Each keeps every output bit.

| # | change | removes | estimate | exactness argument |
| --- | --- | --- | --- | --- |
| 1 | station normals from the nonzero entries, in Eigen's own summation order (implemented, section 6) | `est.addStation` dense products and the symmetry pass: 533 ms, of which about 450 ms | 18 to 21 % | bit comparison with Eigen's product (unit test) and the 23-arm gate |
| 2 | read the border-known conditional variance directly and reuse the datum-frame covariance across the consumers | `pureConditionalLocalVarianceDiag` 212 ms to about 15 ms, and the 75 ms of copies in `conditionalLocalVarianceDiag` | 12 to 13 % | the product with an exact zero matrix adds +-0 to the column's own variance; a cache of the zeroed copy is the same bytes |
| 3 | extend the order model to the other products with a sparse operand: the reduce subtract (180 ms), the redundancy census (201 ms, measure-only) and the joint conditional covariance (26 ms) | the dense work on zeros in each | 8 to 12 % | the same model with the left factor sparse; one unit test per product |

The three together would take the field loop from 2 127 to about 1 100 ms per epoch. Change 1 was implemented and
measured (sections 6 to 8): 13 % on the field golden, 11 % on the sim golden. Of the rest, the slip scans, the
leveling harvest's per-column solves and the fixed per-station work scale with the satellite count and are not
dense in the border.

## 6. The change: station normals from the nonzero entries

`network.sparse_station_normals` (default false) makes the diagonal-weight `addStationObservations` form the three
blocks `A_sᵀ W A_s`, `A_sᵀ W A_r` and `A_rᵀ W A_r` from the nonzero products of the station's rows instead of by dense
products over the whole shared border. It also drops the three full passes over the `n_s` x `n_s` matrix that
followed (the symmetry check, the allocation of a fresh result, the addition into the border): the border is
updated on the square of the shared columns the rows touch, and the scratch matrix is kept all zero between stations.
A shape the change does not cover takes the dense path for that station; the counters of the run-end report and
every other output are unchanged. The weight path with a full covariance (a non-diagonal `W`) is not touched.

**Why this is bit-identical.** The obvious sparse sum (every nonzero product in ascending row order) is not: a
standalone comparison against Eigen's product on 600 random shapes, with a guard that only asked for a depth within
one panel, left 78 shapes with differing elements. The dense products run Eigen's blocked kernel
(`GeneralBlockPanelKernel.h`, SSE2 doubles, no fused multiply-add in this build) and its summation order has one
exception to "ascending from +0": an element in the 2-row tile of its row panel and in a 4-column block of its column
panel keeps two accumulators (even and odd depth index below `depth & ~7`), adds them, and then continues with the
remaining rows. The rows are cut into panels of `mc` and tiled by 4, then one 2-row tile, then a single row; the
columns into panels of `nc` and blocks of 4. `mc` and `nc` come from Eigen's own blocking heuristic
(`computeProductBlockingSizes`), which for the field shapes cuts 418 rows into panels of 212 or 418. The sparse code
reproduces exactly that order for the elements it touches; a skipped term is a zero product, which adds +-0 to a sum
that starts at +0 and so cannot be -0.

The model was checked against Eigen's product before it was used: 14 126 products (rows 2 to 420, columns 2 to 150,
depth 1 to 257, sparse rows and rows with dense columns) and 34.8 million elements, 438 352 of them in the
two-accumulator order, with no element different (`D:/GnssProcessorSupport/gates/netcost/exact/model_test.cpp`). The unit test
`tests/network/test_neq_sparse_station_normals.cpp` repeats the comparison at every residue of the tile sizes and at
the network's own shapes (about 420 shared columns, 50 locals, 90 rows), once with a third of the rows made dense
across the border as the carried priors make them; it checks that nothing outside the reported square is nonzero and
that the two symmetry numbers are the ones the full scan takes, and it runs two kernels (dense and sparse) through
stations, a rejected station and a reduction to bitwise equal state. It fails if Eigen's kernel changes.

**What makes the change step back to the dense products** (each tested): an Eigen thread count other than one
(`processing.eigen_threads` is 1 by default and in the goldens; 0 selects the OpenMP default and takes the dense
path), a result that is a vector or a tiny size sum, a depth over the panel depth, a kernel geometry other than the
described one, a build with fused multiply and add (`/arch:AVX2`), and a non-finite entry, weight or weighted product.

**How the sums are formed, and why the first versions lost.** The products of one row are the outer product of its
nonzero left entries with its nonzero right entries, added in row order. Two ways to add them were timed in the real
run. As scatter-adds into the full matrix a product costs 1.2 ns (index, multiply, add, store), against about 0.4 ns
per product inside the dense kernel (371 ms per epoch for 58 stations of 93 x 418 x 418 products, zeros included);
as a contiguous multiply-add of a packed row onto a result column it costs 0.35 ns per element and vectorises. A settled field station holds about 27 rows dense across the border (the block
rows of the carried priors: some 400 nonzeros among 408 touched columns) next to its data rows of 3 to 5 nonzeros,
so most products sit in dense rows. The first version (one reported entry per product, the symmetry check by a
transposed pass) was 6 times slower than the dense path on the field golden; the second, with the scan and the check
reduced to the touched square, gained 4 % of the loop on a settled run (the dense rows still went through the
scatter); the third packs a row that fills at least a third of the result's rows into a contiguous vector and
accumulates `N_ss` on the compressed square of the touched columns, which is what the figures below measure.

## 7. Result

Four pairs of 2 h goldens, each pair run at the same time on the same executable (the knob off in one member and on
in the other, the launch order swapped in the second pair of each bed), so that both members see the load of the shared
machine (other sessions' processes held 17 to 18 logical processors throughout). In the tables `on` is the knob on
and `ge` the golden as it is; the ratio is `ge` over `on` and the excess `ge` minus `on`. Field golden, ms per epoch:

     on: 2 runs, loop ms/epoch 1807 1813 (mean 1810, spread 6); process CPU s 1090 1078; other processes' CPU (logical procs) 17.6 17.8
     ge: 2 runs, loop ms/epoch 2071 2102 (mean 2087, spread 31); process CPU s 1146 1146; other processes' CPU (logical procs) 17.9 17.9
    wall ratio (numerator/denominator) = 1.15
    CPU ratio (numerator/denominator) = 1.06
    
    section            on ms/ep (spread)    ge ms/ep (spread)  ratio   excess    pts   share
    assembly-wall         294.0      3.9       569.3      3.4   1.94    275.3   0.15   99.4%
    cov-consumers         397.3     10.0       400.8     20.0   1.01      3.6   0.00    1.3%
    leveling              137.2      3.5       137.9      7.0   1.01      0.7   0.00    0.2%
    raim                   10.6      0.2        10.9      0.3   1.03      0.3   0.00    0.1%
    pre-asm                66.7      1.2        67.0      1.5   1.01      0.3   0.00    0.1%
    sol-prep                8.3      0.0         8.5      0.4   1.03      0.2   0.00    0.1%
    solve                   6.8      0.0         7.0      0.1   1.02      0.2   0.00    0.1%
    solve-glue            224.1      0.9       224.1      3.3   1.00      0.0   0.00    0.0%
    t1a-health              0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    stash-status           14.1      0.2        14.1      0.5   1.00      0.0   0.00    0.0%
    gauge-build             0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    netar                   0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    install-post            0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    clock-upd               0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    shdev-vtec              0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    dd-build                0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    iono-diag               8.4      0.2         8.4      0.3   1.00     -0.0  -0.00   -0.0%
    roster                 23.4      0.0        23.4      0.1   1.00     -0.0  -0.00   -0.0%
    dd-frac                 8.0      4.1         8.0      4.5   0.99     -0.0  -0.00   -0.0%
    slip-scans             51.7      0.2        51.5      2.4   1.00     -0.1  -0.00   -0.0%
    cov-inverse            31.8      0.1        31.7      0.6   1.00     -0.1  -0.00   -0.0%
    reduce                230.4      3.0       230.1      4.0   1.00     -0.3  -0.00   -0.1%
    emit-store             13.4      3.1        12.9      2.1   0.96     -0.5  -0.00   -0.2%
    unattributed           10.9      4.8        10.3      3.6   0.95     -0.6  -0.00   -0.2%
    frame-diag            272.8      3.9       270.6      1.1   0.99     -2.1  -0.00   -0.8%
    TOTAL                1809.8               2086.6            1.15    276.8   0.15  100.0%

Sim golden from 00:30:15:

     on: 2 runs, loop ms/epoch 981 974 (mean 977, spread 7); process CPU s 757 773; other processes' CPU (logical procs) 17.3 17.1
     ge: 2 runs, loop ms/epoch 1095 1090 (mean 1093, spread 5); process CPU s 803 798; other processes' CPU (logical procs) 17.4 17.3
    wall ratio (numerator/denominator) = 1.12
    CPU ratio (numerator/denominator) = 1.05
    
    section            on ms/ep (spread)    ge ms/ep (spread)  ratio   excess    pts   share
    assembly-wall         152.9      0.5       264.5      0.8   1.73    111.6   0.11   96.8%
    cov-consumers         167.3      6.6       170.0      8.0   1.02      2.7   0.00    2.3%
    leveling               99.3      2.1       100.6      1.4   1.01      1.3   0.00    1.1%
    pre-asm                31.5      0.4        32.2      0.3   1.02      0.7   0.00    0.6%
    roster                 18.5      0.5        19.2      0.3   1.04      0.6   0.00    0.6%
    stash-status           11.4      0.2        11.8      0.1   1.04      0.4   0.00    0.4%
    frame-diag            176.6      5.9       176.9      0.6   1.00      0.3   0.00    0.3%
    slip-scans             50.4      1.2        50.6      1.1   1.00      0.2   0.00    0.2%
    raim                   10.3      0.2        10.5      0.4   1.02      0.2   0.00    0.1%
    dd-frac                13.8      1.3        14.0      0.5   1.01      0.2   0.00    0.1%
    solve                   2.1      0.1         2.1      0.1   1.00      0.0   0.00    0.0%
    t1a-health              0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    netar                   0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    gauge-build             0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    shdev-vtec              0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    clock-upd               0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    dd-build                0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    install-post            0.0      0.0         0.0      0.0    nan      0.0   0.00    0.0%
    iono-diag               8.0      0.3         7.9      0.1   0.99     -0.1  -0.00   -0.1%
    cov-inverse            10.1      0.6        10.0      0.5   0.99     -0.1  -0.00   -0.1%
    unattributed            8.3      1.9         8.1      1.1   0.98     -0.2  -0.00   -0.1%
    sol-prep                6.4      0.1         6.2      0.2   0.96     -0.3  -0.00   -0.2%
    solve-glue             76.4      1.0        75.9      0.5   0.99     -0.5  -0.00   -0.4%
    reduce                121.4      0.3       120.7      0.5   0.99     -0.8  -0.00   -0.7%
    emit-store             12.7      2.3        11.7      1.2   0.92     -1.0  -0.00   -0.9%
    TOTAL                 977.5               1092.8            1.12    115.3   0.12  100.0%

The field loop is 13.3 % shorter (1 810 against 2 087 ms per epoch; the repeats agree within 6 ms and 31 ms) and the
sim loop 10.6 % shorter (977 against 1 093). The assembly wall falls from 569 to 294 ms (field) and from 265 to 153 ms
(sim); every other section stays inside the spread of its repeats. The process CPU time falls by 5 to 6 %, less
than the wall, because the removed work was serial and the rest of the process time is spent by the parallel
sections. The estimate of section 5 was 18 to 21 %; the difference is the formation that stays. It was not
decomposed again after the packed accumulation (in the version before it, the accumulation was 5.75 of the 6.56 ms
per station of the formation, and the dense block rows were most of its products).

The four pairs also serve as the full-length identity check: for each pair `products.txt` (113 902 lines on the
field bed, 80 704 on the sim bed), the trace CSVs, the run manifest, `stdout.txt`, the main log, the real-time
log and the exit code are IDENTICAL between the knob-off and the knob-on member (`scripts/nom_td_gate.py --whole
--no-arm-check`).

## 8. Checks

- Unit tests: five in `NeqSparseStationNormalsTest`, in about 9 s of Debug time; the full Debug suite on the final
  source gives 3 313 passed, 30 skipped (the fixture-gated skips of the suite). An earlier full run on a loaded
  machine had one failure, `TcpClientSilenceTest.SilentPeerIsDroppedAndRedialed` (a redial count of 1 against 2, a
  timing assertion); it passes alone and in the final run and does not touch this code.
- Ten-epoch Release smoke of the final executable, field and sim, knob off and on: IDENTICAL on every comparison
  of the whole-run gate (`products.txt`, manifest, logs, exit code); `est.addStation` falls 76 % (field) and 72 %
  (sim) at that early, sparse state.
- Knob off, no key set: the final executable against the executable of main `30ca49e`, ten-epoch field and sim
  golden arms: IDENTICAL on every comparison of the whole-run gate (the knob-off path is the dense path behind a
  forwarding function).
- `python scripts/nom_epoch_lint.py` PASS; `python scripts/nom_n4_verify.py --whole` PASS;
  `python scripts/make_golden_config.py --verify` PASS (both goldens, 76 recommended knobs armed, 2 documented
  exceptions, 0 failures); `python scripts/config_posture_check.py --verify-loader` 0 coverage gaps;
  `python scripts/ledger_render.py --check` clean.
- The 23-arm gate with the knob set in every arm of both sides (the reference side runs the executable of main
  `30ca49e`, which ignores the key; the candidate side runs the final executable with it on):
  24 of 24 IDENTICAL (the 23 arms and the snapshot bytes; summary `D:/GnssProcessorSupport/gates/netcost/summary_nco.txt`, per-arm output `D:/GnssProcessorSupport/gates/g5/gate_nco/`). All 24 candidate arm configs carry the key set true; four arms (`zero`, `joint`, `a4`, `gate`) exit 1 on both sides, and the exit code is part of the comparison.


## 9. Reproduction

```
# tables of section 2 (above-normal priority is the default of run_timed.ps1)
bash scripts/cost_attrib/abba.sh <Release exe under a path with x64-Release> <label> f:ge f:g f:g f:ge s:ge s:g s:g s:ge
python scripts/cost_attrib/nc_table.py f <label>
python scripts/cost_attrib/nc_table.py s <label>
# the sparse station normals against the dense ones, one exe, members of a pair run at the same time
bash scripts/cost_attrib/abba.sh <candidate exe> <label> f:ge+f:on f:on+f:ge s:ge+s:on s:on+s:ge
python scripts/cost_attrib/nc_table.py f <label> --vs ge on
# two executables: one label each, compared with variant@label
python scripts/cost_attrib/nc_table.py s ignored --vs ge@<new label> ge@<old label>
```

## 10. Side measurement: the sim cost of card NOM4

Card [NOM4](NOM4_network_object_model_finish_2026-10-15.md) closed with the sim golden 3.3 % slower than the build of card EP1 (field 0.7 %), cause owed. The same executables (`gnss_processor_ep1main.exe` and `gnss_processor_n5final.exe`) were run here as two concurrent pairs on the sim golden from 00:30:15 with the section timers, on the loaded machine of section 1 (labels `n4a` and `n4b`, `abba.sh` with one executable each, members of a pair started together). Sim golden, ms per epoch (`ge@n4b` is the N5 build, `ge@n4a` the EP1 build):

    ge@n4a: 2 runs, loop ms/epoch 1097 1082 (mean 1090, spread 15); process CPU s 804 834; other processes' CPU (logical procs) 17.4 16.6
    ge@n4b: 2 runs, loop ms/epoch 1104 1086 (mean 1095, spread 18); process CPU s 806 834; other processes' CPU (logical procs) 17.4 16.7
    wall ratio (numerator/denominator) = 1.00
    CPU ratio (numerator/denominator) = 1.00
    
    section          ge@n4a ms/ep (spread)  ge@n4b ms/ep (spread)  ratio   excess    pts   share
    assembly-wall           268.1      1.6         270.3     15.2   1.01      2.3   0.00   42.2%
    stash-status             10.4      0.1          12.1      0.1   1.16      1.7   0.00   31.2%
    solve-glue               74.8      2.4          76.5      3.8   1.02      1.6   0.00   30.9%
    slip-scans               48.8      0.7          50.4      1.1   1.03      1.6   0.00   30.3%
    pre-asm                  31.2      0.8          31.9      0.3   1.02      0.7   0.00   13.0%
    frame-diag              179.9      2.5         180.5      0.1   1.00      0.5   0.00    9.7%
    sol-prep                  6.2      0.1           6.5      0.0   1.05      0.3   0.00    6.2%
    cov-inverse              10.0      0.6          10.3      0.2   1.02      0.2   0.00    4.4%
    iono-diag                 8.3      0.1           8.4      0.2   1.02      0.2   0.00    3.3%
    unattributed              7.6      0.5           7.7      0.5   1.01      0.1   0.00    1.0%
    solve                     2.1      0.1           2.1      0.0   1.00      0.0   0.00    0.0%
    gauge-build               0.0      0.0           0.0      0.0    nan      0.0   0.00    0.0%
    netar                     0.0      0.0           0.0      0.0    nan      0.0   0.00    0.0%
    shdev-vtec                0.0      0.0           0.0      0.0    nan      0.0   0.00    0.0%
    install-post              0.0      0.0           0.0      0.0    nan      0.0   0.00    0.0%
    clock-upd                 0.0      0.0           0.0      0.0    nan      0.0   0.00    0.0%
    dd-build                  0.0      0.0           0.0      0.0    nan      0.0   0.00    0.0%
    t1a-health                0.0      0.0           0.0      0.0    nan     -0.0  -0.00   -0.2%
    roster                   19.4      0.2          19.4      0.4   1.00     -0.0  -0.00   -0.5%
    raim                     11.2      0.6          11.1      0.4   0.99     -0.1  -0.00   -1.5%
    dd-frac                  13.8      0.1          13.7      1.1   0.99     -0.1  -0.00   -1.9%
    emit-store               12.8      0.5          12.3      0.6   0.97     -0.4  -0.00   -8.4%
    reduce                  121.5      3.4         121.0      2.9   1.00     -0.5  -0.00   -9.3%
    cov-consumers           167.1      2.9         166.4      2.0   1.00     -0.7  -0.00  -13.8%
    leveling                 96.5      8.9          94.5      0.2   0.98     -2.0  -0.00  -36.7%
    TOTAL                  1089.8                 1095.1            1.00      5.3   0.00  100.0%

The loops differ by 5 ms per epoch (0.5 %), inside the spread of the repeats (15 and 18 ms), so the +3.3 % does not reproduce under this load. Two sections differ by more than their repeat spread: `stash-status` (10.4 to 12.1 ms) and `slip-scans` (48.8 to 50.4 ms), 3.3 ms together; `solve-glue` and `pre-asm` move by about their spreads. The figure of card NOM4 was read on an idle machine and is 22 ms per epoch at the 700 ms loop of that machine; stretched in proportion to the loop (1 090 against 700 ms) it would be about 35 ms, and 5 ms was seen. Whether the difference is real and idle-machine only is not decided here; the section pair above is the starting point for the ladder that card NOM4 lists as owed.

## Owed

- Arming `network.sparse_station_normals`: the author's decision through the ledger row and the generator. The key is not part of a snapshot fingerprint (it changes no output), so no migration is attached to it.
- Optimization 2 (the conditional variance of the border-known local columns and the datum-frame covariance copies, estimate 12 to 13 % of the field loop) and optimization 3 (the order model for the reduce subtract, the redundancy census and the joint conditional covariance, estimate 8 to 12 %): not built; each is its own card with its own gate. The 75 ms of copies of the shared covariance depend on the allocator step of section 4, whose threshold was not isolated (the standalone copy benchmark was unreadable on the loaded machine).
- Quiet-machine ratios: the ratios of section 2 are read at 17 to 18 logical processors of foreign load. The GPS-only loop is the more serial one and is stretched more, so an idle machine reads lower than 5.3 and 3.7. A dated idle-machine pair (the same four arms) is owed.
- Settled-state decomposition of the sparse formation (section 7): the formation that remains was not decomposed after the packed accumulation. The Eigen thread count other than one and the full-covariance weight path keep the dense products by construction.
- NOM4 sim cost: section 10 (an idle-machine pair of the same executables, and the step ladder, if the difference is to be attributed).

## 11. Independent check of 2026-10-18 (gate w3netcost)

A second check of sections 2 and 6 to 8 by a separate rebuild and separate arms, run after the card was written.
Executable under test: Release build of `perf/netcost` at `4075b6b` (the code is commit `1e3e857`) in worktree
`netcost_chk` (branch `chk/netcost`, a clean checkout, target `gnss_processor`, no build errors), `gnss_processor_netcost_chk.exe` under
`D:/GnssProcessorSupport/gates/exe/x64-Release` (sha256 `d0ae77f149fb947f`..., 21 326 848 bytes: the size of the
executable of section 7 and other bytes). The gate sides `w3netcost` and `w3ncon` run byte copies of it. Control: side
`w3base` (the executable of main `30ca49e`). Evidence and scratch: `D:/GnssProcessorSupport/gates/netcost_chk/`; arms
`D:/GnssProcessorSupport/runs_nom/_rap_nc_ck1_*` (attribution rounds), `_rap_nc_ck2_*` (knob pairs), `_rap_nc_ck3a_*`
and `_rap_nc_ck3b_*` (the NOM4 pair of section 10); the G5 sides `_rap_g5_w3netcost_*` (knob absent) and
`_rap_g5_w3ncon_*` (knob on).

### 11.1 Knob off, identical (gate)

- **G5, 23 arms against `w3base`:** 24 IDENTICAL lines (23 arms and the snapshot bytes), summary
  `D:/GnssProcessorSupport/gates/g7/summary_w3netcost.txt`, detail `D:/GnssProcessorSupport/gates/g5/gate_w3netcost/`.
  The 2 h arms `f2` and `s2` are in it (`products.txt` 113 902 lines on the field bed). The four arms that exit 1
  (`zero`, `joint`, `a4`, `gate`) exit 1 on `w3base`, `w3netcost` and `w3ncon`.
- **User side (`uom_gate.py`, quick set, 16 arms against `w3base`):** 16 of 16 IDENTICAL.
- **Real time against `w3base`:** every artifact identical except wall-clock text. Dr arms (`sbf`, `sbf_bssd`,
  `sbf_te`): 96 identical artifact lines; the one differing line per arm is the "Real-time session ended" line of
  `console.log` (ms wall: 1677, 2263, 3741 on the control and 4706, 2944, 7011 here). Cl arm: 37 identical artifact
  lines; four lines each in `console.log` and `main_log.log` differ (RINEX ingestion seconds, the ARC LEDGER cost
  line, the round-trip self-check seconds, the epoch-loop wall), and with those four masked both files are identical.
  These are the lines the control itself shows against a rerun of its own executable.
- **Debug build of the same tree:** the five tests of `NeqSparseStationNormalsTest` pass (38 s on this machine's load;
  section 8 gives 9 s); the full suite is 3343 tests in 450 suites, 3313 passed, 30 skipped (fixture-gated), 0 failed.
- **Scripts on the checked tree:** `docs_index_check.py`, `ledger_render.py --check`, `make_golden_config.py --verify`
  (76 recommended knobs armed, 2 documented exceptions, 0 failures), `config_posture_check.py --verify-loader`
  (0 coverage gaps), `nom_epoch_lint.py` and `nom_n4_verify.py --whole` pass.

### 11.2 Knob on, identical

- **G5 with `network.sparse_station_normals` true in every candidate arm, against `w3base` (key absent):** 24 IDENTICAL
  lines (`D:/GnssProcessorSupport/gates/netcost_chk/summary_w3ncon.txt`). The arm configurations of the two sides
  differ by that one key in all 24 arm directories (`armdiff_w3ncon.txt`); the comparison is the G5 comparison without
  its arm-configuration part.
- **The sparse formation ran in every arm.** Summed thread time of `est.addStation` (run-end D0 line), knob absent
  against knob on, the two sides run at the same time: `f2` 350 to 218 s, `s2` 221 to 151 s, `bcl` 132 to 33 s,
  `wcff` 51 to 2.0 s, `trc` 40 to 1.3 s, `gate` 11.2 to 0.3 s; the add counts are equal on the three sides in all 22 arms
  that add stations (`zero` adds none). An identical result that came from the dense fallback would show no fall.
- **Four full 2 h pairs** (section 11.4), each pair run at the same time on the checked executable, knob absent against
  knob on, and the eight pair runs recorded in section 7 re-compared: `scripts/nom_td_gate.py --whole --no-arm-check`
  gives RESULT IDENTICAL for all twelve comparisons.

### 11.3 The attribution reproduces

The four arms of section 1 in the order `ge g g ge` on each bed, one run at a time, on the checked executable
(`abba.sh`, label `ck1`), with 5.5 to 11.0 (field) and 5.8 to 14.7 (sim) logical processors of foreign load, against
17 to 18 in section 2. Share of the total excess, check against section 2:

| section | field check | field card | sim check | sim card |
| --- | --- | --- | --- | --- |
| assembly wall | 27.8 % | 28.0 % | 29.9 % | 25.9 % |
| covariance consumers | 21.9 % | 22.3 % | 15.4 % | 17.3 % |
| frame-diagnostic observers | 13.2 % | 12.1 % | 15.3 % | 16.4 % |
| reduce | 10.2 % | 11.1 % | 11.1 % | 12.4 % |
| solve glue | 10.4 % | 10.8 % | 6.5 % | 7.4 % |
| leveling harvest | 6.1 % | 6.1 % | 8.1 % | 8.3 % |
| the four sections of section 2 | 73.1 % | 73.5 % | 71.7 % | 72.0 % |

The shares of the six largest field sections agree within 1.2 points and the sim shares within 2 points except the
assembly wall (+4.0). Several section ratios repeat on the field bed: covariance consumers 10.5 (10.65 in section 2),
covariance inverse 7.6 (7.15), solve glue 6.16 (6.11), leveling harvest 3.68 (3.66); others move with the load:
assembly wall 4.31 (6.21), reduce 5.33 (5.92). The wall ratios do not repeat, since section 1 gives them as ratios
under that load: field 4.53 (330 and 1 492 ms per epoch) against 5.31, sim 2.95 (276 and 815) against 3.67. Both are
lower at the lighter foreign load, the direction section 2 predicts for an idle machine. The two `ge` runs of the sim
bed differ by 25 % (726 and 905 ms per epoch, at 5.8 and 12.7 foreign processors), so a single sim ratio is not a
usable figure; the wall figures of the cost row are a function of the machine's load. Tables:
`table_ck1_field.txt`, `table_ck1_sim.txt`.

### 11.4 The gain reproduces

Four pairs of 2 h goldens, knob absent against knob on, each pair run at the same time (`abba.sh`, label `ck2`, the
launch order swapped in the second pair of each bed); 17.3 to 18.0 foreign logical processors throughout, the load of
section 7. Mean loop ms per epoch, repeats in brackets:

| bed | knob on | knob absent | loop |
| --- | --- | --- | --- |
| field | 1 870 (1 874, 1 866) | 2 158 (2 160, 2 156) | -13.3 % (section 7: -13.3 %) |
| sim from 00:30:15 | 976 (978, 973) | 1 082 (1 088, 1 076) | -9.8 % (section 7: -10.6 %) |

The assembly wall falls from 581 to 302 ms (field) and from 266 to 156 ms (sim); every other section differs by
3 ms or less (inside its repeat spread in most cases), except the field leveling harvest, which reads 147.5 against
139.7 ms (7.8 ms, 0.4 % of the loop, against repeat spreads of 2.3 and 3.4 ms). The bar of more than 5 % on the field golden is met; the sim
figure is 0.8 points under section 7's. Tables:
`table_ck2_field_on.txt`, `table_ck2_sim_on.txt`.

### 11.5 The NOM4 pair (section 10)

The executables of cards EP1 and N5 (`gnss_processor_ep1main.exe`, `gnss_processor_n5final.exe`) as two concurrent
pairs on the sim golden from 00:30:15 (label `ck3a` for EP1, `ck3b` for N5; 5.3 to 6.1 foreign logical processors):
loop 721 and 722 ms per epoch, ratio 1.00, repeats within 1 ms. Section 10's reading holds: the +3.3 % of card NOM4
is not seen in the loop. Two things differ from section 10. The section mix moves by more than the loop does (assembly
wall 231 to 198 ms, frame-diagnostic observers 101 to 117, stash status 5.2 to 11.7, leveling 53 to 57, roster 11 to
15), so the section pair of section 10 is a load-dependent reading. The process CPU time of the N5 build is 6 % higher
in both pairs (790 and 808 s against 750 and 758 s; kernel time 293 against 262 s), where section 10 read 1.00. Table:
`table_ck3_n5_vs_ep1.txt`. The ladder over the kept step builds stays owed.

### 11.6 Corrections and reading

- **`processing.eigen_threads` is 20 in both goldens, not 1** (`config/golden/golden_field_refound.json` line 38, the
  sim golden line 37), contrary to the statement in section 6. The sparse formation runs because the network driver
  pins Eigen to one thread for the whole epoch loop when the stations run in parallel (`ApplicationNetwork.cpp`;
  `setup.parallel_on` is `network.parallel_stations` with more stations than `network.parallel_min_stations` and more
  than one OpenMP thread, and both goldens set `network.parallel_stations` true), so `Eigen::nbThreads()` reads 1
  inside `addStation`. In an arm without parallel stations, `processing.eigen_threads` other than 1 (or 0 with more
  than one OpenMP thread) keeps the dense path silently and the knob measures as no change. The identity results above
  are unaffected; a serial arm of the knob needs `processing.eigen_threads` 1 to exercise it.
- **Identical off and identical on.** The gain reproduces at the load of section 7 (13.3 % field, 9.8 % sim), the
  attribution shares reproduce at a lighter load, and the wall ratio of the cost row is load-dependent (field 4.5 to
  5.3, sim 2.9 to 3.7 across the rounds of section 2 and of this section).
- **Nothing was armed, no golden was edited, no source was changed.** The arming decision and optimizations 2 and 3
  stay owed as listed above.

## 12. Second independent check of 2026-10-18 (gates w3netcost2 and w3ncon2)

A third rebuild and a fresh set of gate arms, run after the correction of section 11.6 reached the branch. Tree under
test: `perf/netcost` at `6b52c23`, checked out clean in worktree `netcost_chk` (branch `chk/netcost`). `git diff 4075b6b
6b52c23` touches three Markdown files (this card, `CARDS.md`, `OPEN_ITEMS.md`) and no other path, so the code is that of
sections 8 and 11. The three translation units of the change (`NormalEquations.cpp`, `Estimator.cpp`,
`GnssConfigLoadNetwork.cpp`) were touched, recompiled and relinked in Release (target `gnss_processor`; the wrapper's exit
1 is the benign `vswhere.exe` message, ninja linked); the executable is `gnss_processor_w3netcost2.exe` under
`D:/GnssProcessorSupport/gates/exe/x64-Release` (sha256 `1fc9da849862e579`..., 21 326 848 bytes: the size of the
executable of section 11 and other bytes, as every link stamps its own). Control: side `w3base` (main `30ca49e`, still the
tip of `main`; the four real-time arm configs are unchanged against it). New side labels, so that the output of section
11 stays where it is cited: G5 sides `w3netcost2` (key absent) and `w3ncon2` (key true in every arm), user-side tag
`w3netcost2`, real-time labels `w3netcost2_dr` and `w3netcost2_cl`. Scratch, logs, tables and comparisons:
`D:/GnssProcessorSupport/gates/netcost_chk2/`; arms `D:/GnssProcessorSupport/runs_nom/_rap_g5_w3netcost2_*`,
`_rap_g5_w3ncon2_*` and `_rap_nc_cb1_*`, `_rap_nc_cb2_*`, `_rap_nc_cb3_*`, `_rap_nc_ci2_*`.

### 12.1 Knob off, identical

- **G5, 23 arms against `w3base`:** 24 IDENTICAL lines (23 arms and the snapshot bytes), summary
  `D:/GnssProcessorSupport/gates/g7/summary_w3netcost2.txt`, detail `D:/GnssProcessorSupport/gates/g5/gate_w3netcost2/`.
  The arms `zero`, `joint`, `a4` and `gate` exit 1 on `w3base`, `w3netcost2` and `w3ncon2` alike.
- **User side (`uom_gate.py`, quick set):** 16 of 16 IDENTICAL against `w3base` (`uom_w3netcost2.log`).
- **Real time against `w3base`:** every artifact identical except wall-clock text, read line by line. Replay arms
  (`sbf`, `sbf_bssd`, `sbf_te`): 96 identical artifact lines; the one differing line per arm is the "Real-time session
  ended" line of `console.log` (ms wall 1 677, 2 263 and 3 741 on the control; 1 336, 2 704 and 2 396 here). Closed-loop
  arm (`cl`): 37 identical artifact lines; four lines each in `console.log` and `main_log.log` differ: RINEX ingestion
  seconds (7.765 against 2.805), the epoch-loop wall (1 509 004 against 51 091 ms; the control ran with the machine at
  100 % CPU), the round-trip self-check seconds (2.04 against 0.11) and the cost tail of the ARC LEDGER line (audit 2 128.9
  against 176.8 ms), the last with every other character of the line identical.
- **Debug build of the same tree:** the five tests of `NeqSparseStationNormalsTest` pass (13.5 s); the full suite is 3 343
  tests in 450 suites, 3 313 passed, 30 skipped (fixture-gated), 0 failed (1 571 s under load).
- **Scripts on the checked tree:** `docs_index_check.py`, `ledger_render.py --check`, `make_golden_config.py --verify`
  (76 recommended knobs armed, 2 documented exceptions, 0 failures per golden), `config_posture_check.py --verify-loader`
  (0 coverage gaps), `nom_epoch_lint.py` and `nom_n4_verify.py --whole` all pass (`scripts_check.txt`).

### 12.2 Knob on, identical

- **G5 with `network.sparse_station_normals` true in every candidate arm, against `w3base` (key absent):** 24 IDENTICAL
  lines (`summary_w3ncon2.txt`). The arm configs of the two sides differ by that one key in all 24 arm directories
  (`armdiff_w3ncon2.txt`: 24 arms, 0 unexpected); the comparison is the G5 comparison without its arm-config part.
- **The sparse formation ran.** Summed thread time of `est.addStation` (run-end D0 line), key absent against key on, the
  two sides run at the same time: `f2` 133.8 to 68.0 s, `s2` 70.8 to 39.3 s, `bcl` 34.0 to 7.0 s, `wcff` 22.6 to 1.4 s,
  `trc` 24.7 to 1.1 s, `gate` 30.7 to 0.3 s. The add counts are equal on the three sides in all 22 arms that add stations
  (`zero` adds none). The time falls in 21 of the 22 arms; in `legacy`, a small GPS arm, it reads 2.3 against 4.4 s, which
  the concurrent loads of the two sides do not decide (the output is identical).

### 12.3 The attribution reproduces

Two rounds of the four arms of section 1 in the order `ge g g ge` per bed on this executable (`abba.sh`, labels `cb1`
and `cb2`; tables `table_cb1_*.txt`, `table_cb2*.txt`). Foreign load is the mean of the rest of the machine over the
run, in logical processors; in round `cb1` it was 16 to 18 throughout except the last sim `ge` run (5.2), in round `cb2`
the machine was nearly idle for the first two field runs (2.1 and 1.9) and busy again afterwards.

| run, loop ms per epoch (foreign load) | field | sim from 00:30:15 |
| --- | --- | --- |
| `cb1` ge, g, g, ge | 1 895 (17.8), 382 (16.0), 404 (16.2), 2 082 (17.8) | 1 081 (17.2), 289 (15.6), 299 (15.6), 752 (5.2) |
| `cb2` ge, g, g, ge | 1 548 (2.1), 370 (1.9), 384 (7.0), 2 132 (17.8) | 1 090 (17.3), 287 (16.0), 291 (15.9), 972 (14.0) |

Share of the total excess of `ge` over `g`, with the section 2 figures. The `cb2` field column is the one pair run at
2 foreign processors (the first `ge` and the first `g`); the `cb2` sim column pools its four runs:

| section | field card | field `cb1` | field `cb2` quiet pair | sim card | sim `cb1` | sim `cb2` |
| --- | --- | --- | --- | --- | --- | --- |
| assembly wall | 28.0 % | 28.2 % | 27.3 % | 25.9 % | 29.2 % | 26.6 % |
| covariance consumers | 22.3 % | 20.8 % | 21.7 % | 17.3 % | 16.3 % | 15.9 % |
| frame-diagnostic observers | 12.1 % | 12.8 % | 12.7 % | 16.4 % | 15.3 % | 16.6 % |
| reduce | 11.1 % | 11.6 % | 10.5 % | 12.4 % | 11.2 % | 12.2 % |
| solve glue | 10.8 % | 11.4 % | 10.5 % | 7.4 % | 6.6 % | 7.2 % |
| leveling harvest | 6.1 % | 5.9 % | 6.0 % | 8.3 % | 8.1 % | 8.2 % |
| the first four rows | 73.5 % | 73.4 % | 72.2 % | 72.0 % | 72.0 % | 71.3 % |

The shares of the six largest sections agree within 1.5 points on the field bed and within 3.3 points on the sim bed (the
assembly wall of `cb1`, whose two `ge` runs sit at 17.2 and 5.2 foreign processors; every other sim share within 1.4); the
four-section total agrees within 1.3 points everywhere. Section ratios of the field bed (card, `cb1`, quiet pair): covariance consumers 10.65, 9.83, 8.85;
covariance inverse 7.15, 7.04, 5.88; solve glue 6.11, 6.03, 5.09; leveling harvest 3.66, 3.46, 3.13; assembly wall 6.21,
5.95, 4.09. The shares and the ordering reproduce; the section ratios are lower at low load.

**The wall ratio is a function of the load, and it is the GPS + Galileo loop that moves.** Field wall ratio: 5.31 (section
2), 5.06 (`cb1`, 16 to 18 foreign processors), 4.18 (quiet pair, 2 foreign processors), 4.53 (section 11.3, 5.5 to 11).
Sim: 3.67, 3.68 for the `cb1` pair at 17 foreign processors (1 081 over 294), about 2.6 for the `ge` run at 5.2 against the
`g` runs. In these rounds the GPS-only loop does not follow the load (field 330 to 406 ms per epoch over the rounds of
sections 2, 11 and 12, with no trend in the foreign load; sim 258 to 299); the GPS + Galileo loop does (field 1 492 to
1 548 at 2 to 11 foreign processors, 1 895 to 2 132 at 15 to 18). The statement in sections 1 and 2, and in the Owed
list, that the GPS-only loop "is stretched more by load" and so reads lower on an idle machine is not what these rounds
show: the idle machine does read lower (4.2 on the field bed), but because the GPS + Galileo loop is shorter on it, with
the GPS-only loop unchanged. Process CPU time ratio:
2.25 (`cb1` field) and 1.86 (quiet pair), against 2.32 in section 2. A cost statement needs its load: field 4.2 at idle,
5.1 to 5.3 at 17 foreign processors; sim about 2.6 to 3.7.

### 12.4 The gain reproduces

Four pairs of 2 h goldens, key absent against key on, each pair run at the same time (`abba.sh`, label `cb3`, launch order
swapped in the second pair of each bed), 13.8 to 17.3 logical processors of other load (the partner of the pair
included). Loop ms per epoch, key on against key absent:

| bed | pair 1 | pair 2 | mean | loop |
| --- | --- | --- | --- | --- |
| field | 1 823 against 2 115 | 1 793 against 2 079 | 1 808 against 2 097 | -13.8 % (section 7: -13.3 %; section 11.4: -13.3 %) |
| sim from 00:30:15 | 899 against 1 007 | 935 against 1 044 | 917 against 1 026 | -10.6 % (section 7: -10.6 %; section 11.4: -9.8 %) |

The assembly wall falls from 576 to 303 ms (field) and from 251 to 149 ms (sim); every other section moves by 6.4 ms or
less on the field bed (the leveling harvest 144.8 to 138.4 ms, 0.3 % of the loop, against repeat spreads of 3.4 and 4.2
ms and 7.8 ms in 11.4; covariance consumers 3.9 ms, inside its spreads of 12 and 16 ms) and by 1.8 ms or less on the sim
bed. The bar of more than 5 % on the field golden is met at 13.8 %. Tables: `table_cb3_field_on.txt`,
`table_cb3_sim_on.txt`.

### 12.5 The sub-sections behind the three estimates

The scratch timing executable of section 3 (`gnss_processor_netcost_instr.exe`, sha256 `e528ce9ea733446d`..., built by the
card's author from a patch that is not in the tree) was run once per bed and variant (label `ci2`, 15 to 18 foreign
processors, loops 2 126 and 406 ms per epoch on the field bed, 1 005 and 272 on the sim bed; `instr_ci2_field.txt`,
`instr_ci2_sim.txt`). The station dimensions of section 3 reproduce exactly (field `m` 56 and 93, `n_s` 214 and 418, `n_r`
28 and 49; sim 49 and 89, 157 and 279, 27 and 49). Field bed, GPS + Galileo, ms per epoch against section 3: the shared
product 367.9 (370.9), the symmetry check 83.4 (69.2), `pureConditionalLocalVarianceDiag` 206.6 (212.1),
`conditionalLocalVarianceDiag` 130.3 (140.5) of which the copy of the shared covariance 66.6 (75.1), the reduce subtract
181.6 (179.5), the local solves 50.1 (50.0), the redundancy census 212.0 (201.1), the post-fit sigma0 and RHS trip 63.8
(63.0). Sim bed: shared product 145.2, pure conditional variance 71.3 (69.1), reduce subtract 80.8 (83.5), census 110.1
(110.0). The estimates of section 5 follow from these: change 1 removed 13.8 % of the loop against the 18 to 21 % read
from the sub-sections (the formation that stays is the difference, as section 7 states); change 2 addresses 206.6 + 66.6
ms of a 2 126 ms loop, at most 12.8 % and about 12 % if 15 ms of the first term remains; change 3 addresses 181.6 + 212.0 +
25.4 ms (the reduce subtract, the census and the joint conditional covariance), 19.7 % of the loop, so its 8 to 12 %
requires removing 40 to 60 % of that work, which depends on how sparse the left factors of those products are and has not
been measured. Change 2 carries the least exactness risk of the two that remain (it deletes a product with an exact zero
matrix and a copy, and needs no order model); a non-finite regression entry would turn that product to NaN, which a
replacement must either reproduce or exclude.

### 12.6 Reading

- **Pass criteria.** Everything off is identical to `w3base` on the three gates (24 of 24, 16 of 16, real time up to
  wall-clock text), the key on is identical on 24 of 24 arms and the sparse formation is shown to run, the attribution
  shares reproduce on two further rounds at three load levels, and the gain on the field golden is 13.8 % against the bar
  of 5 %.
- **Corrections to the card.** Section 11.6 (`processing.eigen_threads` 20 in the goldens, Eigen pinned to one thread by
  the parallel epoch loop) was checked in the tree: the goldens set 20 on `eigen_threads` (field line 38, sim line 37) and
  `parallel_stations` true, `ApplicationNetwork.cpp` pins at line 303 and restores at 333 and 351, and the sparse formation
  refuses any other thread count (`NormalEquations.cpp` line 130); the 12.2 table shows it running in the goldens. The
  mechanism given in sections 1 and 2 for the load dependence of the ratio is corrected in 12.3.
- **Not repeated.** The NOM4 pair of section 10 (done in 11.5). **Still owed:** the arming decision, optimizations 2 and 3,
  a quiet-machine sim pair (the only quiet reading here is the field pair; the sim has one `ge` run at 5.2 foreign
  processors), and the settled-state decomposition of the sparse formation.

## 13. Continued in card NETCOSTB (2026-10-18)

The `legacy` reading of 12.2 is decided in [NETCOSTB](NETCOSTB_gps_galileo_cost_continued_2026-10-18.md): load, not the knob. That file also corrects two statements of this one (the `legacy` arm is GPS + Galileo; the stated cause of the load dependence of the wall ratio) and holds the results from here on, this file being at the 60 KB bar.
