# AB2: GPS L5 as a third frequency (add-back 2)

**STATUS**

- **id:** AB2
- **topic:** network-vehicle
- **status:** open
- **summary:** Re-opened after a field KEEP_OFF: the L5 field loss is a gap re-entry hole, not an L5 model gap; `gap_reentry_relevel` ARMED in the working bases 2026-09-20; L5 not yet adopted.
- **source:** [NETWORK_REFOUNDING](NETWORK_REFOUNDING_2026-09-14.md) (the add-back list); [PBH](PBH_phase_bias_hold_2026-09-19.md) section 8 (the baseline pair and the L5 pre-flight note); code census of 2026-09-20
- **knobs / posture:** `processing.frequency_mode: triple` plus band-5 entries in `processing.signal_priorities.GPS`; RF-0hv working bases at main `2b12b19`; `network.cycle_slip.gap_reentry_relevel` armed in `config/refound/{field,sim}_gps_dual_rf0hv.json` (section 14)
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** baseline pair `runs/_rf0hv3_{field,sim}6`; bench arms `runs/_ab2_{field,sim}6_{T3,T3n}`; exe `gnss_processor_pbh1.exe` sha256 `F1D3BDDB...`; new baseline pair `runs/_rf0hv4_{field,sim}6`; 12 h arms `runs/_rap_d12_{off,on}`

**Outcome.** Question: can GPS L5 join as a third frequency? The first verdict (KEEP_OFF, field hour 6 at 1.11 m) is superseded from section 7: the loss traces to an unflagged 301-cycle L1C slip at a BSHM00ISR return epoch, an L1/L2 loss of lock the re-entry bookkeeping misses while band 5 tracks. Emulating these dropouts, L5 gives 311 epochs below a decimetre against 167, narrowly missing the frozen bar in hours 3 and 4. [L5P](L5P_l5_phase_posture_2026-09-21.md) section 4 counts the hole as one of three network defects; L5 is sequenced, not adopted.

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

## 7. 2026-09-20: AB2 re-opened: three reads of the existing arms, and the station-scope arms pre-registered

Section 6 parked the card on an inference that was not measured: that the L5 phase
rows carry model error at the stations in general. Three reads of the arms already on
disk test it. No new run is used in this part.

**Read 1: the sim bed never exercises the failing tiers.** `network_events.csv`: the
sim dual and the sim triple arm both hold zero `if_slip` detections and zero
`T4_attrib` events. The sim's `T1_if_slip` growth (59 to 177) is the reference-satellite
reset row written for three pairs instead of one (59 times 3), all `diagnostic`. The
sim's neutrality therefore certifies the three-band algebra, datum and wire on data
that match the model, and says nothing about the field mechanism. The two-bed
comparison isolates a fault only after the sim fixture is given the field ingredient
that produces it.

**Read 2: the L5 rows fit at the typical station.** `network_pf_rows.csv` of field T3,
hours 2 to 6, within-arc RMS of the post-fit residual per station. Station median L5
phase 0.0044 m (L5Q 0.0043, L5X 0.0046), the level of L2. Nine stations sit above
0.010 m: KIT300UZB 0.230, BSHM00ISR 0.061, GAMB00PYF 0.050, ULAB00MNG 0.035,
VOIM00MDG 0.032, JPLM00USA 0.023, BOGT00COL 0.022, YKRO00CIV 0.014, STHL00GBR
0.011. The C5 code rows carry what the datum cannot place: per-satellite mean with a
standard deviation of 1.03 m (C5Q) and 0.83 m (C5X), maximum 2.3 m, which is the
inter-signal correction asserted at zero; per-station mean 0.45 m across the X
family; BSHM00ISR at -11.7 m with 35.7 m within-arc.

**Read 3: what the other stations see is mostly the spill.** The median station's
L1 phase within-arc RMS goes from 0.0073 m (dual) to 0.0131 m (triple), L2 from
0.0044 to 0.0068 m, with L5 at 0.0044 m. In the dual solve the L1 to L2 ratio (1.66)
is exactly that of a range-like misfit after the free slant ionosphere is fitted
(0.287 against 0.174 of its size): a misfit of 0.025 m. In the triple solve a
range-like misfit lands on (L1, L2, L5) as (0.359, -0.056, -0.149) of its size, and an
inconsistency of L5 against the other two bands, local to a line of sight, as
(-0.259, -0.426, 0.536). The L1 growth (0.011 m in quadrature) is therefore mostly
range-like, a misfit of about 0.035 m, consistent with damaged satellite clocks seen
from every station. The L2 growth (0.005 m in quadrature) cannot be range-like: it
indicates an inter-band inconsistency of the order of 0.01 m at the typical station,
which a dual solve cannot see. `T4_attrib` by station:
BSHM00ISR 109 to 283 (37 % of the growth), all others 348 to 648. Band-5 `if_slip`
detections: 121 on 29 stations, 82 of them on six (MAC100AUS 25, GAMB00PYF 14,
BSHM00ISR 12, LMMF00MTQ 11, YKRO00CIV 10, BOGT00COL 10); 96 of the 121 exceed their
threshold by more than half, so they are not a marginal-threshold population.

