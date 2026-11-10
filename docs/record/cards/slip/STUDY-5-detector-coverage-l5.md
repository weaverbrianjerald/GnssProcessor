# Study 5: the unflagged GPS L5 slip and `mw_evidence_refloat`

**STATUS**

- **id:** Study 5
- **topic:** slip
- **status:** closed
- **summary:** The MW detector caught all five missed unflagged GPS L5 slips but a repair-quality gate dropped them; `mw_evidence_refloat` admits them and passes every storm-bed bar (ZWD 0.046 → 0.0071 m): RECOMMEND_ON.
- **dates in the text:** 2026-09-11
- **knobs / posture:** `network.cycle_slip.mw_evidence_refloat` (`mw_evidence_ratio_min` 3.0, `mw_evidence_min_arc_epochs` 10), `network.cycle_slip.mw_unknown_integer_refloat` (the name proposed under H1; the built knob is `mw_evidence_refloat`), `network.phase_leveling`, `network.cycle_slip.mw_detect`
- **hashes named in the text** (commit / exe / byte-cert): `9935aec`, `0550b58`, `2490087`, `8A9BEBA1`
- **run dirs named in the text:** `runs/evleg_doseall`, `runs/evleg_doseall_knob`, `runs/evleg_l5miss_s36`, `runs/evleg_s36only_mwlog`, `runs/evleg_null_main_ancho`, `runs/golden_sim_campaign_zwdblk_mwevid`, `runs/golden_sim_tri_w6_full`, `runs/golden_sim_quiet24_bothcards`
- **source:** `docs/archive/SPEC_event_harness.md` §13, §13.1 to §13.6

**Outcome.** The question: why the L5 slip that wrecked a station in [Study 4](STUDY-4-slip-dose.md) drew no detector response. Only unflagged GPS L5 slips were missed, about 4 % of that class; an L5 MW lane flagged each one, but PASS A applied the 0.15-cycle repair gate to a refloat that never uses the integer. With the knob the dose's T4 falls 200 → 0 and the storm-bed consumer p50 0.278 → 0.174 m; on a pinned anchor arc the refloat alone is a solve no-op and the anchor-slip handover remains the cure. A field pin followed on 2026-09-12 ([field harness](../harness/FIELD-HARNESS.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 13. Study 5 — detector coverage: the unflagged GPS L5 slip (2026-09-11)

### 13.1 The question, and what was established before any new leg

§12.12 left one residual path: a slip that no detector catches, landing on a
phase-bias anchor arc, still wrecks its station (S36 to 7.5 m on
`doseall_knob`), because nothing frees the arc, the handover card has nothing
to hand over, and the outlier tier cannot indict a pinned row.  Question: WHY
does that slip escape, how often, and what production change closes it?

**Census first** (`slip_census.py`, scratchpad; every injected slip of
`runs/evleg_doseall` and `runs/evleg_doseall_knob`, 655 each, identical
outcomes on both legs).  Of 1 282 non-inert slips, every band — with or
without a receiver flag — is caught within +0..+2 epochs by MW / LLI / IF-TD,
EXCEPT unflagged L5:

| injected band, flag | caught late or never | of |
|---|---|---|
| L1 / L2 / L7 (Galileo E5b), flag or none | 0 | 852 |
| L5, flag set | 0 | 212 |
| **L5, no flag** | **10 (= the same 5 events on both legs)** | 218 |

The five: S36/G04 −99 cyc at 20.4° (no detector, caught by NOTHING — the
§12.12 wreck), S25/G05 −40 at 61.6°, S15/G20 +110 at 30.9°, S30/G25 +56 at
20.0° (the §12.10 datum-station miss), S33/G19 −40 at 18.1° — the last four
caught post-fit by T4 in the same epoch.  All GPS; Galileo E5a slips have 0
misses (the E5a/E5b pair, λ 9.8 m, catches them).  **Elevation is NOT the
discriminator** (18–62°), so this is not the low-elevation noise story the
§12.12 wording implied.

