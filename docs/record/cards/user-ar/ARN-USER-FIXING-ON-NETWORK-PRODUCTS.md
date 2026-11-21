# ARN: integer fixing at a user of the network's own products, a first measurement

**STATUS**

- **id:** ARN
- **topic:** user-ar
- **status:** closed
- **summary:** Pre-registered 2026-11-13 as a measurement; results of 2026-11-13 and 14 below. On the network's golden products (GPS + Galileo, 23 to 27 May 2026) the user resolver of cards ARU and ARUB finds no integers, but the user's ambiguities are an integer plus one fraction per satellite and signal that all receivers share. With those fractions fitted from other receivers and removed from a COPY of the product file (offline, analysis tools only; a decision taken for the author's review, the ruling of 2026-11-09 stands), two receivers outside the fit hold 85 to 95 % of their pair-epochs on integers (a third 63 to 87 % GPS), two start times agree on all 755 distinct combinations over five days, and the double-difference integers equal those on CODE products in 96 of 97 combinations with nine receivers held out and in all 61 of the three-receiver sets. A resolver step for a third and further signal is built on the branch (default off) and fixes their integers on CODE products with fitted biases. Continued in card [ARNB](ARNB-USER-FIXING-ON-NETWORK-PRODUCTS-CONTINUED.md) (2026-11-14: a network that has run for most of a day).
- **dates:** 2026-11-13 (pre-registered, sections 5 and 6), 2026-11-14 (sections 7 to 15)
- **knobs / posture:** consumer replay (`network.closed_loop.replay_products_path`) with `consumer_undifferenced` false and the ambiguity-resolution block of the b12 control; new, default off: `estimation.ambiguity_resolution.extra_signals`, `estimation.ambiguity_resolution.primary_bands` (section 13, ledger rows UNVALIDATED).
- **code:** no producer code. The extra-signal step of the user resolver on branch `ar/rx-sat-code-bias` (uncommitted; executable `gnss_processor_rscb4.exe`); every offset, stream copy and bias-file copy is made by analysis tools.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nw_<tag>_<doy>_<site>` (replay runs), corrected streams `_rap_nwg{c,w}[9]_n<doy>`, network runs `_rap_p26_n<doy>` (golden field posture, 56 receivers), every-frequency runs `_rap_aru_{tf,t3}*_est_144_<site>`; tools `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\{nw_,tf_}*.py`.
- **source:** cards [ARU](ARU-USER-FIXING-CONTROL.md), [ARUB](ARUB-USER-FIXING-CONTROL-CONTINUED.md), [RSCB](../../../cards/user-ar/RSCB-USER-RECEIVER-SATELLITE-CODE-BIAS.md); the integer route of the network (cards IRD to IRDG, the rulings of 2026-11-09)

**Outcome.** Closed 2026-11-20; measured, then demonstrated offline. The golden stream gives a user no integers, yet the fractions of its
ambiguities are common to all receivers, one per satellite and signal. Fitted from some 70 other receivers and removed from a
copy of the product file, they leave receivers outside the fit with integers: 85 to 95 % of pair-epochs held at two receivers
on five days (a third is weaker), no integer different between two start times, and the double-difference integers of CODE
products. The chain posture gave neither integers nor shared fractions. Position gains are millimetres. Two starts failed on a
signal the stream was not corrected for, undetected by the resolver. Third-frequency integers are fixed with fitted biases and
do not improve the position. Nothing is in the producer; no bar is frozen.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

The golden posture does not run the integer-recovering chain of card IRD, and its emitted phase biases are held. Whether
a user's between-satellite ambiguities on that stream sit on integers, sit at a fraction common to every receiver, or
sit at fractions of their own, decides what a user can do with it and what the network has to add. The resolver of the
control gives the instruments: the float wide lane and, immune to the code and to the ionosphere, the N_1 float of the
ionosphere-free row.

## 2. Runs

- **Observe arm** (`obs`): consumer replay of `_rap_p26_n144/products.txt` at STPM00SPM, WTZZ00DEU and BAKE00CAN (none is
  a network receiver), between-satellite differences, static position, the b12 ambiguity-resolution block with
  `ratio_threshold` 1e9 so that no fix is ever accepted. The resolver log gives the float of every pair at every epoch.
- **Control twin:** the b12 fixed run of the same receiver and day with `ratio_threshold` 1e9 (CODE products).
- **Fix arm** (`fix`), reported only: the b12 block as it stands.

## 3. Readings

Per receiver and constellation, pairs above the 20 degree fixing mask, epochs after the first hour: the distribution of
the wide-lane fraction and of the fraction of the ionosphere-free N_1 float (with the wide lane rounded), on the
network's products and on CODE's; the same fractions per satellite pair compared between the three receivers.

## 4. Pre-flight

Release only. Nothing is armed and no posture changes. The readings are fractions of between-satellite quantities. A
fraction graded against a uniform null is quoted with that null (0.25 mean, 20 % within 0.1 cycle).

## 5. First readings on the golden product stream (2026-11-13, Release, diagnostic, not graded)

Executable `gnss_processor_rscb2.exe` (f8b252d706dd0e39). Observe runs `_rap_nw_obs_144_<site>` at the 17 test receivers
of the period (consumer replay of `_rap_p26_n144/products.txt`; `network.closed_loop.consumer_undifferenced` false,
`ar_mode` 1 and `ar_wl_only` false, static position, the b12 block with `ratio_threshold` 1e9), control twins
`_rap_aru_rsobs_est_144_{stpm,wtzz,bake}` (the b12 fixed runs with `ratio_threshold` 1e9). Tools `nw_arms.py`,
`nw_frac.py`, `nw_common.py`, `nw_fcb.py`. Fractions from the `CLFRAC` lines, pairs above the 20 degree fixing mask,
epochs after the first hour. Trap met: the closed-loop wrapper overlays its own `ar_wl_only` (default on), which applies
wide-lane-only constraints; the runs set it false.

### 5.1 The user's ambiguities carry no integers on the golden stream

Mean |fraction| and share within 0.10 cycle (uniform null 0.25 and 20 %):

| receiver, constellation | CODE products: wide lane | CODE products: N_1 of the ionosphere-free row | network products: wide lane | network products: N_1 |
| --- | --- | --- | --- | --- |
| STPM00SPM GPS | 0.119, 52.6 % | 0.045, 93.5 % | 0.252, 18.8 % | 0.252, 17.4 % |
| STPM00SPM Galileo | 0.103, 57.9 % | 0.037, 94.9 % | 0.230, 24.9 % | 0.252, 23.1 % |
| WTZZ00DEU GPS | 0.158, 30.5 % | 0.089, 64.6 % | 0.265, 19.4 % | 0.292, 8.0 % |
| WTZZ00DEU Galileo | 0.040, 95.0 % | 0.027, 98.9 % | 0.172, 29.4 % | 0.243, 17.6 % |
| BAKE00CAN GPS | 0.046, 98.0 % | 0.051, 81.8 % | 0.235, 18.9 % | 0.234, 26.4 % |
| BAKE00CAN Galileo | 0.055, 85.9 % | 0.067, 80.8 % | 0.267, 10.0 % | 0.227, 33.0 % |

### 5.2 The fractions are common to receivers that track the same signals

The same satellite pair in the same 30 min bin at two receivers, difference of the circular means (`nw_common.py`):

| receivers, constellation | pair-bins | network products: N_1, wide lane (mean, within 0.10) | CODE products: N_1, wide lane |
| --- | --- | --- | --- |
| STPM00SPM and WTZZ00DEU, Galileo (both L1X/L5X) | 51 | 0.057, 86.3 %; 0.065, 72.5 % | 0.039, 94.6 %; 0.063, 71.4 % |
| STPM00SPM and BAKE00CAN, Galileo (L1X/L5X against L1C/L5Q) | 64 | 0.240, 20.3 %; 0.249, 21.9 % | 0.068, 72.6 %; 0.142, 39.7 % |
| WTZZ00DEU and BAKE00CAN, Galileo (L1X/L5X against L1C/L5Q) | 27 | 0.233, 33.3 %; 0.266, 22.2 % | 0.040, 96.3 %; 0.065, 77.8 % |

Two receivers 5 000 km apart see the same fraction on the network's products when they track the same signals, nearly
as closely as they see integers on CODE products; a receiver on other signals of the same bands does not. The product
stream carries one phase bias per satellite and signal, and the two Galileo signal families sit in different frames.
Signals of the 17 receivers: Galileo L1X/L5X at 11 (ARHT, ASCG, BADG, CEDU, CHTI, CIBG, QUIN, STPM, UTQI, WTZZ, WUH2),
L1C/L5Q at 6 (BAKE, DAV1, HARB, KOKB, POHN, SCRZ); GPS L1C/L2W at 11, L1W/L2W at 6 (ARHT, BADG, BAKE, QUIN, WTZZ, WUH2).

### 5.3 Leave one receiver out: per-satellite offsets fitted on the others

Per signal group and time bin, one wide-lane offset and one N_1 offset per satellite, fitted to the pairs of every
receiver of the group but one (circular means, zero-mean datum; the N_1 fraction is formed with the wide-lane integer
taken after the wide-lane offsets are removed), then removed at the held-out receiver (`nw_fcb.py`). All held-out
receivers pooled, mean |fraction| and share within 0.10:

| group | receivers | wide lane: raw, with offsets | N_1: raw, with offsets | from hour 2.5, 90 min bins: wide lane, N_1 with offsets |
| --- | --- | --- | --- | --- |
| GPS L1C/L2W | 11 | 0.246, 22.3 %; 0.124, 53.6 % | 0.234, 23.1 %; 0.131, 52.7 % | 0.108, 59.6 %; 0.111, 60.2 % |
| GPS L1W/L2W | 6 | 0.264, 17.6 %; 0.143, 41.3 % | 0.250, 19.1 %; 0.092, 64.1 % | 0.139, 40.9 %; 0.086, 66.5 % |
| Galileo L1C/L5Q | 6 | 0.263, 15.4 %; 0.174, 45.6 % | 0.237, 19.7 %; 0.170, 38.1 % | 0.205, 38.2 %; 0.160, 42.0 % |
| Galileo L1X/L5X | 11 | 0.235, 22.1 %; 0.104, 58.6 % | 0.245, 19.8 %; 0.095, 62.4 % | 0.087, 67.3 %; 0.077, 70.0 % |

Best single receivers: CEDU00AUS (GPS 0.084 and 0.072, Galileo 0.054 and 0.062), STPM00SPM Galileo (0.085 and 0.081),
WTZZ00DEU Galileo (0.097 and 0.077). Receivers with few satellites in common with the others stay near the null
(SCRZ00BOL, UTQI00USA, KOKB00USA on GPS).

### 5.4 Reading

- On the golden stream the user's between-satellite ambiguity is an integer plus a fraction that belongs to the
  satellite pair and the signal family, not to the receiver. Per-satellite offsets from 5 to 10 other receivers bring a
  held-out receiver to 0.08 to 0.11 cycle on N_1; the control on CODE products sits at 0.03 to 0.09. The remainder is
  not separated here between the fit of the offsets (few receivers, their own float errors, the receiver-satellite code
  bias of card RSCB in the wide lane) and the stream itself.
- The offsets were fitted from users, offline. An emission-side transform of the stream was declined by the ruling of
  2026-11-09 (the chain first); nothing here builds one.
- Not done: fixing on a stream with the offsets removed; the chain posture on this day (the run is section 6 when
  it exists); more than one day; the time course of the offsets.

## 6. The chain posture on the field day (2026-11-13/14, Release, diagnostic, not graded)

Executable `gnss_processor_rscb3.exe` (5ecf8e1c765fafe3; the network path is that of `ar/user-control`, merge base with
`main` b6bbea7). Network runs of DOY 144, 56 receivers, 6 h, copies of `_rap_p26_n144/arm.json` with the key set of arm
A1krhRsm of `scripts/ird_make_arms.py` for the field bed (chain, pair family, the six event rows, floor off, receiver
phase bias in the block), without the log-only search and the lattice trace (`nw_chain.py`): `_rap_nwa1_n144` (emission
hold as in the golden, 825 s, 720 epochs, pin identity held in 720 of 720, 6 273 chain event rows) and
`_rap_nwa1nh_n144` (the same with `network.emit_pb_hold`, `emit_pb_hold_trip_generation`, `emit_pb_hold_phase_clock`
and the consumer's `pb_hold_presence_flip` off, 743 s). Observe runs at the 17 test receivers on each
(`_rap_nw_obsa1_*`, `_rap_nw_obsa1nh_*`).

| stream | raw fractions at STPM00SPM, WTZZ00DEU, BAKE00CAN (N_1, mean) | leave one out, from hour 2.5, 90 min bins: N_1 raw to with offsets (GPS L1C/L2W, GPS L1W/L2W, Galileo L1C/L5Q, Galileo L1X/L5X) |
| --- | --- | --- |
| golden (`_rap_p26_n144`) | GPS 0.252, 0.292, 0.234; Galileo 0.252, 0.243, 0.227 | 0.236 to 0.111, 0.244 to 0.086, 0.241 to 0.160, 0.244 to 0.077 |
| chain, emission hold on | GPS 0.249, 0.227, 0.278; Galileo 0.263, 0.258, 0.268 | 0.243 to 0.107, 0.246 to 0.086, 0.244 to 0.130, 0.245 to 0.075 |
| chain, emission hold off | GPS 0.241, 0.249, 0.265; Galileo 0.105, 0.244, 0.234 | 0.250 to 0.250, 0.249 to 0.253, 0.238 to 0.247, 0.229 to 0.255 |

- With the emission hold on, the chain's stream reads to a user as the golden's: no integers, and the same common
  fractions.
- With the hold off, a partial integer signal appears without any offset (STPM00SPM Galileo 0.104 wide lane and 0.105
  N_1; the Galileo L1X/L5X wide lane of the 11 receivers 0.147 against 0.244), N_1 stays at the null at 14 of 17
  receivers, GPS everywhere, and the fractions are no longer common to the receivers: offsets fitted on the others
  change nothing. The emitted biases then move every epoch and at every chain event.
- So on this field day the chain as built gives a user neither integers nor a fraction that others can supply.

## 7. Offsets from 73 receivers, a corrected copy of the golden stream, and fixing on it (2026-11-14, demonstration)

**Decision of the session (the author asleep, autonomy granted; for review).** The golden stream is the one whose
remainder is a stable number per satellite and signal. Its offsets were fitted offline and removed from a COPY of the
product file. This is the idea the ruling of 2026-11-09 declined as an emission-side transform; it is done here with
analysis tools only (no producer code, no key, no golden change), to show what a user gets when the fractions are
gone. The ruling stands until the author reopens it.

- **Observers.** The 17 test receivers and the network's own 56 receivers as users of `_rap_p26_n144/products.txt`
  (`nw_obs_net.py`, runs `_rap_nw_obsn_144_*`): GPS L1C/L2W 54, GPS L1W/L2W 19, Galileo L1X/L5X 39, Galileo L1C/L5Q 34.
- **Leave one out with 73 observers** (`nw_fcb.py --held`, from hour 1, 90 min bins), mean |fraction| with the others'
  offsets, wide lane and N_1: CEDU00AUS GPS 0.067 and 0.052, Galileo 0.035 and 0.043; POHN00FSM GPS 0.070 and 0.074,
  Galileo 0.055 and 0.059; QUIN00USA GPS 0.064 and 0.056, Galileo 0.068 and 0.070; STPM00SPM GPS 0.139 and 0.070,
  Galileo 0.094 and 0.061; WTZZ00DEU GPS 0.104 and 0.078, Galileo 0.049 and 0.044; BAKE00CAN GPS 0.106 and 0.060,
  Galileo 0.042 and 0.062. Pooled N_1 0.056 to 0.065 (78 to 86 % within 0.10): the level of the control on CODE products.
- **Offsets in time** (`nw_offsets.py`): per signal group, every 30 min over 90 min, w_s and n_s converted inside the
  window to cycles on the two signals (delta_hi = n - s w, delta_lo = delta_hi - w), each satellite's series made
  continuous (window datum aligned over shared satellites, integer representative nearest the previous value). STPM00SPM,
  WTZZ00DEU and BAKE00CAN are excluded from the fit. The two GPS groups share L2W and the wide-lane part of an offset
  comes from the code as well as the phase, so one L2W series cannot serve both (the L1W group's N_1 misfit under the
  L1C group's L2W is 0.203 cycle): one file per GPS group, `offsets_p26_n144_x3_gc.json` (L1C/L2W) and `..._gw.json`
  (L1W/L2W).
- **Corrected streams** (`nw_rewrite.py`, sign +1 settled by the self-check: the other sign leaves every fraction at
  the null): `_rap_nwgc_n144/products.txt` and `_rap_nwgw_n144/products.txt`, the P records of 125 163 and 125 155 of
  147 886 changed, nothing else.
- Trap met and fixed: making w and n continuous separately breaks the pair (the N_1 offset is formed with the window's
  own wide-lane representative); the continuity is applied to the converted per-signal values, where only integers are
  free.

Observe runs on the corrected streams, mean |fraction| (share within 0.10), the three held-out receivers and two that
were in the fit:

| receiver (stream) | GPS wide lane | GPS N_1 | Galileo wide lane | Galileo N_1 | N_1 on CODE products, GPS and Galileo (5.1) |
| --- | --- | --- | --- | --- | --- |
| STPM00SPM held out (gc) | 0.189 (34 %) | 0.056 (84 %) | 0.109 (49 %) | 0.072 (70 %) | 0.045, 0.037 |
| WTZZ00DEU held out (gw) | 0.095 (59 %) | 0.077 (78 %) | 0.041 (95 %) | 0.025 (99.5 %) | 0.089, 0.027 |
| BAKE00CAN held out (gw) | 0.106 (49 %) | 0.058 (78 %) | 0.030 (97 %) | 0.056 (89 %) | 0.051, 0.067 |
| CEDU00AUS in the fit (gc) | 0.056 (86 %) | 0.039 (91 %) | 0.036 (96 %) | 0.035 (94 %) | |
| QUIN00USA in the fit (gw) | 0.032 (98 %) | 0.075 (77 %) | 0.053 (88 %) | 0.074 (71 %) | |

Fixing at the held-out receivers on the corrected streams (static position; `fix` the b12 block, `fxb` with
`wl_branch_check`; the float is the observe run on the same stream; runs `_rap_nw_{fix,fxb}g{c,w}_144_<site>`, late
starts `_rap_nw_fxbg{c,w}l_144_<site>` on the trimmed observation files of the b12 late runs):

| receiver | held share GPS: fix, fxb | held share Galileo: fix, fxb | first hold (min): GPS, Galileo | 00:00 against 01:00 start (`nw_int_compare.py`): events compared, different | 3D median per hour (mm): float | 3D median per hour (mm): fxb | control on CODE products, held GPS and Galileo |
| --- | --- | --- | --- | --- | --- | --- | --- |
| STPM00SPM | 69.1, 86.6 | 92.0, 92.0 | 22, 22 | 201, 0 | 22.5 9.7 9.0 8.7 15.8 14.1 | 16.7 7.8 9.9 8.9 14.6 16.8 | 86.0, 93.1 |
| WTZZ00DEU | 88.2, 90.5 | 91.6, 91.6 | 20, 20 | 187, 0 | 67.6 13.1 9.7 8.2 7.6 8.4 | 47.2 11.9 8.6 6.7 6.6 7.5 | 90.3, 95.2 |
| BAKE00CAN | 86.7, 94.2 | 94.6, 94.8 | 19, 18 | 130, 0 | 54.7 13.8 14.2 15.0 11.1 9.6 | 38.6 11.8 11.8 12.9 10.2 9.1 | 97.4, 96.1 |

**Reading.** On the network's golden products with the per-satellite offsets of 70 other receivers removed, three
receivers outside the fit hold 87 to 95 % of their GPS and 92 to 95 % of their Galileo pair-epochs on integers from
about minute 20, the integers of the runs from 00:00 and from 01:00 agree on all 518 compared events, and the static
position is better than the float in 15 of 18 receiver-hours. These are the first integers held at a user
of the network's own products in this project.

**What this does not show.** One day and three receivers. The offsets use data on both sides of each window (not causal)
and come from the same six hours; 53 of the 70 observers are network receivers. The position is static. The
receiver-satellite code-bias state of card RSCB did not run (the loader clears `estimate_rx_sat_code_bias` when the
configuration is the network's, so the replay consumer never gets it); the phase check carries the GPS wide lane
instead (STPM00SPM 69.1 to 86.6 %). No bar was frozen for any of it.

## 8. Integers on a third and further frequency: a probe on CODE products (2026-11-14, diagnostic)

Question of the author: integers on three or on every frequency. Probe: the b12 float runs of DOY 144 with only
`processing.frequency_mode` set to `all` (`tf_probe.py`, runs `_rap_aru_tfallflt_est_144_<site>`, executable
5ecf8e1c765fafe3). Signals carried: Galileo E1, E5a, E6, E5b, E5 AltBOC; GPS L1, L2, L5. For every pair of signals of
one satellite, the fraction of the float between-satellite difference n_a - n_b (arcs older than 30 min, epochs after
the first hour), mean (share within 0.10 cycle):

| combination (wavelength) | STPM00SPM | WTZZ00DEU | BAKE00CAN |
| --- | --- | --- | --- |
| Galileo E1 - E5a (0.751 m) | 0.103 (61 %) | 0.045 (91 %) | 0.052 (86 %) |
| Galileo E5a - E5b (9.768 m) | 0.259 (22 %) | 0.259 (18 %) | 0.269 (24 %) |
| Galileo E5a - E6 (2.931 m) | 0.254 (16 %) | 0.252 (22 %) | 0.234 (29 %) |
| Galileo E5b - E5 AltBOC (19.537 m) | 0.126 (51 %) | 0.112 (50 %) | 0.105 (64 %) |
| GPS L1 - L2 (0.862 m) | 0.108 (54 %) | 0.126 (40 %) | 0.053 (90 %) |
| GPS L2 - L5 (5.861 m) | 0.231 (23 %) | 0.274 (10 %) | 0.230 (21 %) |

- Only the pair the products carry biases for is on integers. An extra-wide lane of 5.9 or 9.8 m at the uniform null
  cannot be a code or ionosphere effect; the cause is in the products: `COD0MGXFIN_20261440000_01D_01D_OSB.BIA` holds
  Galileo biases for C1C, C1X, C5Q, C5X, L1C, L1X, L5Q, L5X and GPS biases on bands 1 and 2 only, nothing for GPS L5 or
  Galileo E5b, E6, E5 AltBOC.
- The float position with every frequency is 17.7, 6.9 and 17.3 mm (median 3D, minutes 60 to 360) against 8.9, 9.3 and
  9.1 mm with two: the extra signals without biases do not help at two of three receivers.
- So integers on a third frequency need satellite biases for that frequency first. Neither CODE's file nor the
  network's golden products (two bands) carry them. The offsets of section 7 are the same kind of quantity, per
  satellite and signal, and could be fitted for an extra signal from many receivers; the resolver would then need a
  step for more than two signals per satellite (`resolveWlConstrained` takes exactly two). Not built.

## 9. The network's own double differences, read on the wide lane and the ionosphere-free row (2026-11-14, diagnostic)

`network_dd_fractional.csv` of the network runs writes one row per traced double difference and SIGNAL. `nw_net_dd.py`
combines the two signals of one double difference (same epoch, station pair, satellite pair, signals of one receiver
family): the wide lane dd_hi - dd_lo and the N_1 float of the ionosphere-free row with the wide lane rounded. From hour
2, mean |fraction| (share within 0.10 cycle; uniform null 0.25 and 20 %):

| run | signals | each signal alone | wide lane | N_1 of the ionosphere-free row |
| --- | --- | --- | --- | --- |
| golden, DOY 144 (`_rap_p26_n144`) | Galileo L1C/L5Q | 0.164, 0.196 | 0.058 (86 %) | 0.058 (82 %) |
| | Galileo L1X/L5X | 0.220, 0.230 | 0.113 (57 %) | 0.107 (60 %) |
| | GPS L1C/L2W | 0.226, 0.245 | 0.095 (64 %) | 0.104 (59 %) |
| | GPS L1W/L2W | 0.203, 0.242 | 0.061 (94 %) | 0.074 (74 %) |
| golden, DOY 143 | the four pairs, same order | | 0.088, 0.111, 0.102, 0.019 | 0.075, 0.090, 0.129, 0.085 |
| golden, DOY 145 | the four pairs | | 0.082, 0.084, 0.090, 0.084 | 0.070, 0.056, 0.123, 0.106 |
| chain, DOY 144 (hold on and hold off, the same solve) | the four pairs | | 0.092, 0.120, 0.128, 0.194 | 0.095, 0.135, 0.151, 0.094 |

- The golden network's float double differences are integers on the wide lane and on the ionosphere-free row, at 0.06
  to 0.13 cycle, on every day read. Read one signal at a time they sit near the uniform null: a per-signal ambiguity
  carries the slant ionosphere of its line of sight, which takes up whatever the code leaves (the receiver-satellite
  code bias of card RSCB among it). The per-signal reading is the one the trace offers.
- The chain's double differences are less integer than the golden's on this day on three of the four signal pairs.
- This is the network-side face of sections 5 and 7: receivers share the fractions of their between-satellite
  ambiguities, so the difference between two receivers is an integer.

## 10. Nine receivers held out, the kinematic position, and offsets from the past only (2026-11-14, DOY 144, demonstration)

Driver `nw_day.py` (offsets, corrected streams, runs and grades of one day). Executable 5ecf8e1c765fafe3.

### 10.1 Nine receivers out of the fit at once

Offsets fitted without STPM, WTZZ, BAKE, CEDU, QUIN, POHN, ASCG, HARB and WUH2 (64 observers left), streams
`_rap_nwgc9_n144` and `_rap_nwgw9_n144`, fixing with the phase check, static (`_rap_nw_fxbg{c,w}9_144_<site>`):

| receiver (GPS signals, Galileo signals) | held GPS | held Galileo | first hold (min) |
| --- | --- | --- | --- |
| STPM00SPM (L1C/L2W, L1X/L5X) | 86.8 | 91.9 | 22 |
| WTZZ00DEU (L1W/L2W, L1X/L5X) | 90.1 | 91.9 | 20 |
| BAKE00CAN (L1W/L2W, L1C/L5Q) | 94.4 | 94.8 | 19 |
| CEDU00AUS (L1C/L2W, L1X/L5X) | 93.8 | 95.0 | 28 |
| QUIN00USA (L1W/L2W, L1X/L5X) | 89.3 | 85.8 | 20 |
| POHN00FSM (L1C/L2W, L1C/L5Q) | 91.6 | 91.5 | 26 |
| ASCG00SHN (L1C/L2W, L1X/L5X) | 86.2 | 92.8 | 20 |
| HARB00ZAF (L1C/L2W, L1C/L5Q) | 72.2 | 42.5 | 54 |
| WUH200CHN (L1W/L2W, L1X/L5X) | 88.4 | 89.4 | 20 |

Eight of nine receivers hold 86 to 95 % on both constellations. HARB00ZAF does not (its kinematic twin holds 7 and
15 % with 113 contradictions in waiting, and its float is 4 to 9 cm); not traced.

### 10.2 Kinematic position

The same runs with the base position process noise (`--kin`), 3D median per hour (mm), float then fixed:

| receiver | float | fixed |
| --- | --- | --- |
| STPM00SPM | 29.4 28.4 55.1 27.8 14.8 13.4 | 20.6 25.1 48.3 23.3 12.9 13.6 |
| WTZZ00DEU | 54.8 16.0 9.3 9.4 7.7 8.7 | 41.9 16.7 11.0 5.9 9.9 9.3 |
| BAKE00CAN | 166.7 21.0 21.5 31.4 27.2 36.5 | 166.7 10.6 14.4 26.9 20.9 33.2 |
| CEDU00AUS | 78.4 19.4 32.5 21.1 15.8 14.6 | 68.7 13.1 25.8 19.9 13.9 12.6 |
| QUIN00USA | 74.4 35.1 38.1 26.8 21.0 18.1 | 53.1 23.0 38.3 25.7 25.3 22.7 |
| POHN00FSM | 153.1 90.5 30.6 33.1 37.4 22.8 | 149.7 44.4 22.5 30.9 33.8 22.7 |
| ASCG00SHN | 60.8 22.5 110.4 51.1 33.2 44.2 | 58.4 22.3 115.4 50.0 21.1 29.7 |
| WUH200CHN | 129.0 62.2 23.1 46.9 88.8 36.7 | 129.0 54.2 23.7 45.6 90.4 38.0 |

The fixed run is the better one in 34 of 48 receiver-hours, by a few millimetres in most; the larger gains are in the
first two hours (POHN00FSM 90.5 to 44.4, BAKE00CAN 21.0 to 10.6, CEDU00AUS 19.4 to 13.1). Held integers do not turn the
kinematic position of these runs into a millimetre one: the error left is not the ambiguities'.

### 10.3 Offsets from the past only

`nw_offsets.py --causal` (each value from the 90 min before it, every 30 min) and `nw_rewrite.py --hold` (the latest
value held, nothing before the first value at 01:00), streams `_rap_nwcgc_n144` and `_rap_nwcgw_n144`, users from
01:00 on the trimmed observation files. N_1 mean |fraction| before fixing and held share, against the two-sided
offsets with the same start (a run from 01:00 continues to 07:00, one hour past the product file, so its held shares
are below those of 7 and 10.1):

| receiver | N_1 before fixing, GPS and Galileo: two-sided, causal | held GPS: two-sided, causal | held Galileo: two-sided, causal |
| --- | --- | --- | --- |
| STPM00SPM | 0.056, 0.072; 0.163, 0.103 | 71.5, 67.4 | 81.6, 78.4 |
| WTZZ00DEU | 0.077, 0.025; 0.158, 0.088 | 73.6, 67.7 | 83.1, 80.6 |
| BAKE00CAN | 0.058, 0.056; 0.094, 0.106 | 80.5, 73.8 | 78.4, 77.8 |

A crude causal fit (users' floats in a trailing window, a step every half hour) keeps most of the fixing and loses 4 to
7 points of GPS held share. Integers of two differently corrected streams are not comparable (the corrections may
differ by whole cycles per satellite); that comparison was run and is discarded.

## 11. Every frequency on CODE products: the missing biases can be fitted (2026-11-14, DOY 144, demonstration)

Section 8 found the extra signals of an all-frequency run at the uniform null because the bias file has nothing for
them. The same remedy as section 7, now on CODE products: all-frequency float runs at the 17 test receivers
(`tf_fcb.py`, runs `_rap_aru_tfallflt_est_144_<site>`), one offset per satellite and pair of signals fitted on the
other receivers per 90 min bin (leave one receiver out). Mean |fraction| of the float between-satellite difference
n_a - n_b (share within 0.10 cycle), raw and with the others' offsets:

| combination (wavelength) | receivers | raw | with the others' offsets |
| --- | --- | --- | --- |
| Galileo E5a - E5b, L5X-L7X (9.768 m) | 10 | 0.259 (21 %) | 0.008 (100 %) |
| Galileo E5a - E5b, L5Q-L7Q | 6 | 0.251 (27 %) | 0.015 (98 %) |
| Galileo E5a - E5 AltBOC, L5X-L8X (19.537 m) | 10 | 0.254 (19 %) | 0.005 (100 %) |
| Galileo E5a - E6, L5X-L6X (2.931 m) | 6 | 0.258 (18 %) | 0.030 (99 %) |
| Galileo E6 - E5b, L6X-L7X (4.186 m) | 6 | 0.255 (19 %) | 0.023 (99.5 %) |
| Galileo E1 - E5b, L1X-L7X (0.814 m) | 10 | 0.261 (20 %) | 0.070 (75 %) |
| GPS L2 - L5, L2W-L5X (5.861 m) | 11 | 0.259 (19 %) | 0.041 (93 %) |
| GPS L2 - L5, L2W-L5Q | 6 | 0.252 (21 %) | 0.039 (94 %) |
| GPS L1 - L5, L1C-L5X (0.751 m) | 6 | 0.255 (21 %) | 0.191 (32 %) |
| the pair the file covers: Galileo L1X-L5X, GPS L1C-L2W | 11, 11 | 0.078, 0.078 | 0.081, 0.093 (offsets add only their noise) |

An augmented bias file: `tf_bias.py` fits one offset per satellite and extra signal for the whole run (extra-wide lane
against the covered signal of the lower band: Galileo band 5, GPS band 2; STPM00SPM, WTZZ00DEU and BAKE00CAN excluded)
and adds one phase OSB line per (satellite, extra signal) to a copy of
`COD0MGXFIN_20261440000_01D_01D_OSB.BIA` (218 lines; value = -offset x wavelength / c in ns, the sign settled by the
self-check, the other sign leaving the null). The copy sits in a private data root
(`D:\GnssProcessorSupport\runs_nom\_tf_data`; its `static`, `2026-143` and `2026-145` are junctions into
`C:\GnssProcessor\data`: that folder must never be deleted recursively). All-frequency float runs of the three
held-out receivers on it (`tf_runs.py`, `_rap_aru_tfbflt_est_144_<site>`), mean |fraction| (share within 0.10):

| combination | STPM00SPM | WTZZ00DEU | BAKE00CAN |
| --- | --- | --- | --- |
| Galileo E5a - E5b (9.768 m) | 0.010 (100 %) | 0.005 (100 %) | 0.011 (100 %) |
| Galileo E5a - E5 AltBOC (19.537 m) | 0.007 (100 %) | 0.005 (100 %) | 0.006 (100 %) |
| Galileo E5a - E6 (2.931 m) | 0.125 (69 %, L6B, 3 observers) | 0.024 (100 %, L6X) | 0.234 (L6C, no other receiver tracks it) |
| Galileo E1 - E5b (0.814 m) | 0.096 (64 %) | 0.044 (92 %) | 0.047 (90 %) |
| GPS L2 - L5 (5.861 m) | 0.101 (64 %) | 0.069 (79 %) | 0.078 (75 %) |
| GPS L1 - L5 (0.751 m) | 0.156 (31 %) | 0.128 (46 %) | 0.057 (89 %) |

**Reading.** With a bias per satellite for each extra signal, fitted from 14 other receivers, the Galileo extra-wide
lanes of a held-out receiver are within 0.005 to 0.011 cycle of integers in the processor itself, and GPS L2 - L5
within 0.07 to 0.10. The integers of the third and further frequencies are there; the product lacks the biases. What
remains for a fix on every frequency is a resolver step for more than two signals (`resolveWlConstrained` takes exactly
two), in progress on the branch.

## 12. More days, and the held integers checked against CODE products (2026-11-14, demonstration)

### 12.1 The demonstration repeated on 23 and 25 May (DOY 143, 145)

`nw_day.py <doy>` runs sections 7 and 10.2 end to end for one day, with nothing tuned between days: observe runs of
the 17 test receivers and of the network's 56 receivers on the day's golden products (`_rap_p26_n<doy>`; 73 of 73
runs ended with exit 0 on both days), offsets without STPM00SPM, WTZZ00DEU and BAKE00CAN, the corrected streams
`_rap_nwgc_n<doy>` and `_rap_nwgw_n<doy>`, and at each held-out receiver the float, the fixing run with
`wl_branch_check` from 00:00 and from 01:00 (static), and the float and the fixing run with the base position noise
(kinematic). Executable `gnss_processor_rscb3.exe`.

Observers in the fit (the three excluded): GPS L1C/L2W 52 (DOY 143) and 53 (DOY 145), GPS L1W/L2W 17, Galileo L1X/L5X
37, Galileo L1C/L5Q 33. Step of a satellite's offset between windows 30 min apart, median (90th percentile), DOY 143
and 145: GPS L1C/L2W 0.053 (0.165) and 0.054 (0.165) cycle, GPS L1W/L2W 0.066 (0.182) and 0.072 (0.214), Galileo
L1X/L5X 0.035 (0.118) and 0.036 (0.113), Galileo L1C/L5Q 0.044 (0.200) and 0.040 (0.156).

Static position, fixing with `wl_branch_check` from 00:00 (runs `_rap_nw_fxbg{c,w}_<doy>_<site>`; the float is the
observe run on the same corrected stream; DOY 144 from section 7):

| day | receiver | held share (%): GPS, Galileo | first hold (min): GPS, Galileo | 00:00 against 01:00 start: events compared, different | 3D median per hour (mm): float | 3D median per hour (mm): fixed |
| --- | --- | --- | --- | --- | --- | --- |
| 143 | STPM00SPM | 73.9, 77.0 | 30, 18 | 168, 0 | 49.5 19.7 23.9 21.4 21.0 30.1 | 39.3 14.8 21.2 19.0 26.9 50.4 |
| 143 | WTZZ00DEU | 91.1, 93.3 | 18, 18 | 222, 0 | 37.9 14.6 12.7 11.1 5.5 4.5 | 9.1 7.9 9.2 10.0 5.7 4.9 |
| 143 | BAKE00CAN | 93.2, 93.4 | 21, 36 | 185, 0 | 60.1 18.2 9.7 7.9 9.2 10.8 | 54.5 10.8 7.3 6.6 8.3 10.3 |
| 144 | STPM00SPM | 86.6, 92.0 | 22, 22 | 201, 0 | 22.5 9.7 9.0 8.7 15.8 14.1 | 16.7 7.8 9.9 8.9 14.6 16.8 |
| 144 | WTZZ00DEU | 90.5, 91.6 | 20, 20 | 187, 0 | 67.6 13.1 9.7 8.2 7.6 8.4 | 47.2 11.9 8.6 6.7 6.6 7.5 |
| 144 | BAKE00CAN | 94.2, 94.8 | 19, 18 | 130, 0 | 54.7 13.8 14.2 15.0 11.1 9.6 | 38.6 11.8 11.8 12.9 10.2 9.1 |
| 145 | STPM00SPM | 78.0, 47.5 | 22, 18 | 61, 0 | 27.9 23.5 19.4 20.3 16.6 20.7 | 30.3 21.8 19.4 19.4 12.2 15.0 |
| 145 | WTZZ00DEU | 90.2, 94.4 | 20, 20 | 158, 0 | 108.3 14.8 23.5 26.6 16.4 14.5 | 65.7 14.6 18.1 24.1 16.0 14.1 |
| 145 | BAKE00CAN | 85.2, 88.1 | 20, 19 | 284, 0 | 55.6 7.9 7.5 6.7 9.6 7.2 | 33.2 8.3 4.5 8.3 10.3 7.9 |

Kinematic position (base position noise; runs `_rap_nw_obsg{c,w}k_<doy>_<site>` and `_rap_nw_fxbg{c,w}k_<doy>_<site>`):

| day | receiver | held share (%): GPS, Galileo | 3D median per hour (mm): float | 3D median per hour (mm): fixed |
| --- | --- | --- | --- | --- |
| 143 | STPM00SPM | 61.9, 53.3 | 71.0 33.4 29.4 28.1 55.1 120.7 | 71.4 23.4 26.1 26.1 70.3 79.6 |
| 143 | WTZZ00DEU | 90.9, 93.2 | 44.1 16.7 14.4 22.8 16.6 12.7 | 13.7 10.1 8.6 19.5 17.0 11.1 |
| 143 | BAKE00CAN | 89.8, 93.3 | 93.3 24.2 20.3 13.9 9.0 14.7 | 95.0 10.6 8.1 13.8 8.7 15.2 |
| 145 | STPM00SPM | 90.1, 51.6 | 46.3 18.2 23.0 13.0 15.4 22.7 | 48.6 15.2 22.0 11.8 12.3 17.2 |
| 145 | WTZZ00DEU | 91.2, 95.4 | 99.7 26.4 26.5 45.5 35.5 42.3 | 101.0 14.9 14.7 30.7 30.6 35.4 |
| 145 | BAKE00CAN | 80.1, 87.6 | 80.9 40.4 16.9 12.6 13.0 10.1 | 77.1 19.6 17.1 15.5 12.5 9.0 |

**Reading.** On three days the integers of the starts at 00:00 and at 01:00 agree on every compared event (575, 518
and 503; 1 596 in all). WTZZ00DEU holds 90 to 94 % on every day and BAKE00CAN 85 to 95 %. STPM00SPM is the weak
receiver: 74 to 87 % GPS and 48 to 92 % Galileo, with 47 contradictions of the phase check on DOY 143 (253 in the
run from 01:00) and a float that stays 2 to 5 cm from the reference on DOY 143 (WTZZ00DEU and BAKE00CAN reach 0.5 to
1 cm on that day) and about 2 cm on DOY 145. That weakness is not traced (the receiver-satellite code-bias state of
card RSCB does not run in the replay consumer, which leaves the GPS wide lanes of this receiver to the phase check;
it does not explain the Galileo share of DOY 145 or the float). The fixed static position is better than the float
in 14 of 18 receiver-hours on DOY 143 and in 12 of 18 on DOY 145 (one equal); the fixed kinematic position in 13 and
14 of 18. The position gains stay at the millimetre to centimetre level: on DOY 145 WTZZ00DEU holds 90 % of its
pair-epochs and its static position stays 14 to 24 mm from the reference, so the error that remains is in the
products, not in the ambiguities. The runs from 01:00 continue past the end of the 6 h product file and their held
shares (28 to 85 %) are not comparable with the runs from 00:00.

### 12.2 The held integers against the integers on CODE products (DOY 143 to 145)

The comparison of two start times (section 7) reads one product stream twice. An independent check: for one receiver,
the integers of a satellite pair held on two different products differ by a whole number that belongs to the products
(the difference of the satellites' phase biases), not to the receiver. Two receivers that track the same signals must
therefore show the same difference for the same satellites at the same time,

    [N_A(network) - N_A(CODE)](i, j, t) = [N_B(network) - N_B(CODE)](i, j, t),

which states that the double-difference integers between the two receivers are the same on both products. The
integers on CODE products depend on nothing this project estimates.

`nw_dd_check.py` reads the held integers of every epoch from the resolver log (a `WLFLOAT` line with |fraction| < 0.01
and sigma < 0.01, and the `CLFRAC` lines of the same pair) in the fixing run on the corrected network stream and in
the fixing control on CODE products (`_rap_aru_b12fix_est_<doy>_<site>`; for the six further receivers of section
10.1 a copy of that control with the receiver's observation file, `nw_code_fix.py`, runs
`_rap_aru_c9fix_est_144_<site>`). Per receiver and epoch d(i) = N_network(i) - N_CODE(i) is formed over the satellites
held in both runs; across two receivers e(i) = d_A(i) - d_B(i) must be one constant over the common satellites, and a
satellite away from the mode counts as different. Epochs with fewer than two common satellites are not comparable.
The two corrected streams carry different GPS corrections (one per signal group) and identical Galileo records (0
differing lines on the three days), so receivers are compared inside one GPS signal group, and on Galileo across both
streams. The control on CODE products runs in the post-processing engine and the network runs in the replay engine,
so the check also crosses the two code paths.

| day, receivers held out of the fit | pairs of receivers | signals | satellite-epochs compared | distinct (satellite pair, integer) combinations | different |
| --- | --- | --- | --- | --- | --- |
| 143, three | STPM00SPM and WTZZ00DEU | Galileo L1X/L5X | 730 | 8 | 0 |
| 143, three | BAKE00CAN and WTZZ00DEU | GPS L1W/L2W | 237 | 5 | 0 |
| 144, three | STPM00SPM and WTZZ00DEU | Galileo L1X/L5X | 1 066 | 11 | 0 |
| 144, three | BAKE00CAN and WTZZ00DEU | GPS L1W/L2W | 207 | 5 | 0 |
| 145, three | STPM00SPM and WTZZ00DEU | Galileo L1X/L5X | 191 | 3 | 0 |
| 145, three | BAKE00CAN and WTZZ00DEU | GPS L1W/L2W | 232 | 5 | 0 |
| 144, nine (the streams of section 10.1) | 8 pairs | Galileo L1X/L5X, L1C/L5Q | 3 019 | 43 | 0 |
| 144, nine (the streams of section 10.1) | 9 pairs | GPS L1C/L2W, L1W/L2W | 2 892 | 54 | 1 combination (36 satellite-epochs) |

The 17 pairs of the nine-receiver set: Galileo ASCG-STPM, ASCG-WTZZ, BAKE-POHN, CEDU-WUH2, QUIN-STPM, QUIN-WTZZ,
STPM-WTZZ, WTZZ-WUH2; GPS ASCG-HARB, ASCG-STPM, BAKE-QUIN, BAKE-WTZZ, BAKE-WUH2, CEDU-HARB, CEDU-POHN, QUIN-WTZZ,
WTZZ-WUH2.

**The one difference.** G29 at ASCG00SHN against STPM00SPM, hour 5: one cycle on both signals, the wide lane equal,
constant over the arc in each of the four runs. G29 crossed the Earth's shadow from 01:22:30 to 01:43:30; both engines
drop it there (the observation file carries it throughout). In the control's wind-up trace the accumulated wind-up of
G29 at ASCG00SHN stood one whole turn from the wrapped angle before the shadow (0.569 cycle against -0.431) and
restarted at the wrapped angle after it. A whole wind-up turn moves both signals by one cycle and leaves the wide
lane, which is the pattern seen. Five such restarts with a dropped turn exist among the nine receivers on the day
(ASCG00SHN G29 and G27, HARB00ZAF E18 and G29, POHN00FSM G02); this is the only one that takes part in a comparison
after the restart, and it is the only difference. The replay engine writes no wind-up trace, so whether it keeps the
turn was not traced; a rerun of the control with `estimation.phase_windup_gap_bridge` (3 600 s) still restarts at the
shadow and leaves the difference. The difference is attributed to the wind-up turn count of the two engines on that
arc, not proven.

**Reading.** On 24 May, with nine receivers held out of the fit, the double-difference integers of 17 pairs of
receivers are the same on the corrected network products as on CODE products in 96 of 97 distinct combinations, and
the three-receiver sets of three days agree in all 37. The one difference has the signature of a whole wind-up turn
on an arc that crossed the Earth's shadow. The integers a user holds on the network's corrected products are the
integers CODE's products give. HARB00ZAF, whose held share is low (section 10.1), agrees where it holds (ASCG-HARB 7
combinations, CEDU-HARB 2).

**Limits.** Distant receivers share few satellites, so a pair of receivers yields 1 to 14 combinations; the check
covers held pair-epochs only; the three-receiver and nine-receiver rows of DOY 144 share receivers and data.

## 13. The integers of a third and further frequency, fixed (2026-11-14, DOY 144, CODE products with the augmented bias file, demonstration)

### 13.1 The resolver step (built on the branch, default off)

`resolveWlConstrained` processed a satellite pair only when it had exactly two signals, so an all-frequency run fixed
nothing (section 8). The step added on branch `ar/rx-sat-code-bias` (uncommitted; keys
`estimation.ambiguity_resolution.extra_signals`, default false, and `estimation.ambiguity_resolution.primary_bands`,
default GPS "12", Galileo "15", BeiDou "26"; ledger rows UNVALIDATED):

- The two signals on the constellation's primary bands go through the wide-lane and N_1 step exactly as before. With
  the key off the rule is the one from before (exactly two signals) and the output is unchanged.
- Every other signal x of the pair gets an extra-wide lane against the lower primary signal, e = n_lo - n_x in cycles,
  read from the float states (`EWLFLOAT` line). It is admitted when |e - round(e)| + 3 sigma_e <= `wl_max_frac`. An
  extra-wide lane is a difference of cycle counts, so no division by a frequency difference amplifies an error in it.
- When the primary pair is fixed, an admitted extra signal gets N_x = N_lo - round(e) as one more pseudo-observation
  row with the same hold variance (`EWL pair` line), and the hold and its release cover it (`hold_release_cyc` compares
  the held extra-wide lane with the float).
- Code: `admitExtraWideLane`, `splitPrimaryAndExtra`, `AmbiguityResolver::evaluateExtraWideLanes_`, the `addExtraRows`
  step of both fix paths, `HeldPairFix::extras`, the run-end line "Extra signals" (evaluated, admitted, fixed, held
  pairs released by an extra-wide lane). Tests: `tests/test_ar_extra_signals.cpp`, 36 tests in 4 suites; with the
  resolver suites of the branch 96 tests in 10 suites pass in the Debug build.
- Executable `gnss_processor_rscb4.exe` (sha256[:16] 17c2aeaee9e686b0). With the new keys off it reproduces the earlier
  build byte for byte on both engines: the fixing control of STPM00SPM on CODE products (state, ambiguity, residual
  and wind-up traces equal to the runs of `gnss_processor_rscb3.exe` and of the control's own executable) and the
  replay run `_rap_nw_fxbgw_144_wtzz` (the three closed-loop traces and every `WLFLOAT`, `NL pair` and `WLBRANCH`
  line equal).

### 13.2 Runs

Copies of the b12 fixing control of card ARUB on CODE products (`tf_runs.py`), data root with the augmented bias file
of section 11 (offsets fitted without STPM00SPM, WTZZ00DEU and BAKE00CAN), `extra_signals` on; the run from 01:00 is
the copy of the control's late run. Three signal sets:

- every frequency (`processing.frequency_mode` all): runs `_rap_aru_tfx_est_144_<site>`, late `_rap_aru_tfxl_...`,
  float `_rap_aru_tfbflt_...`;
- three frequencies, GPS L1/L2/L5 and Galileo E1/E5a/E5b (`frequency_mode` triple): `_rap_aru_t3x_...`,
  `_rap_aru_t3xl_...`, float `_rap_aru_t3flt_...`;
- the third frequency on Galileo only (triple, with the GPS priority list cut to "WCSLPYM" so that no GPS L5 signal
  is taken): `_rap_aru_t3gx_...`, `_rap_aru_t3gxl_...`, float `_rap_aru_t3gflt_...`.

Readers: `tf_extra.py` (extra signals: held share of the `EWLFLOAT` pair-epochs after the first 30 min, fix events,
the integers of the two starts), `grade_arm.py` (the primary pair against the control), `tf_pos.py` (position).

### 13.3 The integers

Extra signals, every frequency. Per cell: the held share of pair-epochs, then the fix events of the start at 00:00
that have a counterpart in the start at 01:00 (same satellite pair, reference and signals, within 900 s), then how
many of those differ.

| receiver | Galileo E5a - E5b | Galileo E5a - E5 AltBOC | Galileo E5a - E6 | GPS L2 - L5 | all extra signals: fix events, compared, different |
| --- | --- | --- | --- | --- | --- |
| STPM00SPM | 98.7 %, 482, 0 | 99.4 %, 482, 0 | 92.5 % (L6B), 438, 0 | 95.0 %, 218, 0 | 2 148, 1 620, 0 |
| WTZZ00DEU | 99.2 %, 192, 0 | 99.3 %, 192, 0 | 96.4 % (L6X), 179, 0 | 94.6 % (280 pair-epochs), 20, 0 | 1 592, 583, 0 |
| BAKE00CAN | 98.8 %, 254, 0 | 99.8 %, 254, 0 | no E6 extra-wide lane | 78.7 %, 275, 0 | 1 875, 783, 0 |

No held pair was released by an extra-wide lane in any run. First hold of an extra signal at minute 15 to 24 (GPS L5
at WTZZ00DEU, 280 pair-epochs in all, at minute 188).

The same on the two smaller sets (held share; compared, different):

| receiver | three frequencies: Galileo E5a - E5b | three frequencies: GPS L2 - L5 | third frequency on Galileo only: E5a - E5b |
| --- | --- | --- | --- |
| STPM00SPM | 98.9 %; 514, 0 | 95.2 %; 233, 0 | 99.6 %; 252, 0 |
| WTZZ00DEU | 99.5 %; 270, 0 | 54.6 % (280 pair-epochs); 20, 0 | 99.3 %; 270, 0 |
| BAKE00CAN | 99.3 %; 210, 0 | 78.0 %; 254, 0 | 99.4 %; 49, 0 |

The primary pair keeps its integers. Against the two-frequency control (pair-arcs compared, different) and between
the two starts, every frequency: STPM00SPM 81, 0 and 77, 0; WTZZ00DEU 66, 0 and 51, 0; BAKE00CAN 77, 0 and 69, 0.
Three frequencies: 81, 0 and 77, 0; 67, 0 and 50, 0; 81, 0 and 66, 0. Third frequency on Galileo only: 86, 0 and
74, 0; 70, 0 and 57, 0; 81, 0 and 61, 0.

### 13.4 The position

Static position, median 3D error of hours 2 to 6 (mm):

| receiver | two frequencies: float, fixed | every frequency: float, fixed | three frequencies: float, fixed | third frequency on Galileo only: float, fixed |
| --- | --- | --- | --- | --- |
| STPM00SPM | 8.9, 14.7 | 17.7, 24.3 | 19.0, 27.4 | 9.2, 16.2 |
| WTZZ00DEU | 9.3, 8.3 | 6.9, 15.7 | 8.8, 12.3 | 9.4, 7.3 |
| BAKE00CAN | 9.1, 4.6 | 17.3, 15.3 | 17.6, 16.8 | 9.6, 5.2 |

Phase residuals by signal at STPM00SPM, three frequencies, hours 2 to 6, float and fixed (rms, mm): GPS L1C 4.5 and
4.9, L2W 5.9 and 5.7, L5X 7.3 and 16.6 (G03 38.9 mm and G25 24.2 mm in the fixed run, both Block IIF); Galileo L1X
4.2 and 5.0, L5X 3.2 and 3.4, L7X 3.1 and 3.3.

**Reading.** With one fitted bias per satellite and extra signal, the integers of every tracked frequency are fixed at
three receivers outside the fit: 5 615 fix events of extra signals in the every-frequency runs, the Galileo E5b and
E5 AltBOC signals held on 98.7 to 99.8 % of their pair-epochs, and no extra-signal integer differs between the starts
at 00:00 and 01:00 (2 986 events compared). The primary integers equal those of the two-frequency control.

The position does not gain. Where the third frequency is Galileo E5b alone, the fixed position stays within 1.5 mm of
the two-frequency fix (16.2, 7.3, 5.2 against 14.7, 8.3, 4.6 mm). With GPS L5 in, the position is 4 to 13 mm worse
than the two-frequency fix. The float is already worse at two receivers, and the pinned L5 integer adds to it at two:
the L5 phase of the Block IIF satellites does not follow one bias per satellite and day (residuals of 24 to 39 mm on
G03 and G25 after the pin, 3 mm on Galileo E5b). A pinned integer turns a bias error that a float ambiguity absorbs into a range
error, so an extra signal helps only when its bias is known at the millimetre level.

**Limits.** One day, three receivers, CODE products (the network's stream has no third frequency). The extra biases
are a fit of the same day from 14 receivers, one number per satellite and signal; E6 comes from 3 to 6 receivers
(L6B at STPM00SPM from 3) and its held share says nothing about the bias being right. The admission bound of an
extra-wide lane is the wide-lane bound (`wl_max_frac`, 0.40 cycle), far wider than the accuracy a pinned extra signal
needs, and the pinned row has the hold variance of the primary signals: a weight that carries the uncertainty of the
extra signal's bias is not built. E6 at BAKE00CAN (L6C, no bias in the augmented file) never entered the resolver.
No bar was frozen.

## 14. 26 May, a start that failed, and a comparison free of the reference satellite (2026-11-14, DOY 143 to 146, demonstration)

### 14.1 DOY 146

`nw_day.py 146`, as section 12.1 (73 of 73 observe runs ended with exit 0; observers in the fit GPS L1C/L2W 53,
L1W/L2W 17, Galileo L1X/L5X 37, L1C/L5Q 33). Static, start at 00:00:

| receiver | held share (%): GPS, Galileo | first hold (min): GPS, Galileo | 00:00 against 01:00 start: events compared, different | 3D median per hour (mm): float | 3D median per hour (mm): fixed |
| --- | --- | --- | --- | --- | --- |
| STPM00SPM | 85.4, 87.5 | 32, 31 | 150, 0 | 64.7 7.4 25.6 17.3 15.2 21.2 | 64.9 6.6 28.4 16.1 13.6 18.2 |
| WTZZ00DEU | 92.1, 94.6 | 22, 20 | 48, 0 (Galileo only; the start at 01:00 failed, 14.2) | 55.5 15.7 16.0 9.0 8.3 10.1 | 22.9 14.4 11.7 7.5 7.4 9.4 |
| BAKE00CAN | 91.1, 91.2 | 21, 20 | 191, 0 | 36.4 5.8 5.0 6.0 7.7 9.7 | 29.3 3.8 3.2 6.7 8.1 9.8 |

Kinematic: held share GPS and Galileo, 3D median per hour float, fixed. STPM00SPM 40.4, 64.3 %; 70.3 26.6 40.5 23.3
26.3 36.3; 65.8 28.6 41.2 31.6 29.5 29.7. WTZZ00DEU 91.0, 94.6 %; 61.5 18.2 7.3 11.3 12.9 12.6; 18.7 14.3 8.4 9.6
12.0 11.4. BAKE00CAN 90.4, 90.1 %; 99.7 20.0 12.6 16.2 14.0 17.0; 99.7 15.5 15.2 17.9 12.1 14.3. The fixed position
is better than the float in 13 of 18 receiver-hours static and in 10 of 18 kinematic (one equal).

### 14.2 The start at 01:00 at WTZZ00DEU failed, and why

`_rap_nw_fxbgwl_146_wtzz`: position 10 to 20 cm from the reference for six hours (118.7 142.9 200.7 159.0 110.5
102.1 mm by hour) with a formal 3D sigma of 3 mm; GPS held 12.4 %, Galileo 29.6 %. From 01:00 the consumer took L1C as
the GPS signal on band 1 (L1W in every other run of this receiver), and the L1W stream `_rap_nwgw_n146` has no
corrected L1C offsets: the pair L1C/L2W was fixed on an uncorrected L1C bias and an L2W bias corrected for the other
signal group. The first fix (G13 against G12, 19 min after the start, ratio 441) and the ones after it (ratio near its
cap of 10 000) were accepted, and `hold_release_cyc` did not release the hold. Of the 45 fixing runs on corrected
streams so far (12 static from 00:00, 12 from 01:00, 12 kinematic, the 9 of section 10.1) it is the only one whose
signal pair is not the one its stream was corrected for (the signal pairs of every run were read from its `WLFLOAT`
lines).

The same start on the L1C stream (`_rap_nw_fxbgcl_146_wtzz`): GPS held 71.9 %, Galileo 81.4 %, first hold 19 min
after the start, position 49.4 15.3 5.8 5.9 9.2 8.1 mm, and its Galileo integers equal those of the start at 00:00
(3 053 satellite-epochs, 19 combinations, 0 different).

Two things follow. The two streams are a limit of the demonstration: the wide-lane part of an offset comes from the
code, so one phase series per signal cannot serve two GPS signal groups, and a product needs the code biases of the
signals corrected as well as the phase biases. And the user resolver gave no warning on signals without the integer
property: the ratio test passed, the hold stayed, and the formal sigma was 3 mm on a position 20 cm off. A user needs
the product's statement of which signals carry the integer property and must not fix any other.

### 14.3 A comparison that does not need the same reference satellite

`nw_int_compare.py` (section 7) compares fix events of the same satellite pair with the same reference satellite, so
it compared 48 events of the failed run, all Galileo, found no difference, and could not see the failure.
`nw_ab_check.py` compares the held integers of every epoch over the satellites both runs hold, whatever their
references (e(i) = N_A(i) - N_B(i) must be one constant over them, as in section 12.2). Start at 00:00 against start
at 01:00, STPM00SPM, WTZZ00DEU and BAKE00CAN, DOY 143 to 146:

| signals | satellite-epochs compared | distinct (satellite pair, integer) combinations | different |
| --- | --- | --- | --- |
| Galileo (L1X/L5X, L1C/L5Q) | 26 089 | 286 | 0 |
| GPS (L1C/L2W, L1W/L2W) | 25 687 | 340 | 0 |

The failed run has no GPS pair in it (other signals than the start at 00:00); its Galileo pairs agree.

**Reading.** Over four days the two starts hold the same integers on all 626 distinct combinations, by a comparison
that does not depend on the reference satellite. The one failed start is a mismatch between the signal the receiver
took and the signals the stream was corrected for, and it shows that the resolver alone does not detect a stream
without the integer property.

### 14.4 The number of observers behind an offset (DOY 144, nine receivers held out, diagnostic)

`nw_coverage.py`: for every pair-epoch of a held-out receiver on the corrected stream, the fraction of the
ionosphere-free N_1 float against the number of observers (same constellation and signal pair) that had the pair's
two satellites in the same 30 min bin, the smaller of the two counts. Mean |fraction| (share within 0.10), pooled
over the nine receivers:

| observers | GPS | Galileo |
| --- | --- | --- |
| 1 to 3 | 0.079 (72 %) | 0.159 (44 %) |
| 4 to 7 | 0.092 (63 %) | 0.084 (70 %) |
| 8 to 15 | 0.090 (68 %) | 0.065 (79 %) |
| 16 or more | 0.042 (91 %) | none |

The split of the receivers into signal pairs leaves most satellites with 4 to 15 observers. Galileo degrades clearly
below 4, and HARB00ZAF has 1 to 3 observers on 1 500 of its 2 444 Galileo pair-epochs (0.226, 20 % within 0.10),
which accounts for its low Galileo share. The count does not account for GPS at ASCG00SHN (0.130 to 0.174 with 4 to
15 observers) or at HARB00ZAF (0.120 with 8 to 15).

## 15. 27 May, and the five days together (2026-11-14, DOY 143 to 147, demonstration)

`nw_day.py 147` (73 of 73 observe runs ended with exit 0; observers in the fit GPS L1C/L2W 51, L1W/L2W 19, Galileo
L1X/L5X 37, L1C/L5Q 33). Static, start at 00:00:

| receiver | held share (%): GPS, Galileo | first hold (min): GPS, Galileo | 00:00 against 01:00 start: events compared, different | 3D median per hour (mm): float | 3D median per hour (mm): fixed |
| --- | --- | --- | --- | --- | --- |
| STPM00SPM | 63.3, 79.4 | 16, 16 | 112, 0 | 20.5 4.3 14.6 19.6 28.0 19.5 | 12.2 4.8 11.0 10.0 21.0 15.8 |
| WTZZ00DEU | 90.9, 91.8 | 18, 18 | 10, 0 (Galileo only; the start at 01:00 failed) | 57.3 19.4 17.3 14.9 15.1 15.2 | 46.5 10.6 17.2 14.3 14.9 15.0 |
| BAKE00CAN | 86.6, 93.7 | 26, 21 | 213, 0 | 34.6 8.3 10.2 7.8 9.1 12.7 | 27.1 5.4 7.3 7.6 9.2 13.9 |

Kinematic: held share GPS and Galileo, 3D median per hour float, fixed. STPM00SPM 66.8, 79.3 %; 25.0 26.7 51.4 25.7
19.8 29.5; 17.9 24.4 52.2 38.8 31.3 29.5. WTZZ00DEU 90.0, 90.4 %; 60.0 60.6 35.3 12.3 14.4 13.4; 62.1 40.8 12.4 14.2
14.9 13.9. BAKE00CAN 87.2, 93.7 %; 98.9 30.5 21.0 15.8 10.6 12.5; 99.9 15.4 21.5 19.3 16.4 15.2.

The start at 01:00 at WTZZ00DEU failed as on DOY 146 and for the same reason: `_rap_nw_fxbgwl_147_wtzz` took L1C/L2W
on the L1W stream (129.5 137.8 157.9 113.8 88.4 81.0 mm, GPS held 4.9 %). On the L1C stream
(`_rap_nw_fxbgcl_147_wtzz`) it holds 76.7 % GPS and 79.6 % Galileo, first hold 20 min after the start, position
22.5 10.9 8.8 15.2 15.7 14.4 mm. The canonical signal rule gives the receiver the first signal of its priority list
that reaches half of the band's satellite count, and from 01:00 on these two days that is L1C.

The five days together (STPM00SPM, WTZZ00DEU and BAKE00CAN outside the fit):

- **Held share, start at 00:00.** STPM00SPM GPS 63 to 87 %, Galileo 48 to 92 %; WTZZ00DEU 90 to 92 % and 92 to 95 %;
  BAKE00CAN 85 to 94 % and 88 to 95 %. First hold at minute 16 to 36.
- **Two starts** (`nw_ab_check.py`, every pair of runs whose signals match): Galileo 364 distinct combinations over
  31 138 satellite-epochs, GPS 391 over 29 576, none different.
- **Against CODE products** (`nw_dd_check.py`, per day 13, 16, 8, 12 and 12 combinations): 61, none different.
- **Position.** The fixed static position is better than the float in 14, 15, 12, 13 and 15 of 18 receiver-hours. The
  fixed kinematic position is better in 13, 14, 10 and 5 of 18 on DOY 143, 145, 146 and 147 (DOY 144 in section 10.2).
- **Starts at 01:00.** 13 of 15 use the signals their stream was corrected for and hold the integers of the start at
  00:00; 2 took another GPS signal and failed (14.2 and above), and both fix on the stream that matches.

The whole Debug suite on the tree of section 13: 4 411 tests in 580 suites, 4 374 passed, 37 skipped (fixture-gated),
0 failed (2 288 s).

**Reading.** On five days the integers held at three receivers outside the fit are the same from two start times and
the same as on CODE products wherever the two can be compared. Every failure found is a start on signals the stream
was not corrected for. The static position gains in about three receiver-hours of four; the kinematic position gains
little and loses on DOY 147. This card is near the size bar: further results go to a continuation card.

## 2026-11-20: closed

Closed by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); applied by the docs consolidation on branch `docs/consolidate-2026-11-20`, in the batch of the user series where the card's own text shows nothing owed. The user-engine code of this line is on main since 2026-11-17. The line is continued in ARNB.