**Reading of the three.** The field damage is consistent with a handful of stations
whose band-5 data (or all of whose code) are bad, converted by the armed tiers into
refloats that damage the satellite clocks for every station. A mechanism for the
asymmetry between two and three bands: with two bands and a free slant ionosphere a
line of sight has no inter-band redundancy, so any phase discrepancy between the
bands is absorbed silently by the ionosphere state; the third band over-determines
it, and whatever does not scale as the inverse square of the frequency becomes
post-fit stress. The list is dominated by low-latitude and high-latitude sites; a
scintillation reading is a hypothesis and is not tested here.

**AB2-1, pre-registered (config and RINEX only, field bed).** Station rule, applied to
T3's own traces: within-arc L5 post-fit phase RMS above 0.010 m in hours 2 to 6, or
ten or more band-5 `if_slip` detections. It selects 11 of 59 stations: the nine of
read 2 plus MAC100AUS and LMMF00MTQ.

- **T3s**: T3 with the GPS band-5 observation types removed from the RINEX files of
  those 11 stations. They stay in the network as dual stations; the other 48 stay
  triple; the consumer is unchanged (triple, WTZZ00DEU).
- **T3sp**: T3s with `cycle_slip.if_phase_td_pairs: "first"`.

Reading, frozen. An arm whose consumer p50 is within 0.03 m of the baseline pair in
each of hours 3 to 6, with `T4_attrib` below 600 and the clock wire's within-arc RMS
against COD within 10 % of the baseline's (0.0274 m consumer-tracked, 0.0281 m all
satellites), means the field damage is station-local: L5 is adoptable behind a
per-station band-5 health screen, the build owed is that screen, and the sim
ingredient owed is a station-local band-5 fault so that the sim bed can falsify the
screen. Neither arm meeting it means the loss is network-wide, and the sim injection
ladder is run next, one ingredient per arm on the sim's T3: a receiver band-5 code
bias per station, the two-family split of the band-5 signal, partial L5 coverage (17
of 32 satellites), a satellite inter-signal correction. A partial recovery is
reported as the fraction of the loss that is station-local, and both follow-ups are
then owed.

## 8. 2026-09-20: AB2-1 result (the consumer loss is station-local), and the two-bed one-change batch pre-registered

Arms `runs/_ab2_field6_{T3s,T3sp}`, exe `gnss_processor_pbh1.exe`, exit 0, 720 consumer
epochs, about 5 minutes each. The 11 stripped files were verified record by record
against the rule (27 650 GPS records of one station, 0 mismatches).

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | if_slip (band 5) | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| baseline pair (dual) | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 457 | 117 (0) | 0.0274 / 0.0281 |
| T3 (triple) | 0.67 0.30 0.14 0.62 0.68 1.11 | 0 | 931 | 259 (121) | 0.0308 / 0.0420 |
| T3p (T3, IF-TD on the first pair) | 0.69 0.31 0.14 0.15 0.17 0.30 | 9 | 947 | 145 (0) | not graded |
| **T3s** (band 5 removed at 11 stations) | 0.55 0.20 0.09 0.09 0.14 0.15 | **204** | 516 | 151 (24) | **0.0218** / 0.0361 |
| T3sp (T3s, IF-TD on the first pair) | 0.56 0.21 0.09 0.09 0.14 0.32 | 165 | 557 | 128 (0) | 0.0287 / 0.0365 |

**Reading, by the frozen rule: a partial recovery that is nearly complete.** T3s is
within 0.03 m of the baseline pair in hours 3 to 5 and misses hour 6 by 0.01 m (0.15
against 0.11 m); `T4_attrib` is below 600; the clock wire on the consumer's
satellites is 20 % BETTER than the dual baseline, and on all satellites 28 % worse
(T3 was 49 % worse). Of the consumer's loss in hours 4 to 6 (0.52 / 0.55 / 1.00 m)
between 96 and 100 % is station-local. 48 triple stations with 11 dual ones serve
the consumer as well as the dual network and give it more epochs below a decimetre
than any arm so far (204 against 167). The section 6 verdict (an L5 phase-term model
is owed before L5 can return) is superseded: what is owed is a per-station band-5
health screen, and an account of the rest of the all-satellite clock loss.

**The IF-TD pair scope is a wrong turn.** Both arms that restrict the IF-TD tier to
the (L1, L2) pair lose hour 6 (0.30 and 0.32 m) where the matching arm with all
pairs does not. The band-5 pairs detect real events; the scope removes the
detection, not the event. `if_phase_td_pairs` stays at `all`.

**AB2-2, pre-registered: one change per arm, both beds, one batch.** Tool:
`scripts/rapid_arm.py` (arm factory with RINEX ops, parallel runner, scorecard;
`selftest` 11/11). Field arms subtract from T3; sim arms add to the sim's T3. Run
dirs `runs/_rap_<tag>`.

