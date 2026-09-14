# Study 4 — the slip DOSE — the anchor-slip handover cure, validated on the harness and on the storm bed

**STATUS**

- **id:** Study 4
- **topic:** slip
- **status:** closed
- **summary:** the slip DOSE — the anchor-slip handover cure, validated on the harness and on the storm bed
- **dates in the text:** 2026-09-10 → 2026-09-11
- **knobs / posture:** `network.cycle_slip.anchor_slip_handover`, `network.clock_datum_sum_to_zero`
- **hashes named in the text** (commit / exe / byte-cert): `3760bc71`, `95dc556f`, `ec26de1`
- **run dirs named in the text:** `runs/golden_sim_campaign_zwdblk`, `runs/evseed_quiet6h_s2z`, `runs/golden_sim_campaign_zwdblk_ancho`
- **source:** `docs/archive/SPEC_event_harness.md` — 12, 12.0, 12.1, 12.2, 12.3, 12.4, 12.5, 12.6, 12.7, 12.8, 12.9, 12.10, 12.11, 12.12, 12.13

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 12. Study 4 — the slip DOSE: does a CONVERGED network survive the campaign's slip rate?

*Pre-registered 2026-09-10 evening, local (the snapshot files carry 2026-09-11 UTC stamps), Fable manager session, BEFORE any dose leg was graded.
The legs were generated first, the gates written second, the numbers read third.*

### 12.0 Why this study replaced "lane-evidence band choice"

§Z.7 closed with "choose the kept band on evidence that can see a level error —
the lane integers".  The campaign bed's own numbers refuse that before anything is
built: on `runs/golden_sim_campaign_zwdblk` the ref-free lanes are RIGHT
(EWL(2,5) 99.2 %, WL(1,2) 86.2 % at their own true integer) while the per-band
levels are WRONG (L1C 44 % at integer, ambient per-band DD error 1.7 cycles,
per-chord medians 2.9–3.4 cycles per band).  A level error that leaves both lanes
intact is one that is EQUAL IN CYCLES across the bands — exactly the (ρ, I) split
direction §6.17 identified (δρ ≈ −1.28 δI) — and the pure-iono direction Study 2
measured on the erase floor moves WL by only ≈ 0.13 cycle per 0.45-cycle level
error and EWL by ≈ 0.03.  The lanes are therefore blind to the very error the
band choice has to see; the item is dropped, not deferred.

What the campaign statistics DO say (measured from the fixture's 61
`truth_slips.csv`): 2486 slips in 5.98 h = **6.8 per station-hour**; 2476 of
2481 events are **single-band**; |Δ| 12–125 cycles (p50 50); 50.6 % LLI-flagged.
Study 1 priced a detector-handled single-band slip at **0.04 cycle** and an
all-band erase at **0.45 cycle for 45+ epochs**; T4 fires 21 times per
station-hour on the storm bed against 6.8 true slips.  Every storm arm starts
COLD on a bed that is already slipping, so no arm has ever asked whether a
network that converged first would decay under the same schedule.  That is the
question a harness can answer in minutes and a storm bed cannot answer at all.

### 12.1 Mechanics

`scripts/ev_dose_legs.py make` resumes the quiet seed (`evseed_quiet6h`, ep0665,
the snapshot every validation leg used) and injects a Poisson schedule of
single-band slips sampled from the campaign's empirical |Δ| population, band
uniform within the constellation, LLI on half, elevation ≥ 15°, no two events on
one (station, sat) within 20 epochs, none in the last 30 epochs (every slip gets
an aftermath).  Same `--rng` ⇒ same schedule, so an attribution twin changes one
knob and nothing else.  The injector cap moved 64 → 4096
(`GnssConfigLoadNetwork.cpp`, `kMaxInjectEvents`; a paste guard, not a design
statement).  Grader: `scripts/event_dose_grade.py` (folds EVERY injected slip
into the truth; station-frame trajectories for the dosed stations and the
bystander pool; the (tier, event, action) census split into injected vs other
arcs).  Exe: `gnss_processor_dose.exe` = main HEAD + the cap (3760bc71…);
its null must reproduce the gate null (`evleg_gate4_null`, 95dc556f…, itself
byte-identical to the merged harness null) over the shared 62 epochs.

Legs (all 240 epochs = 2 h, ~7 min each; 3 concurrent at most):

| leg | schedule | question |
|---|---|---|
| `evleg_null240` | none | the control every diff is read against |
| `evleg_dose1_s21` | S21000SIM alone, campaign rate (9 events, rng 1) | does ONE station's frame walk under its own churn? |
| `evleg_doseall` | all 60 stations, campaign rate (655 events, rng 1) | does the converged NETWORK decay? |
| conditional `dose3_s21` | S21000SIM, 27 events | dose–response, if dose1 is ambiguous |
| conditional `dose1_s21_not4` + matched `null240_not4` | same rng-1 schedule, `raim_arc_persist_epochs: 1e6` (E1's starve — `raim_attribution: false` is silently RE-ARMED by the golden's `raim_within_epoch`, loader line ~4752) | is the walk the T4 chain or the detector re-levels? (only if T4 fires ≥ 3× on S21 in dose1) |
| conditional `doseall720` | all stations, 6 h | if doseall has a trend at +240 that has not saturated |

### 12.2 Hypotheses and gates (frozen)

Read at the last bin (+220..+240), event − null, dosed station(s) and bystanders:

- **H1 — the plateau is an IGNITION artifact.** A converged network absorbs the
  campaign's slips at Study 1's per-slip cost and does not decay.  Prediction on
  `doseall`: dosed-pool median |level error| diff < 0.15 cycle, ZWD rms diff
  < 5 cm, consumer 3D diff < 10 cm, T4 census ≤ 10 rows network-wide over the
  leg.  If H1 holds, the storm-bed plateau is a CONVERGENCE problem (how to
  converge while slipping), not a slip-response problem, and every response
  knob falsified on the storm bed was being graded against a network that
  never converged.
- **H2 — station-local walk.** The dosed station's frame walks under its own
  churn.  Prediction on `dose1_s21`: ZWD |diff| > 10 cm OR median |level error|
  diff > 0.5 cycle OR ≥ 6 T4 rows on S21000SIM, with bystanders flat (their
  pool diff within the null-vs-null floor).
- **H3 — network-coupled walk.** `dose1_s21` flat, `doseall` decays (the walk
  needs many stations' re-levels interacting through the anchors / wire).
- **H4 — false alarms drive it.** Detector rows on NON-injected arcs of dosed
  stations exceed the null's by ≥ 2× (offers and actuations separated by the
  `action` column).

