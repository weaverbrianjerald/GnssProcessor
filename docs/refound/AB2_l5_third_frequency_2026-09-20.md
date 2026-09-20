# AB2: GPS L5 as a third frequency (add-back 2)

**STATUS**

- **id:** AB2
- **topic:** network-vehicle
- **status:** parked
- **summary:** Second add-back on the held-wire posture, PARKED 2026-09-20 at KEEP_OFF on the field: a triple network damages the L1/L2 wire on real data (consumer hours 4 to 6 at 0.62 / 0.68 / 1.11 m against 0.10 / 0.13 / 0.11; a dual consumer on the triple wire reproduces it), the (L1, L5) IF-TD pairs are the largest destabilizer, the post-fit refloats stay doubled; the sim is neutral to better. An L5 phase-term model and a band-5 slip-tier calibration are owed.
- **source:** [NETWORK_REFOUNDING](NETWORK_REFOUNDING_2026-09-14.md) (the add-back list); [PBH](PBH_phase_bias_hold_2026-09-19.md) section 8 (the baseline pair and the L5 pre-flight note); code census of 2026-09-20
- **knobs / posture:** `processing.frequency_mode: triple` plus band-5 entries in `processing.signal_priorities.GPS`; RF-0hv working bases at main `2b12b19`
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** baseline pair `runs/_rf0hv3_{field,sim}6`; bench arms `runs/_ab2_{field,sim}6_{T3,T3n}`; exe `gnss_processor_pbh1.exe` sha256 `F1D3BDDB...`

## 1. What the census found (2026-09-20)

**A triple arm loads with two keys.** `frequency_mode` is tested only inside the
unarmed `code_bias_gf_datum` block (`GnssConfigLoadNetwork.cpp:3252`, `:3275`); no
knob armed in the bases has a frequency interlock. The delta is
`processing.frequency_mode: "triple"` and band-5 codes appended to the explicit GPS
priority list. Without the second key the arm is inert: the band latch and the
fallback walk both iterate the list (`PppModel.cpp:459-485`, `:1011-1026`), and the
bases' 72-entry GPS array holds no band-5 code.

**Datum.** Code-bias columns allocate per (satellite, signal) with no band
restriction (`MeasurementModel.cpp:1275-1282`). The IF pin pair stays (C1W, C2W).
Band 5 is closed by the per-signal GF pin at the same c T_GD
(`NetworkSbasis.cpp:903-970`), which asserts a zero inter-signal correction between
L1 and L5 and is counted as `n_sat_gf_isc_zero`; the channel's sum-to-zero row is
then suppressed (`:1170-1180`), so that assertion is the band's only code datum.
`sat_isc_state` cannot reach band 5 (its branch is guarded to band 2, `:927`). The
phase datum is per (satellite, signal) already, so anchor election and the held
basis extend without change.

**Receiver side: two censuses disagree.** One census reports the receiver code-bias
datum inert under `estimate_ifb: false`; the other reports `estimate_ifb_` force-set
true by the W6 branch (`GnssConfigLoadNetwork.cpp:3110`), and the RF-PD-fix run
counters (42 463 datum folds) agree with the second. `GPS_IFB5` exists in the key
schema and no allocation site was found. The bench reads the receiver states that
actually exist from `network_rx_states.csv`.

**Consumer.** The closed-loop consumer inherits the producer's frequency mode and
priorities and gains a third float ambiguity per arc. A missing L5 code bias is used
unbiased with a 100-fold variance; a missing phase bias is absorbed by the float
ambiguity.

**Data.** Field: all 60 stations declare band 5, 29 as C5Q/L5Q (Septentrio, Leica)
and 31 as C5X/L5X (Javad, Trimble); the consumer WTZZ00DEU is on the X side. 17 of
32 satellites transmit L5 on DOY 129 of 2024 (Block IIF: G03 G06 G08 G09 G10 G24
G25 G26 G27 G30 G32; Block III: G04 G11 G14 G18 G23 G28), so band 5 fills 40 to 67 %
of GPS satellite-epochs. The COD bias product has no GPS band-5 row and the
broadcast message is LNAV (T_GD only), so every L5 bias on the field wire is the
network's own estimate under the asserted zero inter-signal correction. Sim: the
fixture already carries C5Q/L5Q at full fill on all 32 satellites with a synthetic
constant bias and no inter-frequency clock variation, so the sim bed cannot falsify
the phase-bias hold on L5.

