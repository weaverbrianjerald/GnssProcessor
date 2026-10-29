# SSR-PB-HDR: the SSR phase-bias message read with its header indicators in the satellite block

**STATUS**

- **id:** SSR-PB-HDR
- **topic:** realtime
- **status:** closed
- **summary:** The phase-bias messages (IGS-SSR 4076 IM x6, legacy 1265 to 1270) were decoded with the two consistency indicators read per satellite instead of once in the header, so every satellite block after the first was misaligned; the logged phase-bias coverage ramp and the phase-bias resets of the 2026-10-23 live sessions are decode artifacts. FIXED and unit-tested. Live session after the fix (2026-10-29, 30 min): phase-bias coverage 99.9 % of satellite-epochs (every epoch after the first complete), 0 resets against 28 before.
- **dates:** 2026-10-29 (fix, unit tests, live session)
- **knobs / posture:** none (a decoder fix, no toggle)
- **code:** `src/core/rtcm/RtcmSsrDecoder.cpp` (`decodeSsrHeader`, `decodePhaseBiasBody`), `include/rtcm/RtcmTypes.hpp` (`SsrHeader::dispersive_bias_consistency`, `SsrHeader::mw_consistency`), `tests/test_rtcm_ssr_phase_bias.cpp` (`RtcmSsrPhaseBiasTest.*`)
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_deck_2026-10-22\live_ge_{kin,static}{,_noE23}` (the evidence, decoded before the fix); `D:\GnssProcessorSupport\runs\rt_pbhdr_2026-10-29\{smoke,live_ge_static}` (after the fix, exe `gnss_processor_pbhdr.exe`, sha256[:16] 7f3cb720707187a0)
- **reference:** cssrlib `rtcm.py` `decode_head` and `decode_cssr_pbias` (`D:\GnssProcessorSupport\reference_software\cssrlib\src\cssrlib\rtcm.py`)
- **source:** this card (diagnosis of the live-session phase-bias coverage ramp)

**Outcome.** Why did the live phase-bias coverage start at 0 or 2 satellites and climb for 20 minutes while orbit, clock and code bias were complete within seconds? The decoder read the dispersive-bias and Melbourne-Wübbena consistency bits after each satellite id; they sit once in the message header, before the satellite count. The count was read two bits early, and every satellite block after the first decoded as misaligned bits. FIXED: header read, four unit cases on hand-built messages in both dialects. A 30-minute live session after the fix has every modelled satellite covered from the second epoch (99.9 % of satellite-epochs) and 0 phase-bias resets, against 80.8 % and 28 before (section 6). The ramp and resets were decode artifacts. Still owed: a freshness gate on the coverage count; integer fixing on the live stream is untested.

> Results are appended TO THIS FILE, below the text.

---

## 1. Evidence from the live sessions (exe `gnss_processor_deck.exe`, Release, 2026-10-23)

The per-epoch `SSR coverage` line of `rt_engine.log` counts, among the modelled satellites, those
with any phase-bias record in the store. Phase-bias coverage at the first and last of 1,800 epochs,
and the `SSR phase-bias discontinuity: reset` lines of each session:

| session | first epoch | last epoch | phase-bias resets |
| --- | --- | --- | --- |
| `live_ge_static` | 2/20 | 21/22 | 28 |
| `live_ge_kin` | 0/19 | 18/21 | 6 |
| `live_ge_static_noE23` | 0/16 | 13/16 | 8 |
| `live_ge_kin_noE23` | 0/18 | 14/16 | 29 |

In the same lines of the static session, code bias is complete from the first epoch and orbit from
the sixth, while phase bias stays at 2 satellites. The ramp is the same in all four sessions, so it
belongs to the decoder, not to the stream.

## 2. Mechanism

The phase-bias header is, after the solution id: dispersive-bias consistency indicator (1 bit),
Melbourne-Wübbena consistency indicator (1 bit), number of satellites (6 bits). The satellite block
is the satellite id, the number of biases (5 bits), the yaw angle (9 bits), the yaw rate (8 bits),
then per signal the signal id (5), signal integer indicator (1), wide-lane integer indicator (2),
discontinuity counter (4) and phase bias (20). cssrlib reads this layout for both 4076 and
1265 to 1270.

The decoder skipped the two header bits and read two flag bits after each satellite id instead:

- the satellite count was the two indicator bits followed by the top four bits of the count;
- the first satellite id was the last two count bits followed by the top four id bits, and the
  two "flags" consumed the last two id bits, so the first block's values were aligned and correct
  under a wrong id;
- the second satellite id was read at its true position, and its two "flags" consumed the top two
  bits of its signal count, so its values were misaligned;
- every later block was misaligned in id and values.

A record whose garbage id landed on a tracked satellite counted as coverage, and
`SsrCorrectionStore::coverageFor` counts code and phase biases on presence alone, without an age
check, so the coverage climbed as such records accumulated. The resets are
changes between successive garbage values, not provider discontinuities. Orbit, clock and code-bias
headers carry no such indicators, which is why they decoded cleanly.

## 3. Consequence for the live results

The live sessions ran without usable phase biases, so integer ambiguity resolution on the live stream
was not possible in them. A constant wrong bias joins the float ambiguity; a changing one triggers a
phase-bias reset of the ambiguity. The float positions of the live sessions are affected through those
resets. The statements that the stream carries phase biases for 69 to 89 % of satellites and that it
resets its phase biases repeatedly are withdrawn: the stream's phase-bias coverage has not been measured.

## 4. Fix and test

`decodeSsrHeader` takes the body type and reads the two indicators for phase-bias messages only, into
`SsrHeader`; `decodePhaseBiasBody` starts at the satellite id and copies the indicators onto each
record. `tests/test_rtcm_ssr_phase_bias.cpp` builds the messages bit by bit in the cssrlib layout:
4076 GPS (three satellites, both signals checked), legacy 1267 Galileo (two satellites), legacy 1265
GPS with both indicators set (four satellites; the old decoder reads the count as 49), and a 4076
clock header that carries no indicators. No raw capture of the live stream exists on disk, so the
2026-10-23 sessions cannot be re-decoded.

## 5. Owed

- One live session with a Release exe built after the fix, to measure the stream's phase-bias coverage
  and discontinuity rate.
- The coverage count reads any record as coverage; a freshness gate on it would have shown the
  defect as low coverage instead of a slow ramp.

## 6. Live session after the fix (2026-10-29)

Exe `gnss_processor_pbhdr.exe` (Release, built from c37d4d6, sha256[:16] `7f3cb720707187a0`), the
`live_ge_static` config of 2026-10-23 unchanged (static, float, GPS + Galileo, the same receiver,
SSR and navigation streams), run dir `D:\GnssProcessorSupport\runs\rt_pbhdr_2026-10-29\live_ge_static`
(a 60 s smoke session in `smoke\` beside it). 1,800 epochs from 19:42:06 to 20:12:05 GPS time, exit 0,
1,813 s wall.

| session | epoch 1 | epoch 2 | last epoch | satellite-epochs with a phase bias | phase-bias resets |
| --- | --- | --- | --- | --- | --- |
| `live_ge_static`, 2026-10-23, before the fix | 2/20 | 2/20 | 21/22 | 30,816 of 38,129 (80.8 %) | 28 |
| `live_ge_static`, 2026-10-29, after the fix | 0/18 | 18/18 | 16/16 | 30,239 of 30,257 (99.9 %) | 0 |

The one short epoch is the first, before the first phase-bias message arrived. From the second epoch
every modelled satellite has a phase-bias record in every epoch. No phase-bias discontinuity counter
changed in 30 minutes (0 discontinuity resets, 0 freshness-flip resets, 0 carries), against 28
discontinuity resets in the session before the fix. The smoke session agrees: 59 of 60 epochs at 18/18,
0 resets.

Verdict: the stream carries phase biases for all modelled GPS and Galileo satellites from its first
message, and its discontinuity counters were constant over the session. The ramp and the resets of
section 1 were the decoder's alone.

Not graded here:

- The coverage count reads presence alone (section 5), so the age of each record is not graded; the
  freshness gate stays owed (`docs/status/OPEN_ITEMS.md`, consumer and real-time service).
- The count is per satellite, not per signal. The engine's bias attribute census (a tracked signal with
  no bias while a same-band sibling has one) names a stable set through the whole session: E07, E29 and
  E33 on L6C, and L2L and L2W on G03, G04, G09 and G21 among others (end of run: 60,710 code and
  111,773 phase lookups). Before the fix the same census named a shifting set. Not examined.
- Integer fixing was off (`ambiguity_resolution.mode` 0), as in the 2026-10-23 sessions, so fixing on
  the live stream is untested.