| tag | bed | the one change | question |
| --- | --- | --- | --- |
| `fA_bshm` | field | band 5 removed at BSHM00ISR only | how much is the gross-code station |
| `fB_kit3` | field | band 5 removed at KIT300UZB only | how much is the 0.23 m station |
| `fD_rest9` | field | band 5 removed at the other nine | how much is the rest |
| `fE_phase` | field | L5 phase removed at the 11 (C5 code kept) | is it the phase |
| `fF_code` | field | C5 code removed at the 11 (L5 phase kept) | is it the code |
| `sA_rxb5` | sim | constant C5 code bias per station, sigma 3 m, consumer too | the receiver band-5 code bias that no state takes |
| `sB_isc` | sim | constant C5 code bias per satellite, sigma 2 m | the inter-signal correction asserted at zero |
| `sC_fam` | sim | band 5 relabelled Q to X at every second station | the two-family split of the L5 columns |
| `sD_cov` | sim | band 5 removed on the 15 satellites without L5 that day | partial coverage |
| `sE_slip` | sim | L5-only integer slips at 5 stations, 2 per arc-hour | a station-local band-5 slip fault |
| `sF_drift` | sim | L5 phase drift 0.05 m, 1.5 h period, per line of sight, at 5 stations | a station-local band-5 drift fault |

Reading, frozen. Field: an arm "recovers" when its consumer p50 is within 0.03 m of
the baseline pair in hours 3 to 6 with `T4_attrib` below 600; the smallest recovering
subtraction names the fault. Sim: an arm "reproduces" when its consumer p50 in any of
hours 4 to 6 is more than 0.05 m above the sim's T3 (0.04 m), or its `T4_attrib` or
`if_slip` count leaves zero by more than 50 events. A sim arm that reproduces gives
the sim bed its first L5 fault with truth, and the health screen is then developed
against it. Sim ingredients that do not reproduce are recorded as harmless to this
posture at the injected size.

## 9. 2026-09-20: AB2-2 field wave, and the second field batch pre-registered

Field arms of section 8 (`runs/_rap_<tag>`, six arms concurrent, about 8 minutes each):

| arm | the one change from T3 | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | if_slip (band 5) |
| --- | --- | --- | --- | --- | --- |
| T3 | none | 0.67 0.30 0.14 0.62 0.68 1.11 | 0 | 931 | 259 (121) |
| `fA_bshm` | band 5 removed at BSHM00ISR only | 0.53 0.20 0.09 0.20 0.20 0.13 | 168 | 574 | 246 (109) |
| `fB_kit3` | band 5 removed at KIT300UZB only | 0.66 0.29 0.14 0.62 0.71 1.22 | 0 | 933 | 254 (116) |
| `fD_rest9` | band 5 removed at the other nine | 0.69 0.31 0.15 0.52 0.60 0.96 | 0 | 905 | 167 (39) |
| `fE_phase` | L5 phase removed at the 11, C5 code kept | 0.63 0.28 0.13 0.19 0.19 0.36 | 0 | 651 | 151 (24) |
| `fF_code` | C5 code removed at the 11, L5 phase kept | 0.40 0.09 0.09 2.28 2.05 1.42 | 175 | 914 | 259 (121) |
| T3s | band 5 removed at all 11 | 0.55 0.20 0.09 0.09 0.14 0.15 | 204 | 516 | 151 (24) |

**One station carries the loss.** Removing band 5 at BSHM00ISR alone takes hours 4 to 6
from 0.62 / 0.68 / 1.11 m to 0.20 / 0.20 / 0.13 m and `T4_attrib` from 931 to 574. The
station with the largest L5 post-fit misfit (KIT300UZB, 0.23 m) changes nothing on
its own, and neither do the other nine while BSHM00ISR keeps its band 5; with
BSHM00ISR out they are worth the last 0.10 m of hours 4 and 5. It is the phase that
does it (`fE_phase` recovers most of the loss). A band with phase and no code
(`fF_code`) is not a posture: it is the best arm of the day through hour 3 and
diverges after.

**The raw data at BSHM00ISR are clean.** Estimator-free read of the RINEX files
(`scripts/ab2_l5_raw_read.py`, selftest 25/25, outputs `runs/_ab2_l5raw_20260920/`;
the combination 0.22618 PHI1 - 1.22618 PHI2 + PHI5 in metres cancels geometry,
clocks, troposphere and the first-order ionosphere; re-read independently at three
stations, agreeing). BSHM00ISR, KIT300UZB and JPLM00USA hold zero L5-lattice steps in
six hours with 30 s noise of 3.8 to 4.5 mm (control median 4.6 mm), their three
codes agree with each other to 0.3 to 0.5 m, the common-mode code-minus-phase rate at
BSHM00ISR equals that of a healthy station with the same receiver type, and the epoch
tags are on the grid. These stations are sick in the ESTIMATOR, in the dual solve
already: L1 post-fit within-arc RMS of 0.247 m (BSHM00ISR), 0.171 (GAMB00PYF), 0.090
(KIT300UZB), 0.089 (JPLM00USA), 0.047 (PARK00AUS) against a median of 0.0073 m,
range-like in its band ratio. A third band multiplies the damage paths of a station
that is already sick; it is not a band-5 fault. L5-only slips are real and belong to
a receiver family, not to the suspects: 57 L5-lattice steps of one to three cycles
in the 23 stations read, 56 of them on L5X receivers and 5 flagged by the receiver;
38 at suspects (ULAB00MNG 12, YKRO00CIV 8, VOIM00MDG 5, LMMF00MTQ 4, STHL00GBR 4)
and 19 at four control stations that are healthy in the solve (SGOC00LKA 6,
GCGO00USA 5, MBAR00UGA 4, POTS00DEU 4). That is what the band-5 IF-TD pairs are
catching, and slips alone do not make a station a suspect. The satellite-common term is visible and
small: Block IIF range 0.032 m median and 0.112 m maximum (G25), Block III 0.008 m;
it is the inter-band inconsistency of the order of 0.01 m read in section 7.