Verdict words: **flat** = |ZWD diff| < 2 cm AND level diff < 0.15 cycle AND
T4 rows ≤ 2 on the station over the leg; **walk** = the H2 numbers; anything
between = ambiguous → the 720-epoch leg before any verdict.  A `walk` on
`dose1_s21` makes the harness the development loop for the cure at 7 min per
iteration; a `flat` on `doseall` retires the slip-response family and opens the
convergence card.

### 12.3 First pass (2026-09-10 evening, hand-graded with the trace-complete channels; the dose grader re-grades on landing)

Gate: the dose exe's `evleg_null240` reproduces `evleg_gate4_null` over the 62
shared epochs (0 differing lines of 25 989), and the gate itself reproduced the
merged harness null (95dc556f…).  Byte-identity holds on the null path.

| leg | injected slips | station ZWD, event − null, last bin (+200..+239) | T4 erases | detector rows on injected arcs | bystanders |
|---|---|---|---|---|---|
| `dose1_s21` (S21000SIM, campaign rate) | 9 | **+4.3 mm** (creep 0.4 → 4.3 mm over the leg); rx-clock contrast 1–2 mm; consumer −2 mm | **0** | MW 9 / LLI 4 / IF-TD 5 — 9 of 9 caught | ZWD diff median 0.01 mm, max 0.15 mm (59 sta) |
| `dose3_s21` (S21000SIM, 3× rate) | 27 | **+4.0 mm** | **0** | MW 23 / LLI 12 / IF-TD 13 | — |
| `doseall` (all 60, campaign rate) | 655 | median \|diff\| **221 mm**, rms **1.83 m**; 11 stations > 1 m, 15 > 0.5 m, 45 > 0.1 m (of 60) | **3 099** = 25.8 per station-hour, flat from the FIRST bin (402 / 384 / 779 / 517 / 555 / 452 per 30–40 ep) | MW +581 / IF-TD 350 / LLI 312 | none (all dosed) |

Other doseall counters vs the null: `CLK_ANCHOR` decouple/recouple **258 / 259**
(null 2 / 3), `PBGAUGE handover_cold` **22** (null 5), `CLK_STEP mark` **16** (0),
`T1_if_slip refsat_reset` **78** (0).  Consumer 3D event − null by bin: +0.3, +0.5,
−0.8, −0.4, +0.6, **+1.4 m** (the null's own consumer sits at 0.6–2.4 m on a
resumed wire — §6(1) — so this channel is read for trend only).

**Per-slip price on the converged station** (dose1, signed event − null with the
injected delta folded into the EVENT leg's truth only; GPS arcs only — the DD
trace has no Galileo chords at S21000SIM, its reference E02 is below the horizon
there, and its GPS chords end near +160 when G01 sets: the coverage half of
§6.13): G27 L1C +69 cycles — MW(1,5) and IF-TD(1,2) fire together, so all THREE
bands are freed — residual **−0.65 cycle at +1, −0.53 at +5, −0.40 at +20, −0.11
at +45** (the §9 all-band floor, decaying); G27 L5Q +57 cycles — MW(1,5) alone,
L2W stays pinned — **−1.57 at +1, −0.52 at +5, −0.11 at +10**.  The station's
OTHER satellites: median |diff| 0.01–0.04 cycle per bin (max 0.63, which is the
slipped satellite's other bands re-drawing).  One injected slip (G16 L1C, +152)
drew no detector row at all: G16 is not in the network's solution at S21000SIM
(no chord in the trace either), so the generator must pick satellites the
network USES at the station, not satellites the truth says are visible.

**Where the outlier tier fires on `doseall`, with the base rate** (the honest
half of a lag attribution — at this density every station slips every ~11 min and
every satellite every ~6 min, so "a slip nearby just happened" is nearly always
true; the base rate is 20 000 random (station, sat, tow) triples through the same
ordered categories):

| category (ordered) | T4 erases | base rate | enrichment |
|---|---|---|---|
| same ARC slipped within 30 min | 15.6 % | 4.7 % | **3.3×** — the re-erase chain (§6.17 H-C) |
| same STATION, other satellite, within 10 min | 58.6 % | 59.3 % | none |
| same SATELLITE, other station, within 10 min | 15.8 % | 19.4 % | none |
| none of the above | 10.1 % | 16.6 % | — |

Per station: corr(injected slips, T4 erases) = **0.13** — a station's erase count
does not follow its own slip count (T4 per station 23 / 45 / 157 min / median /
max); corr(T4 erases, |ZWD error| at the end) = **0.68** — where the erases land,
the frame walks (S43 7.6 m, S32 −5.5 m, S28 4.8 m with 56 / 77 / 121 erases;
S06 / S42 / S45 within 25 mm with 27–39).  Erases hit 974 distinct (station,
sat) arcs, 475 of them ≥ 3 times, max 18; 45.7 % landed on arcs that had an
injected slip at all.

**Reading before the twins land.** H1 is falsified: the converged network decays
inside the first 30 epochs of network-wide slips, so the plateau is not an
ignition artifact.  H2 is falsified twice: one station at the campaign rate and
at three times it stays flat (4 mm, zero erases, other satellites at 0.03 cycle).
H3 is what the numbers show: the outlier tier fires only when MANY stations
slip, and it fires on arcs that have nothing to do with the slips (no
same-station or same-satellite enrichment), at a rate that does not track the
station's own churn.  The one enriched category is the arc's own re-erase, 11 %
of the total.  Pending: `doseall_not4` (same 655 events, T4 starved — is the
tier the amplifier or a symptom?), `dose24` / `dose12` / `doseall_half` (is the
onset a density THRESHOLD or linear in the dose?).

### 12.4 The twins and the density ladder (2026-09-10 evening, `scripts/event_dose_grade.py` + the quick frame read)

All legs share the `doseall` schedule (`--from-leg`): a rung is a SUBSET of the same
655 events, so the events themselves are identical across rungs.  Every leg is
graded against `evleg_null240` (the T4-starve knob is inert on the null: it fired
0 times there).

| leg | slips / stations | T4 erases per bin (10–39 / 40–79 / 80–119 / 120–159 / 160–199 / 200–239) | onset | ZWD rms diff at +200..+239 | wire diff (m) at +100 / +180 / +220 | consumer diff, last bin |
|---|---|---|---|---|---|---|
| `dose1_s21` | 9 / 1 (S21, anchors 0 families) | 0 everywhere | never | 4 mm | — | −0.001 |
| `dose3_s21` | 27 / 1 (S21) | 0 everywhere | never | 4 mm | — | +0.000 |
| `dose_s30` | 13 / 1 (**S30 = clock datum**, anchors 9 families) | 0 / 0 / 0 / 45 / 294 / 12 | **+120..+159** | (single station) | 0.0003 / 1.36 / 1.76 | +0.22 |
| `dose12` (incl. S30) | 128 / 12 | 0 / 0 / 50 / 141 / 309 / 12 | +80..+119 | 0.80 m (dosed), 48 bystanders 0.06 m | 0.005 / 1.44 / 1.80 | +0.05 |
| `dose24` (incl. S30) | 262 / 24 | 0 / 0 / 61 / 195 / 293 / 150 | +80..+119 | 1.6 m (dosed), 36 bystanders 0.08 m | 0.019 / 1.79 / 2.09 | +0.51 |
| `doseall_half` | 328 / 60 | 168 / 6 / 443 / 287 / 346 / 174 | +10..+39 | 1.3 m | 2.75 / 3.31 / 3.51 | +0.46 |
| `doseall` | 655 / 60 | 402 / 384 / 779 / 517 / 555 / 452 | **+11** | 1.83 m (median 221 mm) | 3.80 / 4.74 / 5.18 | +1.36 |
| `doseall_not4` (T4 starved) | 655 / 60 | 0 everywhere (by construction) | — | 1.49 m (**median 441 mm**) | 1.21 / 1.73 / 2.21 | +1.34 |

**(1) The outlier tier is a symptom.**  With every T4 erase removed the walk
persists; the median station is WORSE (441 vs 221 mm), the rms is a little
better (fewer blow-ups), the consumer ends the same (+1.34 vs +1.36 m), and the
clock-anchor decouple count is unchanged (267 vs 258).  The detector rows on
NON-injected arcs in `doseall_not4` equal the null's (no false-alarm cascade
either).  What is left as the engine is the 655 detector re-levels themselves.
The levels of untouched satellites drift more without T4 (`oth_d` 5.4 vs 3.0
cycles at the end as-traced) while the wire is SMOOTHER without T4 (2.2 vs
5.2 m): the erases keep the levels closer and chop the wire into steps.
⚠ The as-traced level channels carry the trace's reference satellite (§6.13), and
in a dose leg that satellite's own arcs are re-levelled at many stations, so the
diff no longer cancels it.  `event_dose_grade.py --ref-free` (the
`simnet_dd_reref.py` method: per-(ref, pair, band, epoch) group median removed,
groups of ≥ 3 satellites, identical group structure in both legs) puts the
reference term at 0.014 of the 0.037-cycle single-slip residual (38 %) and at
more than half of the dose legs' drift.  REF-FREE, at +220..+239: `doseall`
untouched arcs 0.95 cycle (as-traced 2.23), injected arcs 0.18 (1.30);
`doseall_not4` untouched 2.07 (4.96), injected 0.94 (5.10); `dose24` untouched
0.18 (0.52).  The ordering survives the correction: untouched arcs drift by a
cycle over two hours in the 60-station dose, twice that without the erases, and
the injected arcs settle far better WITH the erases (0.18 vs 0.94) — the tier
re-draws them closer to the network's current frame, wrong as that frame is.

