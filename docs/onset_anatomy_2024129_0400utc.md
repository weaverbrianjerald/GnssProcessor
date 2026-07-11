# Divergence-onset anatomy: 50-station GPS+Gal network, 2024-DOY129, ~03:55 UTC

All times are **content time** (run start 00:00 UTC = TOW 259200, wk 2313; hr = (tow-259200)/3600).
Primary run `t7d_tu_12h`; cross-checks `t7d_off_12h`, `t7f_shmarg_12h`, `t7e_pbcbfloor_12h`, plus
`t7h_offset2h_6h` (starts 02:00 UTC) and `t6_gg_freeiono_lowfloor_12h` (free per-LOS iono) as decisive discriminators.

## Event chronology (t7d_tu_12h)

| hr (UTC) | event |
|---|---|
| 3.633 (03:38) | **G08 leaves the PB set** (deterministic, identical in all 6 runs checked) |
| 3.767 (03:46) | **E14 leaves / G31 joins the PB set**; GPS PB ensemble re-levels (median \|step\| 12.1 m, decomposes as +14.8 m clock-like − 3.6 m iono-like at L1); G31 CLK steps −2.4 m; CB disc-counter waves (56 @3.733, 58 @3.767, 29 @3.80); **deg-1 SH RMS begins ramping** (8.5→12.7 TECU by 3.90); a10 climbs 0.5→6.9 TECU |
| 3.800 (03:48) | First DD steps >1 cyc above background: G04/G14/G08 at GUAM-DARW-SOLO-MAJU (west Pacific, LST 12.7–15.3) — present in tu, off, and offset runs |
| 3.900 (03:54) | G13 clock first CLK step (z=−14.6) |
| **3.917 (03:55)** | **Broadband breakout**: a00 first departure (30.3→26.5); 8 GPS clocks step \|z\|>8 simultaneously; 13 DD series step (background ~3/epoch); (1,1,S) dawn-dusk term explodes 16.6→25.5 |
| 3.95–3.98 | Sustained divergence (rms_all > 2× baseline permanently); clock RMS integrator ramp ~1 m/epoch |
| 4.15 (04:09) | Catastrophic: a00 −104→−3011 TECU in one epoch; LOS>3σ = 491/492 |

No data gap at onset (network-wide 2–3 min gaps exist at hr 0.83/1.48/1.58/2.32/3.32/4.70 — none in 3.4–4.6). No n_sat, n_pivot_pins, gate, or dimension change at onset (trace: n_present 50, n_clock_sats 53, n_phase_bias 35, n_pivot_pins 35, gate_ok=solved=1 throughout 3.7–4.05).

## Q1 — SH coefficient anatomy (window 3.5–4.5, z vs hr 2.0–3.5 baseline)

Top movers by first |z|>5 crossing, t7d_tu_12h:

| coef | kind | t(z>5) | t(z>10) | max\|z\| |
|---|---|---|---|---|
| (0,0,C) a00 | zonal | **3.917** | 3.950 | 6324 |
| (1,1,S) | sectoral | 3.933 | 3.950 | 10049 |
| (2,0,C) | zonal | 3.933 | 3.950 | 4408 |
| (4,1,S) | tesseral | 3.933 | 3.950 | 7615 |
| (2,2,C) | sectoral | 3.933 | 3.967 | 4136 |
| (3,0,C) | zonal | 3.950 | 3.967 | 4424 |
| … last: (2,1,C) | tesseral | 4.133 | 4.150 | 846 |

- **a00 leads by one epoch**, then the breakout is broadband: 11/25 coefficients cross 5σ within 3 epochs (3.917–3.950), all 25 by 4.133.
- Median t(z>5) by kind: **zonal 3.95** < sectoral 4.00 = tesseral 4.00. By degree: deg0 3.92 < deg1 3.98 ≈ deg4 3.98 < deg2 4.07. **Large-scale/zonal terms lead; no sectoral(m=n)-first pattern.**
- Subtle precursor: all three degree-1 terms creep from **3.767** (a10 0.5→6.9, a11C 8.4→12.6 TECU by 3.90) — below 5σ but monotonic. At 3.917 the **dawn-dusk term (1,1,S)** takes over explosively (16.6→38.5→54.6 TECU by 3.95).
- Cross-run: t7f_shmarg and t7e_pbcbfloor have the **identical top-3 first movers and timing** (a00/(1,1,S)/(2,0,C) at 3.917–3.933). t7d_off (noisier baselines, 4–17 TECU scatter) scrambles the z-ranking (first: (1,0,C),(4,3,S) at 3.933) but breaks out in the same 3.93–3.95 window.