**Second field batch, pre-registered** (one change each from T3; same reading as
section 8, and `fG_nobshm` is read against the dual diagnostic without that station,
`runs/_diag_field6_noBSHM`: 1.03 0.29 0.19 0.18 0.16 0.14 m, `T4_attrib` 252):

| tag | the one change | question |
| --- | --- | --- |
| `fG_nobshm` | BSHM00ISR dropped from the roster, L5 at all 58 | is L5 adoptable once the sick station is out |
| `fH_nobshm_s10` | the same, and band 5 removed at the other ten | what the other ten are worth without it |
| `fJ_bshm_slip5` | band 5 removed at BSHM00ISR and the five slip stations | is the last 0.10 m the real L5 slips |
| `fK_bshm_sick4` | band 5 removed at BSHM00ISR, KIT300UZB, JPLM00USA, GAMB00PYF, MAC100AUS | or the chronically sick stations |

## 10. 2026-09-20: BSHM00ISR is not a gross-code station: the estimator diverges there at one epoch; the trigger arms pre-registered

Read of the dual baseline `runs/_rf0hv3_field6` (no new run). The post-fit code rows
of BSHM00ISR are healthy for two and a half hours and gross from then on, RMS by half
hour: 2.1 0.3 0.3 0.3 0.2 **40.6** 14.7 31.9 35.7 5.7 6.0 5.8 m (all other stations
0.6 to 0.9 m). The raw data are clean through all six hours (section 9, and the raw
C2W minus C1W of the station is 1.7 m median, the level of a healthy station with the
same receiver). At TOW 268860 (02:41:00):

- `network_rx_states.csv`: the station's `CB:GPS_DCB` goes from +41.238 m to -2.800 m
  in one epoch and is carried from there with a shrinking sigma (0.116 to 0.050 m).
- `network_events.csv`, same epoch: `PRIORRES family=rxcb label=RESPONDED
  mech=innov_withhold folded=0 e=4.403838e+01`, which is exactly +41.238 minus -2.800.
  The carried receiver code bias was withheld for the epoch and the state fell to what
  the datum alone gives it.
- Same epoch and station: `T0_gap arc_resume G14 ... L1C shifted_bridged ...
  dGF=-57.0436` together with `L2L shifted_fresh`: after a three-epoch phase dropout
  the band-2 signal of the arc came back as L2L in place of L2W (the fallback walk of
  the priority list), and three arcs were refloated by `T4_attrib`.

From that epoch the station's code bias is 44 m from the level its ionosphere states
and ambiguities carry, which is 68 m of L1 slant ionosphere: the "gross slant
ionosphere of BSHM00ISR" of AB1 section 4 (median 57 m) and the "gross code" of RF-0
are this one event, not the data. With two bands the free slant ionosphere absorbs
most of it; with three bands a code-bias error does not scale as the inverse square
of the frequency and cannot be absorbed, which is the L5 loss. BSHM00ISR holds 12 of
the 26 arc resumes of the run that name a non-primary band-2 signal. The receiver
code-bias gate is an open item already ("frame-blind"); this is a field instance with
its trigger, its epoch and its price (2.6 times on the dual clock wire, all of the L5
loss).

**Trigger arms, pre-registered (RINEX only).** `dA_bshm_noalt` = the dual baseline with
the L1L and L2L observation types removed from the RINEX file of BSHM00ISR, so that no
fallback signal exists there; `fL_bshm_noalt` = the same on T3. Reading, frozen: the
trigger is confirmed when the station's `CB:GPS_DCB` stays within 1 m of its hour-2
value through hour 6 and its code rows stay below 2 m RMS in every half hour; the
arms are then graded like any other (consumer, `T4_attrib`, clock wire). A station
that still diverges names a second trigger, and the event epoch is read again.