**(2) It is a cascade with an incubation that shrinks with density.**  On
`dose24` the network is silent for 80 epochs (ZWD diff 0.1–1 mm, levels 0.000,
wire diff 0.6 mm) and then the wire disturbance DOUBLES roughly every 20 epochs
(0.9 → 3 → 9 → 19 → 40 → 78 mm → 0.94 m at +160 → 2.1 m), the 36 quiet
bystander stations walk with it (median 22 mm, rms 80 mm at +240: coupling
through the wire, proven on stations with no slips at all), and the outlier
tier starts erasing only once the wire is already off by centimetres.  At 60
stations the doubling is immediate; at one non-datum station the gain is
below one and nothing grows.

**(3) Which station slips matters more than how many.**  S21000SIM (anchors no
phase-bias family; not the clock datum) stays flat at 27 slips.  S30000SIM, the
`clock_datum_station` (and anchor of 9 families), cascades ALONE with 13 slips,
onset +120..+159, wire to 1.76 m.  A code-quality re-level at the datum station
cannot be absorbed by its own free receiver clock (that clock IS the network's
zero), so it goes into every satellite clock instead; the same for a family's
anchor arc, whose re-level re-references the whole satellite.  Enrichment on
`doseall`: the 35 slips that landed on the current anchor arc of their
(sat, signal) are followed by 8.1 clock-anchor decouples in 3 epochs (base 3.5)
and 86 T4 erases (base 41).  Pending to separate "clock datum" from "anchor
load": `dose_s29b` / `dose_s31b` (S29 / S31 anchor 18 families each, neither is
the clock datum, 14 / 15 slips) and `doseall_noS30` (642 slips, everything but
the datum station).

**(4) Working model.**  A re-level from one epoch of code inherits the station
frame's displacement at that moment (invariant #1's re-coupling edge).  Young
arcs at the datum / anchor stations move the satellite products directly; young
arcs elsewhere move them by their weight.  Every station's frame absorbs the
moved products into its clock / ZWD / iono because its old arcs are stiff.  The
next re-levels inherit MORE displacement.  Above a critical (rate × coupling)
the loop's gain exceeds one and the network walks; the storm beds live far above
it from their first epoch, which is why every per-arc response knob tested on
them moved the plateau by tens of percent at most.  The test of the model is
`doseall_bscope` (running): the same 655 events with both band-scope knobs ON,
so every single-band slip frees ONE band and the re-level stays phase-precise
(no code-quality young arcs) — if the converged network HOLDS, the fuel is the
whole-satellite refloat and the storm-bed falsification of band scope (§Z.7)
was an artifact of starting cold on a slipping bed.

