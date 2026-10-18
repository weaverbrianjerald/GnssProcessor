# NETCOSTB: the GPS + Galileo cost card, continued (the legacy reading and two corrections)

**STATUS**

- **id:** NETCOSTB
- **topic:** perf
- **status:** closed
- **summary:** Card NETCOST's continuation (that file is at the 60 KB bar), 2026-10-18. It decides the one reading that section 12.2 of card NETCOST left open: in the 31-epoch `legacy` arm of the 23-arm gate the station-add time (`est.addStation`) read 2.3 s with `network.sparse_station_normals` absent and 4.4 s with it on. The rise is load. The key-on side of that gate ran a loop of 60.4 s against 12.6 s, the sections the key does not touch were stretched 5.2 to 8.1 times and the station adds 1.9 times; in three controlled pairs on one executable the station-add time is 1.84 to 1.94 s with the key absent and 0.54 to 0.57 s with it on, the loop is 11.5 to 16.0 % shorter, and the output is identical. Two wording corrections to card NETCOST section 12 follow: `legacy` arms GPS and Galileo, and the stated cause of the load dependence of the wall ratio contradicts its own direction. Card NETCOST stays open and keeps the owed list (the arming decision, optimizations 2 and 3, a quiet-machine sim pair).
- **source:** [NETCOST](NETCOST_gps_galileo_cost_attribution_2026-10-18.md) section 12.2 (the `legacy` reading) and section 12.3 (the load dependence of the wall ratio); the gate runs `runs_nom/_rap_g5_{w3base,w3netcost2,w3ncon2}_legacy`.
- **knobs / posture:** none; `network.sparse_station_normals` stays default false, ledger UNVALIDATED, unarmed in both goldens. No source, golden or ledger change.
- **dates in the text:** 2026-10-18
- **hashes / run dirs:** tree `perf/netcost` (code of `1e3e857`; the later commits change documents only); executable `gnss_processor_netcost.exe` under `D:/GnssProcessorSupport/gates/exe/x64-Release` (sha256 `d72d846658bd7a26`..., the bytes of `gnss_processor_netcost_cand.exe`); controlled runs `D:/GnssProcessorSupport/runs_nom/_rap_nc_lg_{off,on}_{1,2,3}`; scratch `D:/GnssProcessorSupport/gates/netcost_fix/` (`lg_setup.sh`, `lg_run.sh`, `lg_run.log`, `lg_read.py`).

**Outcome.** Question: is the sparse station-normal formation slower than the dense one in the `legacy` arm of the gate,
where the station-add time read 2.3 s with the key absent and 4.4 s with it on? Result: no. The key-on run of that gate
arm was stretched by load (loop 60.4 s against 12.6 s, the untouched sections 5 to 8 times, the station adds 1.9 times).
In three controlled pairs the station adds take 0.54 to 0.57 s against 1.84 to 1.94 s and the loop is 11.5 to 16.0 %
shorter, with identical output. Standing: the open reading of card NETCOST section 12.2 is closed, and two wording
corrections to its section 12 are recorded in section 3.

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