**The hole is the gap re-entry, and its response exists unarmed.** The `arc_resume`
row above reads `shifted_bridged ... pl=1;dd=0;slip=0`: the L1C arc returned after
three missed epochs with its raw level moved (MW by 259 cycles, GF by 57 m), nothing
marked it slipped, and the carried value was folded across the break. No detector
covers a return epoch (the MW tier needs an adjacent predecessor, the IF-TD tier
three consecutive epochs). The gap instrument counts 131 resumptions in the dual
baseline, 88 of them shifted, and 41 arcs shifted AND bridged (BSHM00ISR 9 with a
largest GF move of 277 m, BOGT00COL 8, GCGO00USA 5, SGOC00LKA 4, GAMB00PYF 3): the
"shifted-and-bridged is about zero, so defer the response" ruling of the slip-QC
card G does not hold on this posture. The response is built, default OFF, and legal
here (phase-leveling lineage with `relevel_free_epoch` on):
`network.cycle_slip.gap_reentry_relevel`. Standing invariant 1, open on the working
bases.

**Response arms, pre-registered (config only).** `dC_gapre` = the dual baseline with
`network.cycle_slip.gap_reentry_relevel: true`; `fM_gapre` = the same on T3. Reading,
frozen: the BSHM00ISR criteria of the trigger arms above; the run-end card-G line
must show the response driving (re-entry refloats above zero, invariant 4); then the
consumer, `T4_attrib` and the clock wire against the dual baseline (`dC_gapre`) and
against T3s (`fM_gapre`). A `dC_gapre` that is better than the dual baseline on the
clock wire is a posture fix in its own right; an `fM_gapre` that matches T3s with all
59 stations on three bands makes L5 adoptable without a station screen.

## 11. 2026-09-20: the sim wave, the second field batch, the trigger and response arms; the hole is per SIGNAL

**Sim wave (section 8): no L5 ingredient reproduces.** Sim T3 is 0.56 0.16 0.08 0.04
0.04 0.04 m, 430 epochs below a decimetre, `T4_attrib` 0, `if_slip` 0.

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | if_slip (band 5) | note |
| --- | --- | --- | --- | --- | --- |
| `sA_rxb5` | 0.56 0.16 0.08 0.04 0.04 0.04 | 429 | 0 | 0 | the injected constant (6.253 m at S01000SIM) leaves the C5Q post-fit mean at +0.303 m, as in T3 |
| `sB_isc` | 1.20 0.14 0.05 0.05 0.06 0.05 | 464 | 0 | 0 | a slower first hour only |
| `sC_fam` | 1.06 0.23 0.07 0.06 0.07 0.05 | 442 | 1 | 0 | a slower ignition, `STRANSFORM` 145 to 188 |
| `sD_cov` | 0.73 0.06 0.05 0.04 0.03 0.04 | 568 | 0 | 0 | better |
| `sE_slip` | 0.52 0.14 0.07 0.04 0.03 0.04 | 452 | 13 | 1635 (1635) | every injected class is detected and scoped (`T0_mw_detect` 531, `T0_band_scope` 794); the consumer does not move |
| `sF_drift` | 0.56 0.17 0.08 0.04 0.03 0.04 | 435 | 5 | 0 | harmless |

Correction of sections 1 and 6: a receiver band-5 code bias IS estimated. A constant
of metres per station on C5 is absorbed to 0.1 mm (`sA_rxb5`); the state is
`GPS_IFB5`, allocated per station on a three-band lineage and not carried by the
receiver code-bias carry (`NetworkRunContext.cpp:3039`), and it is not written to
`network_rx_states.csv`, which is why the AB2-0 bench did not see it. It is removed
from the list of owed items.

**Second field batch (section 9).**

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib |
| --- | --- | --- | --- |
| dual without BSHM00ISR (the AB1 diagnostic) | 1.03 0.29 0.19 0.18 0.16 0.14 | 33 | 252 |
| `fG_nobshm` (L5 at all 58) | 0.62 0.17 0.13 0.15 0.14 0.26 | 3 | 397 |
| `fH_nobshm_s10` | 0.34 0.20 0.14 0.15 0.14 0.22 | 5 | 335 |
| `fJ_bshm_slip5` | 0.63 0.23 0.09 0.09 0.10 0.21 | 219 | 653 |
| `fK_bshm_sick4` | 0.55 0.21 0.08 **0.05 0.07** 0.18 | **350** | 590 |

Without the sick station the triple network is better than the dual one on the same
roster in hours 1 to 5. With band 5 off at BSHM00ISR and the four chronically sick
stations the consumer reaches 0.05 and 0.07 m in hours 4 and 5 and 350 epochs below
a decimetre, the best field result of the program. Every three-band arm loses some
of hour 6 (0.13 to 0.26 m against 0.11 m); late re-entries of the kind described
below are the candidate.

**Trigger arms: the fallback signal is not the trigger.** `dA_bshm_noalt` has the
consumer table of the dual baseline and `fL_bshm_noalt` that of T3, with event
counts within 3; BSHM00ISR diverges at the same epoch. The raw data show the trigger: across the three-epoch dropout C1C minus
L1C of G14 jumps by +57.3 m (301 cycles on L1C) and C2W minus L2W by -4.5 m, while
L2L and L5Q track through; the receiver flags L2W and not L1C. It is an ordinary
unflagged slip at a return epoch.