**The hold.** It is keyed per (satellite, signal) (`NetworkPbHold.cpp:100`), with one
scalar gap and a telemetry distribution pooled over signals (`:143`), so an L5
excursion is invisible in the run summary. No inter-frequency clock bias model
exists in the tree.

**Priors.** LV-SEED (s24.14): L5 signals with no code bias to pin them left 13 to
17 near-zero eigenvalues and withheld every epoch, measured before the per-signal
pin existed. SIMNET-SLANT-TRIPLE and the open trio-runaway item: a third band alone
bought nothing for code-bias calibration and the settled lane trio diverged, on a
lane-bearing posture before the wind-up fix. RF-0d: the per-signal pin is a real
datum whose first field consumer regressed. Slip tiers already armed
(`if_phase_td_pairs: all`, `common_band_scope`, `common_band_ref_arc`,
`mw_evidence_refloat`, `adm_screen`) change structure at three bands with no config
edit, and `cb_admission` is off, so sparse L5 columns ride the two-station gate.

## 2. Reasoning before any number

For a float consumer a third frequency adds observations and one more ambiguity
and receiver bias per arc; position accuracy should move little. Its value is for
the lane add-back that follows. The pass criterion here is therefore health and
neutrality, not gain. The one physical risk specific to this posture is the hold:
on Block IIF satellites the L5 phase carries a thermally driven variation relative
to the L1/L2 clock of order a decimetre, which the estimator can only place in the
L5 phase bias. Freezing that channel would push the variation into the consumer's
L5 residuals. Block III satellites are the control (no comparable variation), and
L1/L2 on the same satellites are the noise floor of the estimator's own wander.

## 3. AB2-0: the pre-registered config-only bench

Both beds, 6 h cold, exe `gnss_processor_pbh1.exe`.

- **T3**: the base plus `frequency_mode: "triple"` and the band-5 codes (C5Q C5X C5I,
  L5Q L5X L5I, D5Q D5X D5I, S5Q S5X S5I) appended to the GPS priority list.
- **T3n**: T3 with `network.emit_pb_hold: false` and the consumer translation at a
  zero dead band. The solve is the same as T3; the wire then shows the estimator's
  own phase bias per band, and the consumer result must equal T3's.

**Gates.** G0: both arms exit 0 with 720 consumer epochs and no withheld epoch
beyond the baseline's. G1: band 5 is live (L5 phase-bias and C5 code-bias records on
the wire, the consumer trace shows a third signal, the receiver states present are
listed). G2: `network_estimability.csv` shows no near-null direction beyond the
baseline's count after ignition. G3: T3n's `closed_loop_trace.csv` equals T3's.

**Bars.** B1 neutrality: consumer p50 of T3 within 0.02 m of the baseline pair in
hours 2 to 6 on both beds (better is reported as gain, worse is a finding that
blocks adoption). B2 the clock wire: the C records' truth grade (sim) and COD grade
(field) within 10 % of the baseline's within-arc RMS.