What catches an unflagged L5 slip when it IS caught: MW L2W;L5Q (80) and MW
L1C;L5Q (42) on GPS, MW L7Q;L5Q (83) on Galileo, IF-TD L1C;L5Q (3 — Galileo
E1/E5a).  Two structural facts from the code:

- **IF-TD forms ONE pair per satellite: the first two distinct bands**
  (`IfPhaseSlipDetector.cpp` ~250) — L1C;L2W on GPS, L1C;L5Q on Galileo.  It
  is blind to a GPS L5-only slip by construction.  (§12.12's "the L5 MW pair
  did not exist" was inferred from `network_slip_raw.csv`, which prints the
  first two bands by construction — the MW detector DOES form (1,5) and (2,5);
  the census shows them firing at S36 on other satellites.)
- **The MW detector reports a slip in two grades and the network consumes one.**
  On the network path (uncombined branch; no satellite MW bias on the sim, so
  no integer locking) `processMwSample` returns (integer, integer_known,
  fraction).  PASS A (`ApplicationNetworkAssembly.cpp:1363`) admits a refloat
  candidate only if `slip_wl_known && slip_wl_cycles != 0 && slip_frac <=
  turbo_edit_frac_max` (0.15).  An UNKNOWN-integer detection (the init-frac
  "suspicious" path, a confirmed TD step, an integer not resolvable from one
  sample) or a known integer with fraction > 0.15 is **dropped with no
  counter and no event row** — invariants #4 and #7 both violated.

### 13.2 Hypotheses — frozen before the reproduction legs are read

Two legs from the quiet seed ep0665, main's `gnss_processor_ancho2.exe`
(HEAD 9935aec), the MW detector's logger armed (it is OFF by default —
`logging.enable_mw_slip_detector`):

- `runs/evleg_l5miss_s36` — the ONE slip (S36000SIM / G04 / L5Q / −99, no
  flag) at +89 (tow 281850 = the dose leg's epoch), 100 epochs, log at debug.
- `runs/evleg_s36only_mwlog` — the dose leg's S36 subset (its 11 slips,
  identical events, 240 ep), log at info: station-local history reproduced.

| hypothesis | prediction in the logs | production change if true |
|---|---|---|
| **H1 dropped detection** — MW fired on (2,5) and/or (1,5) at +89 but returned integer-unknown or fraction > 0.15, and PASS A discarded it | a "MW slip detected for G04 … locked=no, delta_wl=n/a" / "MW init suspicious … unknown integer" line at the slip epoch; NO T0 event row | consume unknown-integer / loose-fraction MW detections as a REFLOAT of the pair's two bands (the network's reset-over-repair), knob `network.cycle_slip.mw_unknown_integer_refloat`, default OFF, counter + event row for every dropped detection regardless of the knob |
| **H2 silent detector** — no G04 detection line at all: the pair's samples were absent (NaN MW, no C5 code) or the arc was inside a warm-up that initialised ON the slipped value | `[DIAG MW skip]` lines for G04, or an arc restart at / just before +89 | depends on which: the warm-up hole needs an init-frac test on the (2,5) lane (λ 5.86 m makes a fresh sample's fraction informative), the pair hole needs the pair |
| H3 below threshold | a G04 line with dev < thresh | impossible at 74 m / 580 m for 99 cycles; listed so the reader sees it was considered |
| H4 state-dependent | the miss does NOT reproduce in the single-event leg but DOES in the S36 subset (station history matters), or in neither (network-level reset) | then the dose leg's own log is the instrument |

### 13.3 Pre-registered gates for the production change (whatever H is true)

