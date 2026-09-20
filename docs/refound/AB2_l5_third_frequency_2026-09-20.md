# AB2: GPS L5 as a third frequency (add-back 2)

**STATUS**

- **id:** AB2
- **topic:** network-vehicle
- **status:** open
- **summary:** Second add-back on the held-wire posture. Census findings and the config-only bench AB2-0 (does a triple arm stay healthy, is it neutral for the float consumer, and does the L5 phase bias carry signal that the hold must not freeze) are pre-registered here.
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