## Q2 — per-degree RMS vs Kaula

Baselines (hr 2.0–3.5) and first departure > mean+5σ, t7d_tu_12h:

| degree | kaula_ratio base | departs at hr |
|---|---|---|
| 1 | 1.30 ± 0.15 | **3.933** |
| 2 | 1.20 ± 0.14 | 3.950 |
| 3 | 1.42 ± 0.28 | 3.950 |
| 4 | 1.04 ± 0.20 | 3.950 |
| 0 | 2.30 ± 0.10 | 4.100 (a00 transits zero → ratio dips first) |

Trajectory: rms_deg1 ramps **before** onset (8.5 @3.75 → 12.7 @3.90, +50% in 9 min) while deg2 falls and deg3/deg4 hold; at 3.917 all degrees inflate together, reaching 5–15× Kaula by 4.03 and 300–900× at 4.15. Same ordering (deg1 first, ~3.93–3.95; deg0-ratio last) in all four arms.

## Q3 — DD first-steppers and geography

Background rate: ~3 steps(>1 cyc)/epoch (hr 2–3.8). First 20 series (t7d_tu_12h; consecutive-epoch, same-ref diffs):

| hr | series | note |
|---|---|---|
| 3.800–3.833 | G14,G04(×3) ref G03/G04 @ GUAM/DARW/SOLO/MAJU (1.1–1.6 cyc) | west-Pacific precursor group, LST 12.7–15.3 (early afternoon) |
| 3.917 | E25 ref E03 @ CEBR-REYK, STJO-REYK (L1/L5/L7 coherent) | N-Atlantic, REYK 64N |
| 3.917 | E26 ref E03 @ NYA2-CHUR (1.35–1.39) | Arctic, NYA2 78.9N; **E26 arc only 32 min old** (rose 3.383) |
| 3.917 | G17,G14,G06 ref G03 @ CUSV-ULAB (±2.9–3.4 cyc, L2X/L5X pairs) ; G03 ref G02 @ KOKV-ULAB; G17 ref G02 @ GCGO-ULAB | central/east Asia, LST ~11 (late morning) |
| 3.933–3.967 | more E25/E26 @ MRC1/STJO/HOLB-REYK, NYA2; G10/G03/G17 @ GCGO-CHUR/ULAB/CUSV | |

Step counts/epoch: 4/3/1 @3.80–3.83 → 0 @3.85–3.90 → **13 @3.917** → 23–29/epoch → 137 @4.15.

**Geography.** Step-involvement fraction at breakout (3.91–3.96): CUSV 15/16 rows (0.94), ULAB 21/23 (0.91), REYK 0.38, KOKV 0.30, NYA2 0.29, CEBR 0.17. Two clusters: (i) **dayside Asia near the subsolar longitude (~121°E at 03:55)** — CUSV/ULAB/GUAM/DARW/SOLO/MAJU, LST 10.6–15.3; (ii) **high-latitude North Atlantic/Arctic** — REYK 64N, NYA2 79N, CHUR 59N, GCGO 65N (LST 18–22 and 0.4–4.7). **Not clustered on the 30°E sunrise terminator** (closest leader CEBR at −4.4°E, LST 3.6). Pattern reads dayside + auroral-latitude, not terminator.

**Satellites.** Leaders: E25, E26 (both vs pivot E03), G17, G14, G06, G03, G04. Mixed arc phases: E26 = fresh arc (32 min); E31 (one leading step) arc **ends at 3.933**, i.e. at onset; E25/G04/G14 up since run start; G17 mid-arc (0–5.87). Both constellations, both new and old arcs — no single shared trait beyond geometry through the two clusters.

## Q4 — Clock / phase-bias behavior