**Response arms.**

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | CLK_ANCHOR | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| dual baseline | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 457 | 598 | 0.0274 / 0.0281 |
| **`dC_gapre`** (dual, response armed) | 0.53 **0.11** 0.09 0.10 0.14 0.12 | 203 | **228** | **424** | **0.0107 / 0.0173** |
| T3 | 0.67 0.30 0.14 0.62 0.68 1.11 | 0 | 931 | 777 | 0.0308 / 0.0420 |
| `fM_gapre` (triple, response armed) | 0.67 0.30 0.15 0.30 0.35 0.52 | 0 | 716 | 607 | 0.0269 / 0.0340 |

On the dual posture the response halves the post-fit refloats, removes every
shifted-and-bridged arc (card-G line: 0 against 41) and brings the clock wire to
0.0107 m on the consumer's satellites, which is the whole gain of removing
BSHM00ISR (0.0106 m) without removing it; hour 2 of the consumer goes from 0.17 to
0.11 m. It is a posture fix in its own right. It does not rescue the triple arm, and
it does not keep BSHM00ISR healthy on either: the first re-entry (TOW 268860) is
refloated on the dual arm, the station diverges at TOW 269010 instead, and on the
triple arm no `reentry_relevel` row exists at all.

**Why: the re-entry bookkeeping is per satellite, the fault is per signal.**
`NetworkRawObservables.cpp:53-65` builds one raw row per (station, satellite) from
whichever phase signals with a matching code are present, lowest band first;
`:318-321` defines a resumption as that ROW being absent for at least one epoch. A
dropout of the primary-pair phases during which anything else keeps the row alive is
not a resumption: band 5 (a civil signal that tracks through a semi-codeless loss of
lock) or a fallback signal such as L2L. The second divergence of the dual arm is
such a case: at TOW 268980 the primary phases of G14 are missing for one epoch
while L2L tracks, and no resumption row is written at TOW 269010. At the return epoch
any adjacent-epoch step is formed across a pair change (`:224-230`), and in the field
instance nothing marks the arc slipped. The slipped ambiguities are folded.
A third band makes this the rule instead of the exception: every satellite with L5
stays present through every L1/L2 phase dropout, so the only response that covers a
return epoch never fires for it. That is the mechanism by which adding L5 damages
the L1/L2 wire, and it is a defect of the two-band posture too.

**Emulation arms, pre-registered (RINEX only, no build).** RINEX op `holes` of
`scripts/rapid_arm.py` (selftest 13/13): a satellite's whole record is deleted at an
epoch where its primary L1 or L2 phase is missing inside an arc, so that every
primary-pair phase dropout becomes a full tracking gap which the existing
per-satellite response can see. It removes 12 923 of 464 930 GPS records of the
window (2.8 %; station median 1.9 %, maximum 9.3 %). `dD_holes_gapre` = `dC_gapre`
with the op; `fN_holes_gapre` = `fM_gapre` with the op. Reading, frozen: the
BSHM00ISR criteria of section 10; `fN_holes_gapre` within 0.03 m of T3s in each of
hours 3 to 6 with `T4_attrib` below 600, all 59 stations on three bands, means a
per-signal re-entry response is the build that makes L5 adoptable, and its size is
known before it is written; `dD_holes_gapre` is read against `dC_gapre`.

## 12. 2026-09-20: the emulation arms: L5 on all 59 stations beats the dual baseline once the re-entry is seen

| arm | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | CLK_ANCHOR | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| dual baseline | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 457 | 598 | 0.0274 / 0.0281 |
| `dC_gapre` (dual, response) | 0.53 0.11 0.09 0.10 0.14 0.12 | 203 | 228 | 424 | 0.0107 / 0.0173 |
| **`dD_holes_gapre`** (dual, response, holes) | 0.47 0.15 0.10 0.12 0.13 0.09 | 155 | **117** | **271** | **0.0081 / 0.0111** |
| T3s (band 5 off at 11 stations) | 0.55 0.20 0.09 0.09 0.14 0.15 | 204 | 516 | 742 | 0.0218 / 0.0361 |
| `fM_gapre` (triple, response) | 0.67 0.30 0.15 0.30 0.35 0.52 | 0 | 716 | 607 | 0.0269 / 0.0340 |
| **`fN_holes_gapre`** (triple, response, holes; all 59 on three bands) | **0.31 0.07** 0.14 0.13 0.11 **0.07** | **311** | 327 | 454 | **0.0126** / 0.0270 |

**Reading.** Against T3s, `fN_holes_gapre` is +0.05, +0.04, -0.03 and -0.08 m in hours
3 to 6 with `T4_attrib` at 327: by the letter of the frozen rule a near miss in hours 3
and 4 (by 0.02 and 0.01 m) and a clear win in hours 5 and 6. Against the dual baseline
it is within 0.03 m in every hour from 3, better in hours 1, 2 and 6, with 311 epochs
below a decimetre against 167, and the clock wire on the consumer's satellites is 2.2
times better (0.0126 against 0.0274 m) and equal on all satellites. It is the first
three-band network with L5 at every station that serves the consumer better than the
dual one. The question the arm was built for is answered: a re-entry response that
sees a primary-pair phase dropout is the build that makes L5 adoptable. On the dual
posture the same emulation takes the clock wire to 0.0081 / 0.0111 m (3.4 and 2.5
times better than the baseline) and `T4_attrib` to 117. The emulation pays 2.8 % of
the GPS records, code and third-band data included, which a per-signal response
keeps, so these are lower bounds.

