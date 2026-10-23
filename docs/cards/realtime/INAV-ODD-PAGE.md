# INAV-ODD: Galileo E23 on the live stream, and the SBF I/NAV odd-page offset

**STATUS**

- **id:** INAV-ODD
- **topic:** realtime
- **status:** closed
- **summary:** E23 poisoned the live PPP-RTK float solution because the receiver's raw Galileo I/NAV pages were assembled with the odd-page data 6 bits off (orbit wrong by up to about 300 m); FIXED and tested against the receiver's own GALNav; a live re-run of the E23 event itself is owed.
- **dates:** 2026-10-23
- **knobs / posture:** none (a decoder fix, no toggle); diagnostic `EPH-SAME-IODE-DIFF` (debug level, `rt_engine`) now also compares toc, af0, af1, af2
- **code:** `src/core/sbf/SubframeAssembler.cpp` (`consumeGal`), `src/core/products/LiveEphemerisStore.cpp` (`logSameIodeEphDiff`), `tests/test_sbf.cpp` (`SubframeAssemblerTest.GalileoInav*`)
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_deck_2026-10-22\live_ge_{kin,static}{,_noE23}` (the evidence), `D:\GnssProcessorSupport\runs\rt_e23_diag\{probe,post}` (live trace probes before and after the fix)
- **capture:** `data/sbf/covl1452.26_` (rooftop SBF, 2026 DOY 145: 238,342 GALRawINAV pages, 595 GALNav records)
- **source:** this card (diagnosis requested after the 2026-10-22 deck refresh sessions)

**Outcome.** Why did Galileo E23 alone degrade the 2026-10-23 live sessions? The IGS nav stream held only a day-old E23 record, so the stall fallback ran E23 on the receiver's raw I/NAV assembly, which read the 16 odd-page bits 6 bits late. sqrt(A), IDOT, Crs and af1 were corrupted with IODnav intact, so the SSR applied and E23 entered cm-grade, hundreds of metres off. FIXED: assembly now matches GALNav on 246 of 246 records (0 before); live, 0 of 12 Galileo records differ from IGS (11 of 11 before). Owed: the event re-run (recorder) and the no-recovery response (section 4).

> Results are appended TO THIS FILE, below the text.

---

## 1. Evidence from the live sessions (HEAD 04517ca, Release)

Log lines are quoted without the receiver address or the caster credentials.

**Kinematic session `live_ge_kin`.** The only E23 record held at start came from the IGS
broadcast stream (BCEP00BKG0) and was a day old; the fit-window guard rejected it for 15 epochs:

```
[2441, 436421.000 (2026-10-23 01:13:41.000)] [rt_engine] [warning] EPH-STALE E23 REJECTED: age=+89021s (+24.73h from toe) exceeds 2.0h fit window — sat skipped
...
[2441, 436435.000 (2026-10-23 01:13:55.000)] [rt_engine] [warning] EPH-STALE E23 REJECTED: age=+89035s (+24.73h from toe) exceeds 2.0h fit window — sat skipped
```

One epoch later a fresh record replaced it and E23 was admitted, already about 270 m off on
every signal, code and phase alike (the states were seeded from the offset):

```
[2441, 436436.000 (2026-10-23 01:13:56.000)] [state_manager] [info] Added ambiguity E23_L1C at index 140 with initial value -272.465, std=1000.000 m
[2441, 436436.000 (2026-10-23 01:13:56.000)] [state_manager] [info] Added sat bias state for E23|C5Q at index 144, initial=-268.216 m, std=10.000 m
[2441, 436436.000 (2026-10-23 01:13:56.000)] [measurement_model] [warning] Large innovation for E23-E21 C1C: innovation=-270.012 m, mode=PPP-RTK
```

The filter absorbed the offset into the E23 states, mostly the slant ionosphere (-266.3 m at
01:26:06, not a physical value) and the satellite biases. Over the next 14 minutes the E23 C1C residual (C1C has no satellite-bias state) drifted
from 0.24 m to 2.25 m (`measurement_residuals.csv`, 436536 to 437266), which matches the
along-track growth of an orbit with a wrong semi-major axis. At 01:30:36 the E23 record changed
again and every E23 row stepped by the same non-dispersive amount:

```
[2441, 437436.000 (2026-10-23 01:30:36.000)] [measurement_model] [warning] Large innovation for E23-E04 C1C: innovation=269.470 m, mode=PPP-RTK
[2441, 437436.000 (2026-10-23 01:30:36.000)] [measurement_model] [warning] Large innovation for E23-E04 L1C: innovation=265.692 m, mode=PPP-RTK
[2441, 437436.000 (2026-10-23 01:30:36.000)] [measurement_model] [warning] Large innovation for E23-E04 L5Q: innovation=265.685 m, mode=PPP-RTK
[2441, 437436.000 (2026-10-23 01:30:36.000)] [measurement_model] [warning] Large innovation for E23-E04 L7Q: innovation=265.686 m, mode=PPP-RTK
```

The error after the step was about 1 m. The phase rows re-absorbed the step within one epoch;
the code rows did not, because the satellite-bias states (standard deviation 0.025 m) kept the
absorbed value. Every E23 code row after 01:30:36 was a large innovation and a code outlier for
the rest of the session: 8,036 `Large innovation` lines, all of them E23. The same epoch
coincided with four forced GPS pivot handovers. The info-level log does not record which source
supplied the 01:30:36 record.

**Static session `live_ge_static`** (a new process at 01:44): the same stale IGS record
(`age=+90841s (+25.23h from toe)`), then admission at 01:44:06 with a different offset
(C1C innovation -48.6 m, satellite-bias seeds near -52 m) and a further 22 m step at 01:50:32.

**Only E23** was affected because only E23 lacked a current IGS record. For every other
satellite the receiver record is ignored (`EPH-SOURCE ... IGS-stream record authoritative`).

## 2. Mechanism

1. **Source selection.** `LiveEphemerisStore::insert` gives the IGS broadcast stream priority,
   but a receiver record whose toe is at least 1800 s newer than the held IGS record replaces
   it (the stall fallback). With the IGS E23 record a day old, E23 ran on the receiver's own
   ephemeris. The live SBF stream carries no GALNav (4002) blocks, so that ephemeris comes from
   `SubframeAssembler`, which assembles GALRawINAV (4023) pages.
2. **The decode defect.** An I/NAV word is 128 bits: 112 from the even page part and 16 from
   the odd part. Septentrio's NAVBits drop the 6 tail bits of the even part, so the odd part
   starts at page bit 114 and its 16 data bits are 116..131. The assembler read 122..137 (the
   layout with the even tail kept). The last 16 bits of each word hold the low 14 bits of
   sqrt(A) (word 1), IDOT (word 2), the low bits of Crs (word 3) and the low 8 bits of af1 plus
   af2 (word 4). IODnav sits in the even part, so the four words still agreed on IODnav and the
   record was emitted as valid.
3. **Why the SSR did not catch it.** The SSR orbit and clock gate matches IODnav (low 8 bits),
   which was intact, so the correction was applied and E23 was cm-grade (`clk-applied`), not
   broadcast-grade. The residual range error was the corrupted orbit, hundreds of metres.

Size of the corruption, from the 246 records of the May capture whose raw assembly and GALNav
share an IODnav: semi-major-axis error median 89 m, 90th percentile 169 m, maximum 308 m;
af1 error up to 0.84 mm/s of range rate. The day's offsets (-270 m, -48.6 m) fall inside it.

**Live confirmation before the fix** (`rt_e23_diag\probe`, 2026-10-23 15:23, trace level, the same
exe): the receiver assembled 11 Galileo and 12 GPS records whose IODnav / IODE matched the held
IGS record. All 11 Galileo records differed from the IGS one; no GPS record did:

```
[2441, 487385.000 (2026-10-23 15:23:05.000)] [rt_engine] [debug] EPH-SAME-IODE-DIFF E23 iode 48: IGS vs receiver differ: sqrtA=5440.63/vs/5440.65 idot=3.88588e-10/vs/1.46863e-09 crs=141.969/vs/142.812
```

E23 itself is GSAT0226, USABLE with a PHM clock and no active NAGU (GSC constellation page,
2026-10-23); its broadcast clock (af0 about -54 µs) and SSR C0 (+0.18 m) are ordinary.

## 3. Fix and verification

**Fix.** `SubframeAssembler::consumeGal` reads the odd-page data at bits 116..131
(`kOddDataBit`). `logSameIodeEphDiff` now also compares toc, af0, af1 and af2, so a
clock-only disagreement between the receiver and the IGS record is visible at debug level.

**Layout proof on the capture** (`covl1452.26_`): the CRC-24Q of page bits 0..195 equals bits
196..219 on 19,996 of 19,996 CRC-passed pages; with the odd part at bit 120 it matches none.
Assembled with offset 122 (old), 246 of 246 records differ from the receiver's GALNav at the
same IODnav; with offset 116, 0 of 246 differ.

**Unit tests** (`tests/test_sbf.cpp`): `SubframeAssemblerTest.GalileoInavCrcPlacesOddPartAtBit114`
checks the CRC layout on four real pages; `SubframeAssemblerTest.GalileoInavMatchesReceiverGalNav`
assembles E04 IODnav 51 from those pages and compares 18 elements plus toe and toc with the
receiver's GALNav record. With the old offset the second test fails on sqrtA, IDOT, Crs and af1;
with the fix both pass. Full Debug suite on the fixed tree: 3,591 tests, 3,583 passed, 0 failed,
8 skipped (all data-fixture gated: GPT3 grid, SH IONEX, bundled BRDC, the SP3 fixture).

**Seam gate** (`scripts/rt_seam_gate.ps1`, Release; pre = `gnss_rtseam_pre.exe`, the deck build
of 04517ca, md5 `c7fb24a8c6412932071eb123c1a84ba2`; post = the fixed tree built in a D: worktree,
`gnss_rtseam_post.exe`, md5 `e05a53cb017541998f9b0fddb7b129f9`; captures
`C:\GnssProcessor\runs\rtseam_goldens\e23_{pre,post}`). The comparator reports FAIL on all four
arms; every difference is accounted for:

- `cl` (RTCM network closed loop, no SBF): all 10 CSV artifacts identical. The only flagged line
  is the ARC LEDGER `COST: build N ms total` wall-clock field, which the comparator's masks do
  not cover (its other timing lines are masked).
- `sbf`, `sbf_bssd`, `sbf_te` (SBF file replay, no IGS nav stream, so every Galileo record is a
  raw I/NAV assembly; the capture has 893 GALRawINAV pages and no GALNav): the outputs move, by
  design. In `sbf` the first-epoch position moves about 400 m onto the receiver's own PVT
  (-2497213.1, -3805285.6, 4453257.2 against -2497522.7, -3805433.1, 4453547.7 before), and
  Galileo satellites that were code outliers become clean (E11 at the first epoch). `sbf_bssd`
  (Galileo dropped as broadcast-grade) moves only the Galileo azimuth column at the third
  decimal. `gnss_config.log` differs in the logged internal data root, a property of where the
  post exe was built.
- **Control:** the `sbf` arm with `active_systems` GPS + BeiDou
  (`rt_e23_diag\ctrl_noE_{pre,post}`): 8 of 8 CSV artifacts byte-identical between the two exes.

**Live check of the decoder** (`rt_e23_diag\probe` before, `rt_e23_diag\post` after, 150 s each,
trace level, the same config; E23 below the mask both times, so this checks the decoder, not
the E23 event). Same-IODnav comparisons of a receiver record against the held IGS record:

| | Galileo compared | differing orbit elements | GPS compared | differing |
| --- | --- | --- | --- | --- |
| before (04517ca) | 11 | 11 | 11 | 0 |
| after (fix) | 12 | 0 | 11 | 0 |

After the fix two Galileo records still differ, in af0 only (the comparison added by this card):
E05 IODnav 51 by 0.8 ns and E34 IODnav 51 by 2 ns (0.24 m and 0.6 m). The receiver's assembly
matched its own GALNav af0 on all 246 records of the May capture, so the difference is in the
IGS stream's record; its size is that of an I/NAV against F/NAV clock (follow-up below).

## 4. Follow-ups (not done here)

- **No recovery after a satellite's broadcast baseline jumps.** After the 01:30:36 step the E23
  satellite-bias states held a 266 m error at 0.025 m standard deviation for the rest of the
  session. A per-satellite source switch or record change that moves the predicted range by more
  than the code noise is an event (standing invariants 1 and 7) with no response today: the
  satellite's code-bias and ambiguity states are not re-seeded and nothing counts the switch.
- **The stall fallback is silent at info level.** `EPH-SOURCE ... taking over` is a debug line;
  a source switch per satellite deserves a counter and an info line.
- **BGD naming in the SBF decoders.** `SubframeAssembler` and the GALNav decoder store
  BGD(E1,E5a) in `tgd1` and BGD(E1,E5b) in `tgd0`, the reverse of the `BroadcastEphemeris`
  documentation and of `RtcmNavDecoder` (the word-5 bits match GALNav's E1-E5a and E1-E5b fields
  on 77,889 of 77,889 sets, so only the naming is swapped). Nothing in the real-time path reads
  them today.
- **The GALNav Source byte is ignored.** I/NAV and F/NAV GALNav records (both present in the May
  capture) carry different clock references and overwrite each other at the same IODnav. The
  live stream carries no GALNav today.
- **Upstream.** The IGS broadcast stream served a 24.7 h (01:13) and 25.2 h (01:44) old E23
  record; worth a check at the next E23 pass. Separately, two of its Galileo records (E05, E34
  IODnav 51 at 15:49) carried an af0 0.8 to 2 ns away from the on-air I/NAV value at the same
  IODnav; which clock the SSR provider references decides whether that matters.
- **Seam comparator mask.** The ARC LEDGER `COST: build N ms total` field is wall clock and
  unmasked in `scripts/rt_seam_compare.py`, so the `cl` arm reports FAIL on identical CSVs.
- **Replay.** A paced obs / SSR / nav recorder remains the prerequisite for replaying the
  E23 event itself as an A/B; the live probes here compare the decoder, not the event.
- Cosmetic: the log time stamp prints `15:22:60.000` (seconds rounded to 60).