De-medianed-per-epoch (within constellation) step z-scores (diff-MAD scaled, baseline hr 2–3.5):

- **CLK**: G31 steps at 3.767 (z −36, −2.4 m, coincident with the PB-set event). G13 at 3.900 (z −14.6) is the **first onset-mover in any product**, one epoch before the SH breakout. At 3.917, **eight GPS clocks step simultaneously** (G05,G11,G12,G17,G19,G20,G24,G25; |z| 9–16). Galileo clocks lag 1–3 epochs (E24 3.933, E09 3.967, E18 4.000). GPS raw clock scatter grows 0.02→1.0–1.5 m/epoch from 3.917 while Galileo stays ~0.1–0.2 until later — **GPS leads, Galileo follows**.
- **PB**: the hr-3.767 "coherent Galileo −22 m step" in de-medianed space is a **median artifact of E14 leaving the PB set** (raw E median |step| < 2 m); the **GPS re-level is real** (raw median |step| 12.1 m, frequency-dependent: ≈ +14.8 m clock-like − 3.6 m iono-like at L1 — a datum re-allocation across clock/iono/bias, consistent with an IF-pin/anchor reshuffle when G31 entered / G08 left). Similar re-level events occur benignly earlier (0.60 G, 0.92 E, 2.52 E, 3.48 G) — 3.767 is the largest pre-onset one and the only one followed by collapse.
- Overlap with DD leaders is partial (G17 in both; G03/G06/G14 appear as DD sats vs refs G02/G03): the CLK stepping is network-global while DD stress is concentrated on the two geographic clusters.
- disc_counters: CB bumps are episodic and pre-onset (27 @3.533, 23 @3.583, 15 @3.65, **56 @3.733, 58 @3.767**, 29 @3.80, then ~0 until the crash; 308 @4.283). PB disc churn is a steady 20–38/epoch background throughout (not onset-specific).

## Q5 — Arc turnover

Per-10-min-bin DD-tracer series starts/ends (arc-proxy key, no ref):

| bin start hr | starting | ending |
|---|---|---|
| 3.167 | 8 | 27 |
| 3.333 | 299* | 227* |
| 3.500 | 26 | 35 |
| 3.667 | 7 | 2 |
| 3.833 | 10 | 16 |
| 4.000 | 32 | 7 |
| 4.167 | 55 | 41 |

(*) The 3.333-bin spike is a **logging artifact**: after the network-wide 3-min data gap at 3.317, the tracer logged 377 series for exactly 2 epochs (vs ~120 normally), then reverted. Real satellite-level turnover near onset: E26 rises 3.383, E31 sets 3.933, G22 set 3.383, E18 rises 4.283. **No mass arc turnover at or just before onset.**