**The L5 phase-bias read (from T3n's wire).** Per band and per satellite block:
within-arc RMS of the de-commoned phase-bias movement and its hour-scale range, for
L1, L2 and L5, split Block IIF against Block III on the field. Reading: L5 movement
on IIF clearly above both the L1/L2 floor and the Block III L5 level is signal, and
the hold then needs a band scope (AB2-1, a small build: a per-band exclusion for the
hold with per-band telemetry); otherwise the hold stays as it is on all bands.

Nothing is adopted by this bench. Adoption of the triple posture into the working
bases is a separate decision after AB2-0 and, if needed, AB2-1.

## 4. 2026-09-20: AB2-0 result, and the attribution arm pre-registered

Arms `runs/_ab2_{field,sim}6_{T3,T3n}`, exe `gnss_processor_pbh1.exe`, all exit 0 with
720 consumer epochs; a triple arm takes about 13 minutes against 4 for dual.

**Gates.** G0 PASS. G1 PASS: the field wire carries L5Q (10 884) and L5X (12 022)
phase-bias records with the matching C5Q and C5X code-bias records, the field
consumer tracks L5X on 44 % of its satellite-epochs (2549 of 5809 cells), the sim
carries L5Q at full fill. The only receiver states are the clock and `CB:GPS_DCB`
(42 454 rows): the receiver DCB state is live, and no receiver band-5 code bias is
allocated on either side. G3 PASS: T3n's consumer table equals T3's.

**B1 neutrality: FAIL on the field, mixed on the sim.** Consumer 3D p50 by hour, m:

| arm | field | sub-dm | sim | sub-dm |
| --- | --- | --- | --- | --- |
| baseline pair (dual) | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 |
| T3 (triple) | 0.67 0.30 0.14 0.62 0.68 1.11 | 0 | 0.56 0.16 0.08 0.04 0.04 0.04 | 430 |

The field error is a ramp from hour 3 (per 30 min 1.62 0.40 0.30 0.26 0.13 0.46
0.33 0.67 0.67 0.75 1.11 1.13) carried by the vertical (hours 4 to 6: up -0.672 m
mean against -0.061, east +0.260 against +0.065). The sim is 0.06 m worse in hour 2
and 0.01 to 0.02 m better from hour 3.

**B2 the clock wire: FAIL on the field.** Within-arc RMS against COD, hours 2 to 6,
difference-median: consumer-tracked 0.0274 to 0.0308 m (+12 %), all satellites
0.0281 to 0.0420 m (+49 %). The slip tiers roughly double at three bands
(`T4_attrib` 457 to 931, `T0_band_scope` 415 to 719, `T1_if_slip` 136 to 308,
`CLK_ANCHOR` 598 to 777, `STRANSFORM` 151 to 221).

**The L5 phase-bias read (T3n wire, hold OFF).** Per band the estimator's raw
phase bias wanders by 0.5 to 1.5 m within an arc on the field on every band and
every block, so a decimetre signal cannot be read per band. The instrument that can
see it is the clock-free and ionosphere-free triple combination of the wire phase
biases, -0.2851 PB1 + 1.5457 PB2 - 1.2606 PB5. On the sim it is flat (within-arc RMS
p50 0.001 m, largest range 0.055 m): there the estimator's wander is purely
clock-shaped and ionosphere-shaped. On the field it moves by decimetres to metres
(p50 RMS 0.02 to 0.26 m, p90 to 0.82 m) on Block IIF and Block III alike, and the
two independent receiver families disagree for the same satellite: G03 0.240 (X
family) against 0.004 (Q family), G10 0.820 against 0.008, G25 1.437 against 0.021,
G24 0.251 against 1.604, with the difference as large as the larger member. One
family is always near zero, which is what three bands anchored at one station give
by construction; the other family's L5 is anchored elsewhere. The movement is
anchor-station-specific estimator error, not a common satellite signal, and no
Block IIF excess is resolvable. By the rule of section 3 the hold stays on all
bands; AB2-1 as a band scope is not warranted.

This also names the lead for the open question of why the estimator's phase bias
wanders at all ([PBH](PBH_phase_bias_hold_2026-09-19.md) section 2): the time
variation of a (satellite, signal) phase bias follows its ANCHOR station, not the
ensemble of stations that track the satellite.

**Verdict of AB2-0.** L5 is not adoptable on the field as the posture stands. The
regression is unattributed between the triple NETWORK (slip tiers, clock wire) and
L5 inside the CONSUMER (unmodelled L5 phase behaviour against a frozen product).

**AB2-0b, pre-registered: the attribution arm (config-only).** T3d = the triple
network of T3 with the consumer fed a copy of its RINEX file from which the band-5
observation types are removed, so the consumer is dual while the wire is the triple
network's. Reading, frozen: T3d within 0.03 m of the baseline pair in hours 2 to 6
means the triple wire serves a dual consumer and the loss is L5 inside the consumer;
T3d reproducing T3's loss means the triple network damages the L1/L2 wire. Both
beds.

## 5. 2026-09-20: AB2-0b result (the loss is in the NETWORK), and the slip-tier arms pre-registered

Arms `runs/_ab2_{field,sim}6_T3d`: the wire is byte-identical to T3's and the consumer
tracks two signals only. Consumer 3D p50 by hour: field 0.81 / 0.37 / 0.17 / 0.45 /
0.64 / 0.85 m (hours 4 to 6 up -0.557 m), sim 1.40 / 0.15 / 0.15 / 0.06 / 0.04 / 0.04 m.
The dual consumer on the triple wire reproduces T3's field loss, so by the frozen
reading the triple network damages the L1/L2 wire; L5 inside the consumer is not the
cause. On the sim the triple wire serves a dual consumer as well as a triple one
from hour 4, with a worse first hour (1.40 m).

**What changed on the field wire.** Clock records of T3 minus the dual baseline,
de-commoned with the difference-median and integrated, hours 2 to 6: within-arc RMS
up to 0.558 m (G22), 0.312 (G17), 0.268 (G27), 0.230 (G26), 0.180 (G29), with ranges
to 2 m, on satellites with and without L5. The level of the L5 satellites' clocks
also moves by metres (RMS 2.8 m against 1.2 m for the others), constant in time,
which is the asserted zero inter-signal correction re-realizing the clock datum of
those satellites; a float consumer absorbs a constant. The p50 of the truth grade
(0.0274 to 0.0308 m) hid the tail.

**Where it comes from.** Slip-tier events by block, dual against triple:
`T1_if_slip` 136 to 308 with 151 rows naming a band-5 signal (IIF 38 to 134, III 41
to 117, satellites without L5 unchanged at 57); `T0_band_scope` 415 to 719 with 454
rows naming band 5; `T4_attrib` 457 to 931, doubled on every block including the
satellites with no L5 (231 to 469); `CLK_ANCHOR` 598 to 777. Block III is not
quieter than Block IIF per satellite on the IF-TD tier, so this is not the Block IIF
inter-frequency clock variation. The armed slip tiers fire on the (L1, L5) pairs of
real data, their responses refloat, and the refloat traffic spills over the whole
network.

**AB2-0c, pre-registered (config-only, field bed first).** T3p = T3 with
`cycle_slip.if_phase_td_pairs: "first"` (the (L1, L2) pair only). T3q = T3p with
`cycle_slip.common_band_scope: false`. Reading, frozen: an arm whose consumer returns
to within 0.05 m of the baseline pair in hours 4 to 6 with `T4_attrib` back below
600 names the tier that must be scoped before L5 is adoptable, and the sim twin of
that arm is then run; if neither arm does, L5 is recorded KEEP_OFF on the field
with this evidence, the open item becomes a band-5 calibration of the slip tiers,
and the add-back sequence moves on.

## 6. 2026-09-20: AB2-0c result, and the verdict: L5 PARKED, KEEP_OFF on the field

Field arms `runs/_ab2_field6_{T3p,T3q}`, both exit 0, 720 consumer epochs.

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | T1_if_slip | T0_band_scope | CLK_ANCHOR |
| --- | --- | --- | --- | --- | --- | --- |
| baseline pair (dual) | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 457 | 136 | 415 | 598 |
| T3 (triple) | 0.67 0.30 0.14 0.62 0.68 1.11 | 0 | 931 | 308 | 719 | 777 |
| T3p (IF-TD on the (L1, L2) pair only) | 0.69 0.31 0.14 0.15 0.17 0.30 | 9 | 947 | 164 | 719 | 1053 |
| T3q (T3p and band scope off) | 0.69 0.31 0.14 0.26 0.24 0.32 | 0 | 827 | 164 | 0 | 882 |

Restricting the IF-TD tier to the (L1, L2) pair removes most of the loss (hours 4
to 6 from 0.62 / 0.68 / 1.11 to 0.15 / 0.17 / 0.30 m), so the (L1, L5) IF-TD pairs
are the largest single destabilizer. Neither arm meets the frozen reading: hour 6
stays at 0.30 m against 0.11, hour 2 at 0.31 against 0.17, and the `T4_attrib`
refloats stay doubled (947 and 827 against 457) with more clock-anchor decouples
than T3. Turning the band-scope tier off does not help. The remaining stress is in
the post-fit attribution tier, which says the L5 phase rows themselves carry model
error at the stations on real data; scoping detectors cannot remove that.

**Verdict.** On the field, GPS L5 in the NETWORK damages the L1/L2 wire and is
KEEP_OFF on this posture. On the sim it is neutral to better (0.04 m settled
against 0.05 to 0.06 m), which the sim earns by having no receiver band-5 bias, no
inter-frequency clock variation and full L5 coverage. Nothing is adopted; the
working bases stay dual.

**Owed before L5 can return** (one build card, not config): a model for the
L5-specific phase term (a per-satellite inter-frequency clock state common to the
stations, and the receiver band-5 code bias `GPS_IFB5` that the key schema names and
nothing allocates), a band-5 calibration of the IF-TD and post-fit attribution
tiers, per-band telemetry on the hold, and a sim fixture that carries receiver
band-5 biases and an inter-frequency clock variation, so that the sim bed can
falsify the field behaviour. The inter-signal correction asserted at zero moves the
L5 satellites' clock level by metres; a float consumer absorbs it, an integer
consumer will not.