**BSHM00ISR.** `CB:GPS_DCB` stays within 0.3 m (dual +0.90, triple +1.20 m) and the
station's `T4_attrib` falls to 15 and 40; the code rows are disturbed between hours 2
and 4.5 (half-hour RMS up to 13 m) and healthy again afterwards (0.2 to 0.7 m dual),
where they used to stay at 6 to 40 m. The criterion of 2 m in every half hour is not
met: a second, smaller fault at that station remains unread.

**Sim neutrality of the response.** `sG_gapre_dual` and `sH_gapre_tri` have the
consumer tables, the event counts and the clock grades (0.0030 / 0.0040 m) of their
bases; the 74 shifted resumptions of the sim are fresh already.

**Recommendation, for the author's decision.** Arm
`network.cycle_slip.gap_reentry_relevel` in both working bases now: config only,
legal on the posture, neutral on the sim, and on the field it halves the post-fit
refloats and takes the clock wire on the consumer's satellites from 0.0274 to 0.0107
m. It closes standing invariant 1 at the satellite level. The per-signal level is the
build card [GRS](GRS_gap_reentry_per_signal_2026-09-20.md).

**The sim gets the field's fault, pre-registered.** RINEX op `dropslip` of
`scripts/rapid_arm.py` (selftest 15/15): L1C, L2W and C2W are blanked for three
epochs and both phases return with their own integer step of 1 to 300 cycles, while
C1C, C5Q and L5Q track through; 81 events at 10 of the 60 sim stations in six hours.
Four arms: `sI_ds_dual_off` and `sJ_ds_dual_on` (the dual sim base without and with
the response), `sK_ds_tri_off` and `sL_ds_tri_on` (the same on the sim's T3).
Expectation, frozen: the dual arm is damaged without the response and healthy with
it; both triple arms are damaged, because band 5 keeps the satellite present and the
response never fires. If so the sim bed reproduces the field mechanism with truth,
and `sL_ds_tri_on` with the GRS knob becomes the acceptance test of the build. An arm
that is not damaged is reported as such: the injection is then too weak or the
mechanism is not the one read from the field.

## 13. 2026-09-20: the sim bed carries the field's fault, with truth

Arms of section 12 plus three controls, exe `gnss_processor_pbh1.exe`, all exit 0.
The injection is the same in every arm (81 losses of lock of three epochs at 10 of
the 60 sim stations, each phase returning with its own step of 1 to 300 cycles);
"partial" blanks L1C, L2W and C2W while C5Q and L5Q track through, "full" blanks the
band-5 pair as well, so that the satellite's raw row disappears.

| arm | loss of lock | response | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | CLK_ANCHOR | re-entries: bridged / refloated |
| --- | --- | --- | --- | --- | --- | --- | --- |
| sim dual base | none | off | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 | 0 | 127 | 2 / 0 |
| `sO_dsfull_dual_off` | full | off | 0.77 0.23 0.18 0.26 0.45 0.45 | 14 | 2416 | 2669 | **141** / 0 |
| **`sM_dsfull_dual_on`** | full | ON | 0.59 0.11 0.10 0.06 0.05 0.05 | 439 | 64 | 127 | 0 / **161** |
| `sI_ds_dual_off` | partial | off | 0.81 0.72 0.12 0.14 0.17 0.20 | 108 | 1955 | 2437 | 2 / 0 |
| `sJ_ds_dual_on` | partial | ON | 0.81 0.72 0.11 0.10 0.17 0.16 | 178 | 1940 | 2416 | 0 / 20 |
| sim T3 (triple) | none | off | 0.56 0.16 0.08 0.04 0.04 0.04 | 430 | 0 | 133 | |
| `sK_ds_tri_off`, `sL_ds_tri_on` | partial | off, ON | 0.50 0.23 0.28 0.24 0.22 0.22 (both) | 1 | 2944 | 2893 | |
| **`sN_dsfull_tri_on`** | full | ON | 0.58 0.14 0.05 0.04 0.05 0.04 | 469 | 193 | 139 | 0 / 160 |

**The same slips are harmless when the re-entry is seen and ruinous when it is not.**
With a full loss of lock the satellite's row disappears, the instrument counts 141
shifted-and-bridged arcs, and the response turns a run that ends at 0.45 m with 2416
post-fit refloats into one at the level of the undisturbed base (0.05 m, 64, the
clock-anchor decouples back at 127). With a partial loss of lock no re-entry row is
written at all, on two bands or three, and the response changes nothing (the two
triple arms are equal to the last digit). The triple posture pays more for the same
81 events (2944 refloats, one epoch below a decimetre). Both halves of the field
reading are now reproduced on a bed with truth: `gap_reentry_relevel` is necessary
on the dual posture, and a per-signal re-entry is necessary whenever a signal tracks
through a loss of lock of the primary pair.

**One expectation of section 12 was wrong.** The dual arm with the response was
expected to be healthy under the partial injection. It is not, because
`raw_observed` holds the band-5 observables in a dual-frequency run too, so the
satellite's row survives there as well. The hole is therefore open on the dual
field posture for every satellite that transmits L5 (17 of 32 on this day), which is
consistent with the further gain of the `holes` emulation on the dual field arm
(clock wire 0.0107 to 0.0081 m).

`sL_ds_tri_on` and `sJ_ds_dual_on` with the GRS rider are the acceptance arms of the
build; `sN_dsfull_tri_on` and `sM_dsfull_dual_on` give the level they should reach.

## 14. 2026-09-20: the gap re-entry response is armed in the working bases; the baseline pair is re-set

At the author's instruction `network.cycle_slip.gap_reentry_relevel: true` is armed in
`config/refound/{field,sim}_gps_dual_rf0hv.json` (the knob and a dated `_comment` block,
nothing else). The new baseline pair is `runs/_rf0hv4_{field,sim}6`, exe
`gnss_processor_pbh1.exe`, exit 0, 720 consumer epochs; its wires are byte-identical to
the validated arms (`_rap_dC_gapre` sha256 `4bbb10e1f89b622b...`, `_rap_sG_gapre_dual`
`46449eac18d3ca0b...`). Every later arm is a delta against this pair.

| bed | pair | consumer 3D p50 by hour (m) | sub-dm | T4_attrib | CLK_ANCHOR | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- | --- |
| field | previous (`_rf0hv3_field6`) | 0.57 0.17 0.11 0.10 0.13 0.11 | 167 | 457 | 598 | 0.0274 / 0.0281 |
| field | **new (`_rf0hv4_field6`)** | 0.53 0.11 0.09 0.10 0.14 0.12 | 203 | 228 | 424 | **0.0107 / 0.0173** |
| sim | previous (`_rf0hv3_sim6`) | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 | 0 | 127 | 0.0030 / 0.0040 |
| sim | **new (`_rf0hv4_sim6`)** | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 | 0 | 127 | 0.0030 / 0.0040 |

Field card-G line of the new pair: 131 resumptions, 88 shifted, 0 arcs shifted and
bridged (41 before), 176 fresh.

**The ledger row says KEEP_OFF, from another posture.** `docs/CONFIG_promotion_ledger`
graded this knob on 2026-09-09 on the converging leveling posture (12 h, 63 stations):
the mechanism worked (shifted-and-bridged 88 to 0, `T4a` refloats -46 %, the ZWD step
of BSHM00ISR at TOW 271200 from -2612 m to -0.028 m) and the float consumer got 56 %
worse (settled RMS 3.18 to 4.95 m, concentrated in hours 3 to 9), because every freed
arc re-levelled from one epoch of code. The same row records that no sim reproduction
was possible, the simulator having no tracking-gap scenario; the `dropslip` op of
section 12 now supplies one. On the float vehicle of RF-0hv the consumer is at a
decimetre, not at metres, and the 6 h grade is a gain in hours 1 to 3 and equal
afterwards. Because the old cost sat in hours 3 to 9, two 12 h arms of the dual field
base (`runs/_rap_d12_off`, `runs/_rap_d12_on`) are in flight; their reading, frozen: the
armed arm must not be worse than the unarmed one by more than 0.03 m of consumer p50
in any hour from 3 to 12, and its clock wire must stay better. A loss there reverts
the arming. The ledger row is owed an amendment scoped to the float vehicle once the
12 h arms and the GRS build have landed (it needs a loader posture row, and the
loader file is under edit by the build).

## 15. 2026-09-20: the 12 h arms: the arming stands

`runs/_rap_d12_off` (the previous dual field base) and `runs/_rap_d12_on` (the armed
base), 12 h, exe `gnss_processor_pbh1.exe`, exit 0, 1440 consumer epochs each.

| arm | consumer 3D p50 by hour, hours 1 to 12 (m) | sub-dm | T4_attrib | CLK_ANCHOR | clock wire over 12 h, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| unarmed | 0.57 0.17 0.11 0.10 0.13 0.11 0.11 0.13 0.17 0.16 0.18 0.40 | 223 | 1069 | 1354 | 0.0406 / 0.0440 |
| **armed** | 0.53 0.11 0.09 0.10 0.14 0.12 0.14 0.15 0.13 0.12 0.17 0.35 | **321** | **702** | 1066 | **0.0203 / 0.0303** |

By the frozen reading of section 14 the arming stands: from hour 3 to hour 12 the armed
arm is never worse by more than 0.03 m (the largest loss is 0.03 m in hour 7, the
next 0.02 m in hour 8) and it is better by 0.04 to 0.05 m in hours 9, 10 and 12; it
has 44 % more epochs below a decimetre, a third fewer post-fit refloats, and a clock
wire twice as good on the consumer's satellites over the whole window. The cost that
the ledger recorded on the old posture in hours 3 to 9 does not return on the float
vehicle. Both arms lose hour 12 (0.40 and 0.35 m), which is a separate lead and is
not read here.