Series alive at onset: 185, median age 0.55 h, max 3.68 h — **0% alive since run start** (the tracer's membership churns by design, so this is a weak arc proxy). The "mass turnover of arcs up since run start" variant is killed more decisively by the offset run (below): its states were only 1.9 h old at collapse.

## Q6 — Cross-run determinism (decisive)

- **Onset (sustained rms_all > 2× rolling median):** t7d_tu **3.967**, t7d_off 3.950, t7f_shmarg 3.967, t7e_pbcbfloor 3.967, t7g_fullfloor 3.967, t7g_lowfloor10 3.967, **t7h_offset2h_6h 3.967**, t6b_treated_lowfloor 3.983, t6_freeiono_lowfloor 4.083. First anomalous SH epoch is 3.917 in tu/shmarg/pbcbfloor/offset.
- **Same leaders:** t7d_tu ≡ t7f_shmarg ≡ t7e (identical first-20 DD list: E25×7, E26×5, G17/G06/G14×2, G03, E31; identical stations REYK 8, ULAB 7, NYA2 6, CUSV 5). t7d_off overlaps heavily (E26, E25, E31, G04/G14, NYA2/STJO/CHUR/HOLB/GUAM) with ordering scrambled by its higher noise floor.
- **t7h_offset2h_6h (starts 02:00 UTC):** same west-Pacific precursor at 3.800 (G04/G08/G14 @GUAM-DARW-SOLO-MAJU), same 3.917 breakout with the same series (E25 @CEBR-REYK, G17/G14/G06 @CUSV-ULAB, E26 @NYA2-CHUR). **Collapse at run-age ≈1.9 h — onset is content-time-locked, not run-age-locked.**
- **t6_gg_freeiono_lowfloor_12h (per-LOS free iono, healthy LOS>3σ baseline of 49/490 = 10%):** also collapses (a00 → −7550 by 4.32), starting ~3.98–4.08, 1–2 epochs later; leaders are the same Galileo/Arctic core (E26 @NYA2/CHUR/HOLB 3.883, E08/E15 @N-America 3.80) while the CUSV/ULAB GPS L2X/L5X steps are absent (those were SH-model-mediated). **Even without the SH constraint the network collapses at the same content time.**
- **PB-set events are epoch-identical in all six runs:** G08 out @3.633; G31 in / E14 out @3.767.

## Verdict

| Q | Answer |
|---|---|
| 1 | a00 leads by 1 epoch; breakout broadband within 2–3 epochs; zonal/low-degree first, no sectoral pattern; deg-1 terms creep from 3.767, (1,1,S) dawn-dusk explodes at onset |
| 2 | deg-1 leaves Kaula band first (3.933), deg2/3/4 at 3.950, deg0-ratio last (4.100, zero-crossing artifact); deg-1 RMS ramps +50% during 3.75–3.90 pre-onset |
| 3 | Precursor 3.80: G04/G14/G08 @ west Pacific (local early-afternoon). Breakout 3.917: E25/E26 vs E03 @ Arctic/N-Atlantic + G17/G14/G06/G03 @ CUSV/ULAB (dayside Asia, LST ~11). Two clusters: dayside-Asia near subsolar lon + auroral latitudes. **Not** the 30°E sunrise terminator. Sats mixed rising/setting (E26 fresh 32-min arc, E31 arc ends at onset, others mid-arc) |
| 4 | Yes: G13 CLK steps at 3.900 (first mover anywhere), 8 GPS clocks at 3.917, Galileo lags 1–3 epochs; GPS PB ensemble re-leveled 12 m at 3.767 (clock+iono decomposition) when E14 lost / G31 gained PB; partial sat overlap with DD leaders (G17) |
| 5 | No arc-turnover spike at onset (3.33 spike = post-gap logging artifact); 0% of tracer series alive since run start (weak proxy — tracer churns); age hypothesis killed by offset run |
| 6 | Same coefficients, same sats, same stations, same PB-set events, same epoch — in different config arms, a 2-h-offset start, and a free-iono parameterization. **Deterministic external/data event.** |

**Overall read: hypothesis (a) — external, content-time-locked data/geometry event at 03:46–03:55 UTC — is the trigger; hypothesis (b)'s "~4 h internal accumulation clock" is falsified.** Strongest evidence for (a): `t7h_offset2h_6h`, started 02:00 UTC, collapses at the same 03:55 content time at run-age 1.9 h, with the same leading series. Strongest remaining evidence for (b): the response is a textbook internal runaway — a smooth ~1 m/epoch clock-RMS integrator ramp and a00 → −9000 TECU from a finite disturbance, preceded by shared-datum churn (E14/G31/G08 PB-eligibility turnover and a 12 m GPS PB/clock/iono datum re-allocation at 3.767, CB disc waves 3.73–3.80) — i.e., the external event is survivable in a well-conditioned filter; the shared carried-information/datum machinery (common to every arm, including free-iono) amplifies it into collapse.

Candidate physical event, unverified offline: the leading clusters (dayside Asia near subsolar longitude ~121°E, plus auroral-latitude REYK/NYA2/CHUR/GCGO) at 03:46–03:55 UTC on 2024-05-08 — during the flare-active run-up to the May-2024 Gannon storm (AR13664) — are consistent with a sudden dayside/high-latitude ionospheric disturbance that a deg-4 sun-fixed VTEC cannot follow; but the epoch-exact PB-eligibility turnover (E14 out / G31 in at 03:46) is an equally deterministic candidate seed. Discriminator worth running: a run with the SH model *and* the 3.767 PB events but hardened datum handover (or E14 withheld from the PB set for the whole run) — if it survives 04:00, the trigger is the handover, not the ionosphere.