| gate | leg | pass |
|---|---|---|
| G1 | null on the card exe, knob OFF | byte-identical to `runs/evleg_null_main_ancho` |
| G2 | `l5miss_s36` + knob ON, vs the matched knob-OFF leg | detection row at +0/+1 naming an L5Q-bearing pair; PHASE_ANCHOR handover row for G04 (the freed anchor); T4 erases at S36 in +0..+10 = 0 (the dose leg had 7 at +0, 6 at +1); S36 ZWD / clock / other-arc levels within 2× the null floor (`event_frame_grade.py`); consumer within the null floor |
| G3 | `doseall_knob` events + knob ON (matched knob-ON null) | S36 no longer wrecked: max per-station ZWD rms ≤ 0.2 m (was 7.5); T4 ≤ 200; wire ≤ 0.05 m; the four T4-only slips become detector catches |
| G4 | quiet 240-ep null + knob ON vs knob OFF | false-alarm price: new refloats ≤ 20 network-wide (≤ 1 % of the campaign slip rate); ZWD / consumer diff within the null-vs-null floor |
| G5 | storm bed `_golden_sim_campaign_zwdblk_ancho` + knob (FINAL) | ZWD ≤ 0.05 m, consumer p50 ≤ 0.25 (was 0.278), T4 ≤ 306, WL ≥ 98 %; hour-5/6 consumer rms must not rise |

The IF-TD second pair (a (1,5) IF pair per GPS satellite) is a SEPARATE item:
it would add a detector to the L5 lane, but the census says the MW (2,5)
lane already covers 96 % and the question is what happens to the last 4 %.

### 13.4 RESULT — H1, with the exact gate named (2026-09-11; legs `l5miss_s36`, `s36only_mwlog`, `l5miss4_mwlog`, main exe `gnss_processor_ancho2.exe`)

**The miss reproduces in the single-event leg** (S36/G04/L5Q −99 at +89: the
outlier tier erases 7 innocent arcs at S36 at +0 and 6 more at +1, G04 stays
anchored at S36 on all three bands, no detector row — exactly the dose leg).
The detector's own log (rotating sink; older lines are in
`mw_slip_detector.N.log`) shows what happened at the slip epoch:

| lane | λ | arc age | \|dev\| | threshold | integer | rounding residual | PASS A |
|---|---|---|---|---|---|---|---|
| L2W/L5Q (bias-free) | 5.861 m | 89 | 581.3 m = 99.2 WL | 7.42 m | **99, known** | **0.183 cyc** (1.07 m) | **dropped: 0.183 > `turbo_edit_frac_max` 0.15** |
| L1C/L5Q (bias-free) | 0.751 m | 87 | 75.4 m = 100.3 WL | 8.46 m | unknown (0.3 cyc off) | — | dropped: integer unknown |
| L1C/L2W (BIA, between-satellite) | 0.862 m | — | no jump (L5-only slip) | — | — | — | — |

Both L5 lanes flagged the slip at 8.9× and 78× their thresholds.  The network
consumed neither: the refloat path re-uses the T0 REPAIR gate — "trust the
integer only if its rounding residual is ≤ 0.15 cycles" — and on the 5.86 m
extra-wide lane 0.15 cycles is 0.88 m, below the lane's own 1σ code noise at
20° (σ = 0.93 m in the log).  A repair-quality gate on a detection whose only
consumer is a refloat, which never uses the integer.

**The other four misses have the same shape** (`l5miss4_mwlog`, the dose leg's
S25/S15/S30/S33 subset, identical events):

| slip | L2W/L5Q lane | L1C/L5Q lane |
|---|---|---|
| S25/G05 −40 (61.6°) | 39.8 WL, integer 40 known, residual ≈ 0.2 → dropped | 39.3 WL, unknown |
| S15/G20 +110 (30.9°) | 109.8 WL, −110 known, ≈ 0.2 → dropped | 109.8 WL, −110 known, ≈ 0.2 → dropped |
| S30/G25 +56 (20.0°) | 56.2 WL, −56 known, ≈ 0.2 → dropped | 55.5 WL, unknown |
| S33/G19 −40 (18.1°) | 40.2 WL, 40 known, ≈ 0.2 → dropped | 39.5 WL, unknown |