**(5) Harness defects fixed on the way.**  `event_leg.py` / `chain_make_leg.py`
anchored `start_offset_hours` on the GPS DAY START while the producer's
fast-forward gate anchors on the FIRST SYNCED OBSERVATION: the campaign
fixture (12:00 start) was refused 12 h late, and every quiet-fixture leg before
the fix resumed ONE EPOCH LATE (first emitted tow = snapshot + 60 s).  Both
scripts now read `TIME OF FIRST OBS` (`fixture_first_obs_s`); a post-fix null
resumes at snapshot + 30 s (verified, `evleg_null_fixchk`).  Post-fix legs are
therefore NOT byte-comparable with pre-fix legs; every §12.3–12.4 leg is
pre-fix and internally consistent (`doseall_bscope` and `null240_bscope` are
the first post-fix pair, compared only with each other).  Also: the generator
must pick satellites the network USES at a station (one injected slip landed on
a satellite outside S21000SIM's solution and drew no response at all), and the
slipped seed's last snapshot (ep0665) leaves only 55 epochs of the 6 h
fixture — legs on `evseed_slipped6h` start from ep0604.

### 12.5 The band-scope twin FAILS, and the datum-station timeline names the engine (2026-09-10, evening local)

**`doseall_bscope`** (same 655 events; `refloat_band_scope: implicated` +
`raim_erase_band_scope: min_keep_one`, graded against its own matched null,
both post-fix legs): the scope acted (run-end clause: T4 4 019 cells → 6 855
bands erased / 4 019 kept; detector refloats band-scoped, 72 + 56 + 56
`band_scope` rows) and the network cascades exactly as before or worse — T4
onset +10..+39 (353), ZWD rms 0.90 → 2.24 → 1.78 m, wire 4.9 m, consumer
**+1.98 m** (doseall +1.36), clock-anchor decouples 295, refsat resets 79.
§12.4(4)'s prediction is FALSIFIED: phase-precise single-band re-levels do
not prevent the cascade, so code-quality young arcs are not the fuel.

**`dose_s30` epoch by epoch** (the clock-datum station alone, 13 slips; S30
has no DD chords at all, so the level channels are blind there and the
station's ZWD is the instrument):

| epoch | event at S30000SIM | response | clock-anchor decouples | T4 erases (station:count) | S30 ZWD diff |
|---|---|---|---|---|---|
| +23 … +105 | six single-band slips | detector refloats (2 of 3 bands; one on the IF-TD reference sat) | 0–1 | 0 | 0.3 → 6.7 mm |
| **+122** | G25 L5Q +56 cyc (elev 39°) | **no detector row — caught by T4 in the same epoch** (`attrib_arc`, all bands) | 3 (E36, G15, G25) | 3 (S30:1 S43:1 S59:1) | **−807 mm in that epoch**, −37 next, +22 by +129 |
| **+151** | G20 L5Q −57 cyc | MW refloat L1C;L5Q (L2W kept) | 2 (E36, G20) | 5 — ALL at S30 | **+911 mm** |
| +152 … +157 | — | — | 0 | 6 / 7 / 5 / 3 / 5 / 5 — all at S30 | 2.4 → 4.4 → 6.8 → 8.3 → 9.0 → 9.6 m |
| +160 … +185 | two more slips (+161, +166) | detector refloats | 0 | 1–3 per epoch, all at S30 | plateau **10.2 m**, decaying 9.4 m by +185 |
| **+186** | G12 L1C +45 cyc (LLI) | LLI + IF-TD + MW | **15 satellites in one epoch** (G04 G05 G06 G09 G10 G11 G12 G15 G18 G19 …) | 29 → 44 → 43 → 59 → 47 → 32 → 21 per epoch, at S36 S38 S43 S12 S20 S48 S25 S15 S04 S09 S07 … | 9.65 m |
| +193 … +240 | — | — | 0–2 | 1 per epoch at S30 | 6.0 → 2.6 m, still decaying |

**The engine.**  S30000SIM is `clock_datum_station`: its receiver clock is the
network's clock zero, so it has NO free receiver clock to absorb a slip
response.  At a non-datum station (S21000SIM, 27 slips, flat) the epoch's
inconsistency after a refloat is absorbed by the station's per-epoch clock at
no cost; at the datum station it goes into the ZWD and the satellite clocks.
The displaced ZWD makes the station's OTHER arcs inconsistent, the outlier
tier erases them (5–7 per epoch, all at S30), each erase re-draws an arc from
code against the displaced ZWD, and the frame runs away to 10 m in six epochs
— a self-consistent, catastrophically wrong station frame that then decays
over an hour.  The next slip response at that station (+186) re-realizes the
clock datum from a 10-m-wrong frame and broadcasts a step into 15 satellite
clocks at once; every station sees its satellites step and the outlier tier
fires network-wide — the same signature (`cta_decouple` bursts of up to 20
satellites in one epoch) the `doseall` legs showed from +11.  The +122 miss
adds a second lesson: a slip that reaches the solve (caught post-fit) at the
datum station puts 0.8 m into the emitted products of that epoch.

This is why band scope, T4 starvation, level memory and every other per-arc
response tested on the storm beds could not move the plateau: the bed's datum
station is one of the 60 slipping stations from its first epoch.  Pending
discriminators: `doseall_noS30` (642 slips, everything but the datum station —
does the network cascade without it?), `dose_s29b` / `dose_s31b` (the two
stations that anchor 18 phase-bias families each, neither the clock datum —
does anchor load cascade alone?).  Candidate cures follow from the mechanism:
a clock datum that is not one station's fixed clock (`clock_datum_sum_to_zero`,
sim-validated for wander in the readmit card, currently a golden exception), or
a datum HANDOVER away from a station for the epochs of its own slip response
(the phase-anchor sticky handover's sibling for the clock pivot).

### 12.6 The gauge is the engine — both gauges (2026-09-10, evening local)

| leg | station(s) dosed | phase-bias families the station anchors at +10 | clock datum? | slips | T4 erases (onset bin) | clock-anchor decouples | wire diff at +220 | consumer diff, last bin |
|---|---|---|---|---|---|---|---|---|
| `dose1_s21` / `dose3_s21` | S21000SIM | **0** | no | 9 / 27 | 0 (never) | 2 (= null) | — | 0.00 |
| `dose_s30` | S30000SIM | 9 | **yes** | 13 | 352 (+120..+159) | 27 | 1.76 m | +0.22 |
| `dose_s29b` | S29000SIM | **18** | no | 14 | 163 (**+10..+39**) | 18 | 0.57 m | +1.26 |
| `dose_s31b` | S31000SIM | **18** | no | 15 | 777 (+40..+79) | 77 | 5.04 m | +0.88 |
| `doseall_noS30` | 59 (all but the datum) | — | no | 642 | 2 897 (**+10..+39**) | 266 | 5.60 m | +1.80 |
| `doseall` | 60 | — | incl. | 655 | 3 099 (+10..+39) | 258 | 5.18 m | +1.36 |

A station that anchors NO family and is not the clock datum can slip 27 times
in two hours and the network does not notice.  A station that anchors 18
families cascades alone with 14–15 slips; the clock-datum station cascades
alone with 13 (§12.5); the network without its datum station cascades exactly
as fast as with it.  The engine is therefore not "many slips" but **a slip
response on a GAUGE-DEFINING arc**: the arc whose ambiguity realizes a
satellite's phase-bias datum (one per (sat, signal) family), or any arc of the
station whose clock realizes the clock datum.  On `doseall`, 35 of the 655
slips landed on the current anchor arc of their family; the anchor never moved
afterwards (0 of 35 — no handover of either kind, no `anchor_handover` row,
the anchor station unchanged in the trace for the following 3 epochs), so the
family's gauge was re-realized in place from the freed-and-re-levelled arc.
The same 35 were followed by 2.3× the base rate of clock-anchor decouples and
2× the T4 erases within 3 epochs.  A continuous handover exists in the code
(`NetworkPhaseDatum.cpp`, the `usable` branch: pin the successor at its own
carried `value_abs`) but nothing invokes it when the incumbent's arc is freed
by a slip response — the sticky election keeps the slipped incumbent.

**The slipped seed cannot read this at S21000SIM** (worker report, `evleg_sl_erase`
on `evseed_slipped6h` ep0604): the leg ran warm and clean (first emitted tow
= snapshot + 30 s, erase fired, 24 chords refloated, 0 re-flags, ZWD −2 mm,
consumer +0.57 m peak, L1C phase-bias diff +2.2 m at +38) but the DD trace has
NO rows for S21000SIM on the campaign fixture — its GPS reference is G02,
which S21000SIM never sees, and only 36 of 60 stations appear in that trace
at all.  Any slipped-bed level grade must pick a station inside the trace's
coverage (§6.13's coverage half, again).

**Pre-registered test of the cure, written BEFORE the legs were read.**
Three 60-epoch legs on the quiet seed (post-fix; null = `evleg_null_fixchk`),
one +40-cycle L1C slip at +10 on S29000SIM: `anc_slip` on G10 (S29 anchors
G10's three families), `nonanc_slip` on G27 (S29 is not G27's anchor), and
`anc_slip_ho` = the G10 slip plus an injected `anchor_handover` {G10, all,
continuous} in the SAME epoch (the harness's candidacy ban, forcing the
production election to pick a successor and take the continuous branch).
Gates: (i) `nonanc_slip` must read like Study 1's slip (residual ≤ 0.1 cycle,
frame reaction ≤ 0.02 cycle, no clock-anchor decouple burst, ≤ 2 T4 rows);
(ii) `anc_slip` is expected to show the engine — a clock-anchor decouple burst
within 3 epochs, T4 rows, a wire C/P step; (iii) the CURE passes if
`anc_slip_ho` brings the decouples, T4 rows and the wire C/P peaks to within
2× of `nonanc_slip`'s, and its frame reaction to ≤ 0.05 cycle; it FAILS if
the forced continuous handover leaves the burst or if the handover itself
steps the wire (Study 1's continuous handover cost +0.40 m for one epoch).

### 12.7 The cure, validated on the harness before it exists in production (2026-09-10, evening local)

⚠ Correction to §12.6's pre-registration: `nonanc_slip` was NOT a non-anchor
control — S29000SIM anchors G27 too (its anchor list was truncated when the leg
was chosen; S29 anchors E08, E15, G10, G23, G27, G28 at the snapshot).  Both
legs slipped an anchor arc; a true non-anchor control at S29 is `anc_ctl_s29`
(below).  The three legs, frame grader vs `evleg_null_fixchk` (post-fix):

| leg | +40-cycle L1C slip on | forced continuous handover? | other arcs of S29 (cycles) at +1 / +8 | S29 rx clock diff at +1 / +8 (m) | S29 ZWD diff at +1 / +8 (m) | emitted C diff for the sat at +8 (m) | T4 erases +0..+45 | clock-anchor decouples +0..+45 (null 1) | consumer diff at +14 |
|---|---|---|---|---|---|---|---|---|---|
| Study 1 `evleg_slip` (S21000SIM, anchors nothing) | S21/G10 | — | 0.004 / — | 0.001 / — | −0.001 / — | 0.000 | 0 | 0 | 0.000 |
| `anc_slip` | S29/G10 (anchor of 3 families) | no | **13.1 / 11.7** | **10.7 / 8.8** | −2.0 / −2.1 | **−8.9** | **157** | **13** | +1.53 |
| `nonanc_slip` (actually an anchor arc) | S29/G27 (anchor of 3 families) | no | 4.3 / 2.2 | 2.0 / 5.1 | 0.60 / −0.47 | −7.8 | 71 | 7 | +1.25 |
| **`anc_slip_ho`** | S29/G10 | **yes — G10's three families to S06000SIM in the slip epoch** | **0.07 / 0.12** | **−0.010 / −0.019** | 0.001 / 0.003 | **0.002** | **0** | 1 | +0.09 |

The anchor-arc slip without a handover re-realizes the family from the freed
arc: S29's clock swings 10 m, its ZWD 2 m, the satellite's emitted clock steps
9 m, and the outlier tier erases G10's HEALTHY arcs at eight other stations in
the same epoch (S11 S16 S19 S24 S27 S37 S40 S45), then S29's own other arcs —
the cascade of §12.4–12.6 in miniature.  With the handover forced (the
harness's `anchor_handover` directive, `continuous`; the production election
picked S06000SIM and took the continuous branch for all three bands:
`handover_continuous` 21 vs 18, `handover_cold` unchanged at 3), the same slip
costs what Study 1's slip cost: 0.05–0.12 cycle on the station's other arcs,
millimetres on its clock and ZWD, 2 mm on the wire, no erase anywhere.  The one
channel left is the injected arc's own slant iono at S29 (−0.64 → −0.79 m; the
arc re-levels as a non-anchor with L5Q pinned and absorbs the change there) —
bounded to that LOS, and to be read against `anc_ctl_s29`.

**The production card that follows (build, default OFF, byte-identical):**
`network.cycle_slip.anchor_slip_handover` — when a slip response frees an arc
that currently anchors a (sat, signal) family, stage the same one-epoch
candidacy ban the injector uses, in `continuous` mode, so the election held
later in the SAME epoch hands the family to a successor at the successor's own
carried value BEFORE the freed arc's level is re-drawn.  Counters: families
handed over on a slip, continuous vs cold outcomes (a cold outcome means no
usable successor — count it, do not hide it), and PHASE_ANCHOR event rows with
the freed arc's identity (invariant #7).  Validation gates, in order: OFF
byte-identical to main; `anc_slip` with the knob ON reproduces `anc_slip_ho`'s
numbers; `doseall` with the knob ON on the quiet seed — the 35 anchor-arc slips
are handled, the 13 clock-datum-station slips are not, so the pre-registered
expectation is a LATER and WEAKER cascade, not none; then `doseall` on the
sum-to-zero seed (`evseed_quiet6h_s2z`, prepared) with the knob ON — the
full-cure test, PASS = ZWD rms diff < 0.15 m and T4 ≤ 50 over the leg.

### 12.8 Two more twins (2026-09-10, evening local)

**`dose_s30_not4`** (the clock-datum station's 13 slips with T4 starved, vs
the post-fix `null240_pf`): S30's ZWD still runs away — −0.55 m at +120..+139,
+1.70 m at +160..+179, +2.53 m at +180..+199, +1.18 m at +220..+239 (with T4:
10.2 m) — and its clock contrast −1.0 → −3.4 → +2.8 → +3.7 m; but the
BROADCAST is much weaker: wire 0.07 → 0.26 m (with T4: 1.36 → 1.76 m),
consumer +0.33 m (with T4 +0.22).  The two halves of the datum-station engine
are therefore separable: the LEAK (a slip response at the station whose clock
is the datum goes into its ZWD/clock — no free clock to absorb it) does not
need the outlier tier; the outlier tier's erases of that station's other arcs
against the displaced ZWD amplify the leak 4–10× and are what broadcasts it
into fifteen satellite clocks at once.  The cure for the leak is a datum with a
free clock at every station (`clock_datum_sum_to_zero`; seed
`evseed_quiet6h_s2z` converging); the anchor-slip handover card does not touch
this path.

**`anc_ctl_s29`** (S29000SIM / G26 L1C +40, G26 not anchored at S29) is an
INVALID control: the network does not use G26 at S29 (no detector row, no
residual row, every channel exactly 0.000 — the slip landed on an observation
outside the solution, as G16 did at S21 in `dose1_s21`).  The generator's
satellite pick must come from the station's SOLVED set (the null leg's
`network_station_residuals.csv`), not from truth visibility; a second control
on a solved, non-anchored satellite follows.

**`anc_ctl2_s29`** (S29000SIM / G31 L1C +40; G31 is SOLVED at S29 and not anchored
there) — the valid control.  MW(1,5) and IF-TD(1,2) both fire, so all three
bands are freed; the station's other arcs move 0.11 → 0.26 cycle (as-traced),
its clock < 3 mm, its ZWD < 1 mm, the emitted clock of G31 < 1 mm, the phase
bias < 5 mm, consumer −0.01 → −0.10 m, **no T4 row, clock-anchor decouples 1
(= null)**.  With `anc_slip` (G10, anchored at S29: 157 erases, 10.7 m clock,
−8.9 m wire) and `anc_slip_ho` (0 erases) this closes the identification: at
the same station, in the same epoch, with the same slip size and the same
detector responses, only the ANCHOR arc's slip breaks the network, and only
the missing handover makes it do so.

A second observation from this control, filed for the AR track, not the
cascade: the freed arc itself re-levels **4.0 / 5.2 / 5.4 cycles off** on
L1C / L2W / L5Q (ratios 1 : 1.29 : 1.34 = the pure-iono direction of §10.3),
i.e. a 0.8–1.1 m error on that LOS's slant iono (`ion_d` −0.81 → −1.08 m),
not decaying over the leg — ten times Study 1's all-band floor at S21000SIM
(0.45 cycle).  An all-band re-level draws the level from code and inherits the
station's own receiver-DCB / iono split, which is evidently ~1 m at S29 and
~0.1 m at S21; nothing downstream sees it (no residual, no erase, no wire
step) because the free per-LOS iono absorbs it exactly.  It costs the integers,
not the products.

### 12.9 Pre-registration — the datum half of the cure (written before any leg on the new seed was run)

Seed `runs/evseed_quiet6h_s2z` = the sim golden + `network.clock_datum_sum_to_zero: true`
(each constellation's clock common-mode fixed by Σδtˢ = 0 over its satellite
clocks — no station's receiver clock is pinned; the loader notes sticky_pivot
is inert under it, there being no pivot), converged 6 h on the quiet fixture,
snapshots every 60 epochs, `gnss_processor_dose.exe`.  The knob is
fingerprinted (`clkdatum:s2z=`), which is why it needs its own seed.  Legs
resume ep0665 (same tow as the pre-fix seed's, so `--from-leg` schedules carry
over unchanged); every leg is post-fix and graded against `null240_s2z`.

| leg | schedule | gate (PASS) | what a FAIL means |
|---|---|---|---|
| `dose_s30_s2z` | `dose_s30`'s 13 slips on S30000SIM | S30 ZWD diff ≤ 2 cm in every bin, T4 ≤ 2 over the leg, clock-anchor decouples ≤ null + 2 | the datum-station leak is not the pinned clock (§12.5's reading is wrong) |
| `doseall_s2z` | `doseall`'s 655 slips, anchor-slip handover OFF | report only; expected: the anchor cascade remains (35 anchor-arc slips), with the datum-station contribution gone | — |
| `doseall_s2z_knob` | the same 655 slips, `anchor_slip_handover: true` (worker's exe) | **the full cure**: ZWD rms diff < 0.15 m and median < 2 cm at +200..+239, T4 ≤ 50 over the leg, wire diff < 0.05 m, consumer diff within ±0.1 m, clock-anchor decouples ≤ null + 5 | a third gauge or a non-gauge path exists; go back to the single-event legs |

The comparison that matters is `doseall_s2z_knob` against `doseall` (§12.4):
3 099 → ≤ 50 erases, 1.83 m → < 0.15 m ZWD rms, 5.2 m → < 0.05 m wire.

### 12.10 The datum half is FALSIFIED — there is one gauge, and it is the phase-bias anchor (2026-09-10, evening local)

`dose_s30_s2z` (the datum station's 13 slips on the sum-to-zero seed, vs
`null240_s2z`): the cascade is reproduced to the epoch — T4 0 / 0 / 0 / 0 /
56 / 279 / 15 per bin (pinned datum: 0 / 0 / 0 / 0 / 45 / 294 / 12), S30's
ZWD **10.05 m** at +160..+179 (pinned: 10.2 m), its clock contrast −21 m, its
slant iono +5.5 m, wire 1.27 → 1.63 m (pinned 1.36 → 1.76), 351 erases
(pinned 352).  §12.5's reading ("no free clock to absorb the response") is
wrong, and §12.9's PASS gate for this leg was not met by a mile.  What the
per-slip census on BOTH seeds shows instead:

| S30000SIM slip | anchored at S30? | response | T4 erases in +0..+5 (pinned / s2z) | clock-anchor decouples in +0..+2 |
|---|---|---|---|---|
| +23 E10, +44 G24, +54 G29, +94 E24, +97 E18, +105 E09, +129 E18 | no (anchors elsewhere) | detector refloats | **0 / 0** each | 0 |
| +122 G25 L5Q +56 | no | **missed by every detector, caught post-fit by T4** | 9 / 11 | 3 / 4 |
| **+151 G20 L5Q −57** | **YES** | MW refloat | **31 / 30** (the 10-m ZWD runaway starts here) | 2 / 2 |
| +161 E34, +166 E09 | no | detector refloats | 5 / 3, 4 / 6 (inside the runaway) | 0–1 |
| **+186 G12 L1C +45** | **YES** | LLI + MW + IF-TD | **254 / 224** (the 15-satellite broadcast) | 16 / 14 |
| +190 G24 L2W | no | detector refloats | 102 / 102 (the storm already running) | 1 / 3 |

S30000SIM behaves exactly like S29000SIM and S31000SIM: it is an anchor
station (nine families: G05, G12, G20), and its slips on NON-anchored
satellites cost nothing while its two slips on anchored satellites started
and broadcast the cascade.  Being the clock datum adds nothing — the same
schedule on a datum with every receiver clock free reproduces every number.
The ONE remaining secondary path is the +122 kind: a slip the detectors miss
reaches the solve for an epoch before the outlier tier removes it (0.8 m into
that epoch's ZWD and products; a small burst) — a detector-coverage item, not
a gauge item.

**Consequence.**  The full-cure test does not need the sum-to-zero seed: it is
`doseall` on the ORIGINAL seed with `anchor_slip_handover` ON (the worker's
G3, `doseall_knob` vs `null240_knob`), graded against §12.9's last-row gates
(ZWD rms diff < 0.15 m and median < 2 cm at +200..+239, T4 ≤ 50 over the leg,
wire diff < 0.05 m, consumer diff within ±0.1 m, clock-anchor decouples ≤ null
+ 5), plus `dose_s30` with the knob ON as the single-station gate (expected:
the +151 and +186 events cost what `anc_slip_ho` cost, the +122 miss remains).
`clock_datum_sum_to_zero` stays where the ledger left it (a golden exception);
`runs/evseed_quiet6h_s2z` is kept as a fixture (its null differs from the
pinned-datum null: 135 vs 148 MW rows, 5 vs 2 decouples over 240 epochs).

### 12.11 The card on the datum station's schedule — PASS (2026-09-10, 23:15 local; worker's exe 204736F4…, matched null `null240_knob`)

`dose_s30_knob` = `dose_s30`'s 13 slips with `network.cycle_slip.anchor_slip_handover: true`:

| channel | without the card (`dose_s30`, §12.5) | **with the card** |
|---|---|---|
| S30 ZWD diff, worst bin | **10.2 m** (+160..+179) | **0.055 m** (+120..+139), 0.014 m at the end |
| S30 rx-clock contrast diff, worst bin | −21 m (s2z) / −3.4 m | −0.13 m |
| S30 slant-iono diff | +5.5 m | ≤ 0.005 m |
| T4 erases over the leg | 352 | **11** (10 of them in +120..+159 = the +122 slip every detector missed; null 1) |
| clock-anchor decouples | 27–30 | 5 (null 3) |
| wire diff (emitted clocks, de-meaned rms) | 1.36 → 1.76 m | ≤ 0.002 m |
| consumer 3D diff, last bin | +0.22 m | −0.008 m |
| anchor-slip handovers (event / null) | — | 19 / 16, 18 continuous + 1 cold fallback (run-end clause) |

The null itself carries 16 handovers: the quiet bed's false-alarm MW refloats
free anchor arcs too, and the card handles those the same way.  What remains
in the event leg is exactly the secondary path §12.10 named — a slip missed
by every detector reaches the solve for one epoch — worth its own detector
item, not a gauge item.  Pre-registered gate (§12.10 last paragraph) met.

### 12.12 The card's gates (worker report, branch `worktree-anchorslip` @ ec26de1; 2026-09-10 ~23:40 local)

Built: `network.cycle_slip.anchor_slip_handover` (default OFF, not fingerprinted;
disarmed unless the prioritized anchor election runs), the staging seam
`networkStageAnchorSlipHandovers` in `NetworkPhaseDatum.cpp` (reads
`relevel.free_ambs` and `carry_slipped_ambs`, one directive per (sat, signal),
into a new `NetworkInjectState::anchor_now_slip`; `anchorFor` checks the
injector's own directives first), the call sited immediately before
`networkElectPhaseAnchors`, counters `n_anc_slip_{staged,handed,continuous,
cold,no_successor,inert,epochs}`, `PHASE_ANCHOR/anchor_slip_handover` rows
(station = retired incumbent, detail `to=<successor>;branch=…`), the armed-only
run-end clause and manifest key, 12 tests in `tests/test_anchor_slip_handover.cpp`;
suite 1816 pass / 0 fail / 17 skip.

| gate | result |
|---|---|
| G1 OFF byte-identity (`null_anchorslip_off` vs `null_fixchk`) | **PASS** — identical sha256 |
| G2 `anc_slip_knob` vs `anc_slip` / `anc_slip_ho` | **PASS** — T4 0 (was 157), decouples 1 (was 13), other arcs 0.146 / 0.058 cyc at +1 / +8 (was 13.1 / 11.7), clock −0.015 / −0.031 m (was 10.7 / 8.8), emitted-clock peak 0.0027 m (was −9.7).  Two families handed over, not three: the IF-TD response freed L1C;L2W only, so G10 L5Q's anchor arc was never freed and the card correctly staged nothing for it — the pre-registration over-specified the count. |
| G3 `doseall_knob` vs `null240_knob` (655 slips, all fired) | T4 **3 099 → 200** (onset +80 instead of +11; 0 / 0 / 104 / 42 / 21 / 32 / 1 per bin), clock-anchor decouples **258 → 44**, wire **5.96 → 0.04 m**, consumer **+1.46 → −0.13 m**, slant-iono rms 2.43 → 0.09 m, untouched-arc level drift 0.83 → −0.05 cyc; anchor-slip handovers 120 (107 continuous / 13 cold fallbacks); detector populations unchanged (MW 560, IF-TD 350, LLI 312, refsat 78).  ZWD rms **1.76 → 0.94 m** — but see below. |

**The ZWD rms is ONE station.**  At +200..+239 the median |ZWD diff| over the 60
stations is **5.3 mm**, 59 stations are within 42 mm, and S36000SIM alone
reads **+7.49 m** with **124 of the 200** erases; 7.49 / √60 = 0.97 m is the
whole rms.  S36's cascade starts at +89 (seven of its arcs erased in one
epoch) from a slip that drew NO response row at all — one of **15 slips in
655 (2.3 %) that no detector caught within two epochs** (post-fit T4 caught 0
of them on this leg).  A slip that stays in the solve is the §12.10 secondary
path, and one is enough to wreck a station, though on this leg it did not
propagate (the other 76 erases are scattered; the wire and the consumer are
flat).  The 13 cold fallbacks are NOT where the damage is (S60 3, S30 1, S31 1
— all within 21 mm).  So the anchor-slip handover removes the gauge engine;
what is left is detector coverage, a separate card.

**Knob-on nulls differ from knob-off nulls** by construction: the warm-resume
refloat burst frees anchor arcs too, and the card hands them over (16 families
at +1 on this seed), so every knob-on leg is graded against a knob-on null.

**Collateral (worker):** `scripts/vs2026_build.ps1` hard-codes the main project
path — from a worktree it reconfigured and rebuilt MAIN's Release tree (same
source as HEAD, no harm) and left the worktree unbuilt; the worker used a
repointed copy.  Worth a `-Project` switch.

**The 15 "missed" slips, resolved (from the traces, not the sky).**  Fourteen
fell on (station, satellite) pairs that were NOT in the solved set at the slip
epoch (no residual row within ±1 epoch, no `slip_raw` row) — G16 at eight
stations, E27 at three, G26, E30: the satellites are solved elsewhere and at
other times, but at that station and epoch the observation was outside the
solution, so the injection was inert, not missed.  (Generator item: pick
(station, sat, epoch) from the solved set — the null leg's residual and anchor
traces — not from truth visibility; `dose1_s21`'s G16 was the same.)  The ONE
real miss is S36000SIM / G04 L5Q, −99 cycles at 20.5°, no LLI — and **S36000SIM
was G04's anchor for all three bands at that epoch.**  Its `slip_raw` row
carries the L1C;L2W pair only, so no MW pair covered L5 for that arc at that
moment; IF-TD's pair is L1C;L2W (blind to L5); nothing freed the arc, so the
card had nothing to hand over; the pinned anchor row kept −25 m of misfit in
the solve, the station frame absorbed it, and at +89 the outlier tier erased
seven of S36's INNOCENT arcs (pinned anchor arcs are outside the leveling RAIM
pool, so the tier can never blame the anchor itself) — the 7.5 m runaway.
The datum station's +122 miss (§12.10) had the same shape: G25 L5Q at 20°, no
LLI, caught only post-fit.  Two follow-ups, both precise: (a) DETECTOR
COVERAGE — the L5 MW pair must exist for every triple-frequency arc the moment
it is solved (why was only L1C;L2W in the feed for a 20° arc?); a harness card
that injects L5Q slips at 15–25° on solved arcs and counts detections by tier;
(b) ATTRIBUTION — an undetected slip on a pinned anchor arc is structurally
invisible to the outlier tier and gets charged to the station's other arcs;
when a station-wide burst lands on a station that anchors families, test (or
hand over) the anchor arcs before erasing anything else.

### 12.13 STORM-BED VALIDATION — the plateau is gone (2026-09-10, 23:30 local; `runs/golden_sim_campaign_zwdblk_ancho`)

The user directive reserves the day-long bed for the final validation of a fix
whose event-level behaviour is understood.  One arm: the in-block control
config plus `network.cycle_slip.anchor_slip_handover: true`, cold start on the
slipped campaign fixture, 6 h × 60 stations, exe 204736F4… (the merged card),
1006 s wall, 720/720 emitted.  `scripts/simnet_storm_grade.py`:

| gate quantity | control (`golden_sim_campaign_zwdblk`) | **with the card** | gate |
|---|---|---|---|
| settled ZWD truth rms | 1.188 m | **0.046 m** | ≤ 0.5 **PASS** (the quiet golden's ZWD reads 0.0072 m; [corrected 2026-09-11: the 0.076 quoted here was the quiet bed's CONSUMER rms]) |
| ZWD hourly rms | 1.98 / 1.31 / 0.96 / 0.58 / 1.05 / 1.67 | 0.21 / 0.02 / 0.01 / 0.04 / 0.07 / 0.02 | — |
| per-station ZWD span p50 / stations > 1 m | 0.207 m / 15 | **0.031 m / 0** | — |
| consumer settled p50 / rms | 1.085 / 2.389 m | **0.278 / 0.761 m** | p50 ≤ 0.6 **PASS** |
| consumer hourly rms | 1.59 / 2.19 / 1.56 / 1.03 / 2.55 / 3.09 | 1.12 / 0.34 / 0.33 / 0.22 / 1.04 / 0.78 | — |
| T4 attribution events / erased-arc ratio | 9 871 / 2.17 | **306 / 1.34** | must fall: **PASS** (−97 %) |
| T4 ambient per-band DD error | 1.6 cyc | **0.26 cyc** | — |
| ref-free WL / EWL / L1C at own integer | 86.2 / 99.2 / 44.0 % | **98.1 / 99.8 / 81.1 %** | WL ≥ 83 **PASS** |
| CLK-R drained / max A_c | 41.5 / 25.6 m | 4.2 / 0.3 m | — |
| detector events | 4 545 | 4 545 | unchanged (same slips caught) |
| anchor-slip handovers | — | 261 (211 continuous / 50 cold fallback) | — |

Every pre-registered gate passes, most by an order of magnitude.  The ZWD sink
that opened this campaign (§0 of `project_zwd_sink_census`: 1.2 m in-block,
2.1 m before) is closed to 4.6 cm — below the quiet bed's own 7.6 cm — and the
wide-lane integers, which the storm bed had never held above 86 %, sit at 98 %.
What remains: the consumer's hour-5/6 bump (1.04 / 0.78 m rms against 0.2–0.3
in hours 2–4) and its 0.28 m median against the quiet bed's 0.05 — the §12.12
detector-coverage item (an undetected L5 slip on an anchor arc wrecks a
station) and the 50 cold fallbacks are the two named candidates; both are
harness questions, not storm questions.

**Ledger:** `network.cycle_slip.anchor_slip_handover` → **RECOMMEND_ON**, regime
`on(phase_leveling) and on(phase_datum_prioritized)`, pinned in the sim golden
(`golden_manifest.json`; the field golden is out of regime on its
carry_invariant vehicle — its leveling posture inherits the pin when it lands).
`make_golden_config.py --verify` PASS (51 armed on the sim golden).