So the 4 % is the tail of the rounding-residual distribution on the L5 lanes
(σ_frac ≈ 0.15–0.2 cycles at low elevation on (2,5); larger on (1,5)), not a
missing pair, not elevation, not the detector.  The detector caught 5/5.

**What a refloat gate may cost — the false-alarm census** (`mw_detect_census.py`
over `s36only_mwlog`: 240 ep, 60 stations, S36's 15 real slips, 719 "MW slip
detected" lines):

| population | count | reaches the network today |
|---|---|---|
| warm-up (arc age ≤ 2), all lanes | 680 | 70 admitted (the +1 resume burst: ~80 refloat rows at 279210); the rest dropped |
| arc age 3–9 | 2 | 0 |
| mature (age ≥ 10), bias-free lanes, ratio ≥ 3 | 25 | **every one is one of S36's real slips** — zero false alarms in 59 quiet stations × 2 h |
| mature, BIA lane (L1C/L2W between-satellite), ratio 2.4–7.5 | 12 | 8 of them are ONE event: eight satellites at one station jump by the same 37–40 WL at the same epoch — a between-satellite pivot artifact (the S-transform on a pivot change), dropped today |

Steady-state refloats the network actually performs at the 59 quiet stations
after the burst: **3 in two hours**.  A gate of the form *mature arc AND
evidence ≥ 3× threshold AND bias-free lane* admits all five misses and nothing
else on this bed; the bias-free restriction is what keeps the pivot artifact
out (a detector-side defect worth its own line: that event also ERASES those
eight arcs' MW histories, so the station's L1/L2 lane is blind for the next
few epochs).

**Production card (building, default OFF): `network.cycle_slip.mw_evidence_refloat`**
— the detector exports, with every detection, its evidence ratio (|dev|/thresh),
the arc's age, and the lane's bias class; PASS A admits a refloat candidate on
the legacy rule OR (knob AND bias-free lane AND age ≥ `mw_evidence_min_arc_epochs`
(10) AND ratio ≥ `mw_evidence_ratio_min` (3)); the candidate carries
`integer_known` so the bridge path never consumes a zero integer; dropped
detections are COUNTED always (loose / unknown / would-admit) — invariant #4.
Gates §13.3 G1–G5.  Not built: the IF-TD second pair, and the warm-up
false-alarm limit cycle (a flagged arc is erased and re-initialised on the
next sample; a real slip landing on that sample is absorbed as the new level —
none of the five misses was this, but it is a real hole at low elevation).

### 13.5 The card's gates (worker build 0550b58 on its own worktree, merged to main as 2490087; manager re-grade; 2026-09-11 morning)

`network.cycle_slip.mw_evidence_refloat` (+ `mw_evidence_ratio_min` 3.0,
`mw_evidence_min_arc_epochs` 10), default OFF.  `MwSlipResult` now exports
`evidence_ratio`, `arc_epochs`, `bia_corrected`; PASS A admits on the legacy
rule OR (knob ∧ bias-free lane ∧ age ≥ 10 ∧ ratio ≥ 3); the candidate carries
`integer_known` (the bridge intercepts are skipped for an unknown integer);
rows append `;src=evidence;ratio=…;n=…;int=known|unknown`; run-end clause
always: `MW detect-only admission: legacy N, evidence N [knob on|off;
would-admit N]; dropped loose N / unknown N` (10 new tests, suite 1826 / 0 / 17
re-run by the manager from the worker's Debug build).

| gate | leg(s) | result |
|---|---|---|
| G1 byte-identity OFF | `null_mwevid_off` vs `null_main_ancho` (60 ep) | **PASS** — products / solution / events / ZWD SHA-256 all MATCH (re-hashed by the manager: F3F37BC3…, 30041F28…, 690927F6…, DDC169A2…) |
| G2 single event | 2×2 on S36/G04/L5Q −99 at +89: `l5miss_s36` (both off), `_on` (knob), `_anc` (handover), `_anc_evid` (both) | knob alone: the detection row appears (`L1C;L5Q;src=evidence;ratio=8.9;n=87;int=unknown`; the drain logs "refloated 1 wide-lane slip [S36000SIM/G04]") and the solve does not move — T4 22, clock −17.8 m, ZWD +14.3 m, iono +21.3 m IDENTICAL to both-off; handover alone: identical wreck (nothing freed the arc); **both: T4 0, clock +0.021 m, ZWD −0.013 m, consumer +0.0008 m** (3 families handed over continuously at 281850).  The knob supplies the detection; the handover is the cure on an anchor arc. |
| G3 655-slip dose, knob ON (+ handover) | `doseall_evid` vs `null240_evid`; control `doseall_knob` vs `null240_knob` | **PASS** — T4 200 → **0**; the five formerly-missed slips 0/5 → **5/5** detected at +0 (all `src=evidence`); max per-station ZWD diff 10.6 m (S36) → **0.019 m** (the worker's "0.296" was S36's CLOCK column; ZWD ≤ 0.02 m at every station); wire de-meaned rms 0.041 → **0.009 m**; consumer peak 0.46 → 0.34 m, last bin −0.13 → −0.09 m; MW rows on injected arcs 560 → 705; handovers 120 → 138 (120 continuous / 18 cold); S36 residual after its all-band free: clock +0.30 m, iono +0.53 m at +240 (the §9 all-band re-level of a 20° arc, now a non-anchor) |
| G4 false-alarm price | `null240_evid` vs `null240_knob` | **PASS** — 0 evidence admissions in 240 ep × 60 sta; all four artifacts SHA-256 identical armed vs unarmed; run-end `legacy 286, evidence 0; dropped loose 196 / unknown 1338` (the population the rule declines) |
| G5 storm bed | `runs/golden_sim_campaign_zwdblk_mwevid` (= the §12.13 arm + the knob) | see §13.6 |

**Why the refloat alone is inert on an anchor arc — and a sharper statement of
the §12 engine.**  Two control legs with the handover OFF, injector
`arc_erase mode=free_epoch` on the same pinned anchor arc (S36/G04, no slip):
all three bands freed (`ambs=3;latch=free_ambs`) and two bands freed
(`L1C;L2W`) both read **exactly 0.0000 on every channel** against the null
(clock, ZWD, iono, emitted C and P, consumer) — a freed anchor arc is NOT
re-levelled while it holds the pin; the pin keeps its level.  So a slip on an
anchor arc is a **stale pinned row carrying the whole slip as a misfit** — the
solve distributes −25 m of misfit into the station frame (clock −17.8 m, ZWD
+14 m, iono +21 m at S36) and into the satellite's products, and the outlier
tier, which cannot see a pinned row, erases the station's innocent arcs.
§12.7's phrase "the family is re-realized from the freed arc" is imprecise:
nothing is re-drawn; the pin simply stays on an arc whose observations moved.
The cure is unchanged (move the pin off the arc in the slip epoch), but the
division of labour is now exact: on a NON-anchor arc the detector's refloat
alone repairs the slip (the four T4-only cases in G3); on an ANCHOR arc the
refloat's only job is to put the ambiguity into `free_ambs` so the handover
fires — which is what the knob does for the 4 % the repair gate had hidden.

### 13.6 STORM-BED VALIDATION — PASS on every bar (2026-09-11; `runs/golden_sim_campaign_zwdblk_mwevid` = the §12.13 arm + `mw_evidence_refloat`, worker exe 8A9BEBA1…, 1130 s, 720/720; graded with `scripts/simnet_storm_grade.py`)

| gate quantity | §12.13 arm (anchor handover only) | **+ mw_evidence_refloat** | gate |
|---|---|---|---|
| settled ZWD truth rms | 0.046 m | **0.0071 m** | ≤ 0.05 **PASS** (= the quiet golden's own ZWD floor, 0.0072 m) |
| ZWD hourly rms | 0.21 / 0.02 / 0.01 / 0.04 / 0.07 / 0.02 | 0.17 / 0.01 / 0.007 / 0.006 / 0.007 / 0.008 | — |
| per-station ZWD span p50 / stations > 1 m | 0.031 m / 0 | 0.021 m / 0 | — |
| consumer settled p50 / rms | 0.278 / 0.761 m | **0.174 / 0.365 m** | p50 ≤ 0.25 **PASS** |
| consumer hourly rms | 1.12 / 0.34 / 0.33 / 0.22 / 1.04 / 0.78 | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | hours 5–6 must not rise: **halved** |
| T4 attribution events / erased-arc ratio | 306 / 1.34 | **78 / 1.51** | ≤ 306 **PASS** (−75 %) |
| T4 ambient per-band DD error | 0.26 cyc | 0.229 cyc | — |
| ref-free WL / EWL / L1C at own integer | 98.1 / 99.8 / 81.1 % | **98.9 / 100.0 / 87.1 %** | WL ≥ 98 **PASS** |
| CLK-R drained / max A_c | 4.2 / 0.3 m | 3.62 / 0.28 m | — |
| detector events | 4 545 | 5 151 | +606 = the admitted detections' rows |
| MW admissions (run-end) | legacy only | legacy 4 570, **evidence 2 278** (rows 1 020: 348 integer-known, 672 unknown; ~170 per hour, flat) | dropped loose 1 010 / unknown 5 660 remain |
| anchor-slip handovers | 261 (211 continuous / 50 cold) | 305 (237 / 68) | cold share 19 → 22 % |

The slipped bed now sits at 7 mm of tropospheric error — EQUAL to the quiet
bed's own floor.  [Corrected 2026-09-11 after the quiet re-run: the "76 mm"
quoted in the first draft of this paragraph was the quiet bed's CONSUMER 3D
rms (0.0762 m, `runs/golden_sim_tri_w6_full`), not its ZWD; the quiet bed's
ZWD truth rms is 0.0072 m.  Quiet 24 h with both slip cards on main's build
(`runs/golden_sim_quiet24_bothcards`, the regenerated golden verbatim, 2880
epochs): ZWD 0.0073 m, hourly 0.007 flat; consumer settled h12–24 rms
0.0895 / p50 0.0593 / UP rms 0.0768 m against the in-block quiet arm's
0.0875 / 0.0525 / 0.0759 (`golden_sim_zwdblk_quiet`, an older golden); T4 66,
6 handovers (all continuous), 0 evidence admissions in 24 h.  The matched
no-card twin of today's golden is owed for the exact neutrality delta.]  On the storm bed roughly one MW admission in three now comes
through the evidence path: the population the repair gate had been hiding is
not a tail at storm density.

**Ledger:** `network.cycle_slip.mw_evidence_refloat` → **RECOMMEND_ON**, regime
`on("network.phase_leveling") and on("network.cycle_slip.mw_detect")`, pinned
in the sim golden; the field golden is out of regime (carry_invariant vehicle,
no phase leveling).  Open after this card: (a) two MW pairs firing on the same
satellite in the same epoch free all three bands although only their COMMON
band slipped — a "lane-intersection" scope (free only the shared band) is the
honest single-band response and is the next detector-side card; (b) the 68
cold fallbacks; (c) the IF-TD second pair; (d) the warm-up erase-and-re-init
limit cycle (priced at ~8e-4 per lane-epoch, §13.4); (e) the BSSD L1/L2
pivot-change artifact (8 arcs' MW histories erased at one station in one
epoch).

