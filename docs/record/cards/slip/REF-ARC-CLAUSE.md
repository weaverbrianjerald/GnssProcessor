# REF-ARC: a chain's reference-satellite pair is untested and cannot exonerate

**STATUS**

- **id:** REF-ARC
- **topic:** slip
- **status:** closed
- **summary:** `common_band_ref_arc` PROMOTED (RECOMMEND_ON with the common-band scope): a pair on a chain's reference arc is UNTESTED and refused as an exonerator, closing the GOPE/G24 innocent-band trap.
- **dates in the text:** 2026-09-12 → 2026-09-13
- **knobs / posture:** `network.cycle_slip.common_band_ref_arc`
- **hashes named in the text** (commit / exe / byte-cert): `c8096ee`, `904b89e`, `dcf04dd`, `ee33550`
- **run dirs named in the text:** `runs/evseed_field6h`, `runs/evleg_cbref_`, `runs/golden_sim_campaign_zwdblk_sts`, `runs/golden_sim_campaign_zwdblk_refarc`
- **source:** `docs/archive/SPEC_event_harness.md`: 22, 22.1, 22.2, 22.3, 22.4, 22.5

**Outcome.** The common-band scope's IF-TD veto cannot fire on a chain's reference arc (its DD is exactly 0), so an equal-count L1C+L5X slip at G24 narrowed to the innocent band. With the clause, B1 to B3 PASS on the field seed and only 4 of 106 narrowings are forfeited (bar 15 %), all injected. B4 PASS: the sim storm bed matches the control to the digit on every bar, with 29 ref-arc refusals. Pinned in the then-golden `golden_sim_tri_w6`; the field pin of the scope, pairs, clause and reference arm together later failed FP-2's 6 h twin on response cost ([REF-FAN](REF-FAN.md)). Both current goldens arm it (`config/golden/golden_{field,sim}_refound.json`).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 22. THE REFERENCE-ARC CLAUSE for the common-band scope (`network.cycle_slip.common_band_ref_arc`) — the §20.1 fix stub, pre-registered (2026-09-12 ~08:00 local, manager; before any leg is read)

### 22.1 The defect, restated from the code

The scope's IF-TD veto (`NetworkCommonBandScope.cpp:138–144`) refuses a silent MW pair as an
exonerator only when the pooled IF-TD preview FLAGGED that pair.  The preview (`:277–356`) forms
`flagged = |dd − med| > thr` per sample, and a sample on the chain's REFERENCE satellite has
`dd ≡ 0` by construction (`IfPhaseSlipDetector.hpp:195–206`: `is_reference`) — so on a reference
arc the veto is structurally unavailable and the pair lands in `iftd_silent` instead.  The
per-station reference GATE (card R) tests the FIRST chain's reference through a different statistic,
but its decision never reaches the preview, and the additional chains under `if_phase_td_pairs =
all` have no reference test at all (§14.10.4).  §20.1 measured the consequence on real data: an
equal-count L1C+L5X slip on GOPE/G24 (the L5 chains' reference) narrowed to the innocent band under
`first` AND `all`, while on G25 (not the reference) `all` vetoed at 29× the threshold.  Network-wide
the dead zone is exactly one arc per station per additional chain — the highest L5-capable
satellite in 80.6 % of epochs.

### 22.2 The clause (built default-OFF behind `network.cycle_slip.common_band_ref_arc`)

A pair whose IF-TD sample at (station, satellite) is the chain's reference is **UNTESTED for the
verdict**: the preview records it as such (`CommonBandEvidence::iftd_untested`), the rule refuses
the corresponding silent MW pair as an exonerator — the same shape as `immature` and `lli_shadow`
(counted `iftd_ref_arc`, basis token `iftd(<pair>)=ref_arc`, never listed as `silent`) — and the
verdict falls through exactly as today when nothing else exonerates (`no_exoneration ⇒ all-band`).
No detector change, no re-opening of the §14.10.4 flood, no new reference election.  Applied to
EVERY chain's reference (first chain included): the first chain's reference gate is a separate
instrument whose decision the preview does not see, so treating its arc as tested would be an
assumption, not a measurement — wiring the gate's fire into the preview is a possible second
increment, not this card.  Byte-identical when the knob is off (one branch in the rule).  Also
(trace-only, products byte-identical): `network_if_slip.csv` gains an `is_ref` column so a
reference arc can be identified by the detector's own flag instead of by `abs_dev_m == 0`.

**Named cost, accepted in advance.**  On a reference arc of the L5 chains, a single-band L1 (or L2)
slip loses its narrowing: its only silent pair is (2,5) (or (1,5)), now untested ⇒ all-band = the
pre-card response.  An L5 slip there still narrows (its silent pair is (1,2), tested).  On an arc
that is the reference of all three chains (the (1,2) reference tracks L5 in 46.7 % of
station-epochs) nothing narrows.  The census in 22.3 (B3) prices this from the seed's own traces.

### 22.3 Pre-registered legs and gates

Legs on `runs/evseed_field6h` ep0604 (GOPE00CZE, event at +10, 60 epochs; the resume posture is
whatever §21 Phase 1 decides — `state_snapshot_carry: false` if L1 passes — identical across every
leg of this card), scope ON + `if_phase_td_pairs: all` (the §20.1 `sp` extra), knob ON vs OFF:

| leg | site | expected ON | expected OFF |
|---|---|---|---|
| null on/off | — | byte-identical to OFF unless a natural slip lands on a reference arc; every differing event row must carry `ref_arc` | — |
| E3 L1C +29 & L5X +29 | G24 (reference of both L5 chains, k ∈ [−9, +41]) | **all-band**, basis carries `iftd(L1C+L5X)=ref_arc;iftd(L2W+L5X)=ref_arc`, `reason=no_exoneration` | {L2W} — the §20.1 trap reproduced |
| E1 L5X −99 | G24 | {L5X}, products byte-identical to OFF | {L5X} |
| E2 L1C +37 | G24 | all-band (the named cost) | {L1C} |
| E3 L1C +29 & L5X +29 | G25 (not a reference until +43) | byte-identical to OFF (all-band via the (1,5) veto) | all-band |

* **B1 OFF byte-identity:** knob absent vs `false`, and this exe vs main's, on the null leg —
  `products.txt` and `network_events.csv` identical.
* **B2 identity:** the table above, read from each leg's `network_events.csv` verdict row
  (`implicated=…;exonerated=…;basis=…`) and the run-end clause's new `ref-arc refusals N` field
  (invariant #4: an ON leg at G24 reporting 0 refusals is dead wiring).
* **B3 cost census (no run):** over the seed's 6 h `network_events.csv` (scope OFF, `would-narrow
  23`), the number of would-narrow verdicts whose exonerating pair sits on a reference arc of its
  chain (reference identified from `network_if_slip.csv` by `abs_dev_m == 0` on every row of the
  arc, cross-checked against the new `is_ref` column on a 20-epoch smoke) — the fraction of
  narrowings the clause forfeits.  Recorded; the pre-registered expectation is ≤ 15 % (one
  reference arc per station per chain among ~12 tracked).
* **B4 the sim bed (owed for the sim-golden pin, not for the merge):** the increment-1 storm arm
  (`config/test_suite/_golden_sim_campaign_zwdblk_sts.json` + the knob) graded with
  `scripts/simnet_storm_grade.py` against the Study-7 final arm: consumer settled p50 ≤ 0.13 m
  (was 0.1204), T4 attribution ≤ 15 (was 11), split-referenced satellites ≤ 0.5/epoch (was 0.0),
  ZWD settled rms ≤ 0.0075 m (was 0.0069), IF-TD vetoes not fewer.  Pass ⇒ pin in
  `golden_sim_tri_w6` through the generator; fail ⇒ the clause stays a field-only recommendation
  with the sim cost named.
* **Then (sequel, on a field seed that warm-resumes — §21 Phase 2):** the wire/consumer price of
  the E3 narrowing at G24 with the clause OFF vs ON (gates G-C/G-D of §20, dark until now), and the
  scope+pairs+clause FIELD pin through the generator if the §20 bars pass.
### 22.4 RESULTS — B1–B3 PASS on the field seed; an unregistered effect; B4 running (2026-09-12 ~10:10 local; worker on worktree-agent-a02f192d1479c9bc2 @ `c8096ee`, exe `gnss_processor_cbref.exe` sha256 `5BD7D2DC…`, legs `runs/evleg_cbref_*` on the §21.5 interim posture `state_snapshot_carry: false`)

**Built as §22.2:** `network.cycle_slip.common_band_ref_arc` (default OFF, warn+disarm without the scope);
`CommonBandEvidence::iftd_untested`; the preview carries `is_ref` (an `IfTdPreview` element replaces the
`{pair, flagged}` pair) and folds three ways only when the knob is armed; the rule refuses an untested pair
before the veto check (`iftd_ref_arc` + `iftd_ref_arc_pairs`, token `iftd(<pair>)=ref_arc`); counters
`n_cb_iftd_ref_arc[_by_pair]`; run-end field `ref-arc refusals N [pair: n …]`; `network_if_slip.csv` gains
`is_ref`.  One addition the worker made from its own null A/B (`c8096ee`): the basis tail names untested
pairs the refusal branch never reached (MW-flagged / immature / unprocessed), exactly as the flagged set is
named — without it 6 of the 12 differing null rows differed only by a vanished token.  Basis-string only.
Debug suite 1970 / 0 fail; `CommonBand*` 50/50.

**B1 PASS.**  Null on/off: `products.txt` identical (`1FF0BA20…`); `network_events.csv` differs in 12 of
1992 rows, **12/12 carry `ref_arc`**, all `T0_band_scope`, all natural L5-chain reference arcs (e.g.
WTZR00DEU/G24 at 277410, `iftd(L1C+L5Q)=ref_arc;iftd(L2W+L5Q)=ref_arc`).  Smoke, knob absent vs `false`:
`products.txt` `B9DA8CC1…` identical.

**B2 PASS — every pre-registered row.**

| leg (GOPE00CZE, +10) | ON | OFF |
|---|---|---|
| E3 L1C +29 & L5X +29, **G24** | `all reason=no_exoneration;implicated=L1C;L2W;L5X;basis=mw(L1C+L5X)silent(n=10;thr=1.4WL);iftd(L1C+L5X)=ref_arc;mw(L1C+L2W)flag;mw(L2W+L5X)flag;iftd(L1C+L2W)=flagged;iftd(L2W+L5X)=ref_arc;lli=none` | `keep implicated=L2W;exonerated=L1C;L5X` — the §20.1 trap |
| E1 L5X −99, G24 | `keep implicated=L5X` | same; products `DEF230D7…` identical |
| E2 L1C +37, G24 | `all` (the named cost) | `keep implicated=L1C` |
| E3, G25 | `all`, products `44D461B0…` identical | `all` via `iftd(L1C+L5X)=flagged` |

The clause reaches the drain: on E3/G24 the OFF IF-TD refloat row reads `L1C;L2W;pair=L1C+L2W;cb_kept=L1C`
(the scope held back the band that really slipped); ON it reads `L1C;L2W;pair=L1C+L2W`.  Run-end
`ref-arc refusals`: null 6 `[L1C_L5Q 1, L1C_L5X 1, L1W_L5X 2, L2W_L5Q 2]`, E3/G24 8 (`L1C_L5X` 1 → 3, its two
verdict epochs), E2/G24 8 (`L2W_L5X` +2), E1 and E3/G25 6 (= null) — invariant #4 in both directions.

**B3 — the census could not be run as pre-registered, and the substitute is exact.**  The OFF run writes no
`T0_band_scope` rows (`NetworkCommonBandScope.cpp:521` returns before the emit loop when unarmed; the seed's
20 484 event rows hold 0), so its 23 would-narrow verdicts exist only as a counter.  Census on the five leg
pairs instead (both arms scope ON + `all`, the clause the only difference): **4 forfeited narrowings of 106
(3.8 %, bar ≤ 15 %), all four injected — E3 ×2 (the fix) and E2 ×2 (the cost); natural forfeitures 0/20.**
Reference exposure under `all`: `is_ref` on 18.3 % of IF-TD samples; 107 of 507 (station, satellite) arcs are
a reference of ≥ 1 chain (multiplicity 1/2/3 = 32/47/28).  ⚠ The `abs_dev_m == 0` proxy §22.3 named is
UNUSABLE: 28.5 % false negatives on the smoke (`abs_dev = |dd − med|` is zero only where the pool median is),
0 of 71 hits at a flag epoch on the seed — the `is_ref` column is the instrument.

**Unregistered effect, recorded, not banked.**  On every leg the clause also resolves **six `inconsistent`
fallbacks into narrowings** (null: narrowed 1 → 7, inconsistent 15 → 9): a pair fired while every band was
"exonerated" — a contradiction — and refusing the untestable exonerator leaves consistent evidence that names
one band.  Products are unchanged because those six MW flags were dropped at admission, so the drain acted on
none of them.  The principle is right (an exonerator that cannot be cross-checked is not an exonerator); the
wire effect is nil on the null and is to be read on the storm arm, not assumed.

**B4 running:** `config/test_suite/_golden_sim_campaign_zwdblk_refarc.json` = the increment-1 `sts` storm arm +
the clause, `gnss_processor_cbref.exe`, against `runs/golden_sim_campaign_zwdblk_sts` (exe certification by a
20-epoch smoke pair `stspromo` vs `cbref` on the `sts` config, or a same-exe OFF rerun).  Bars as §22.3.
### 22.5 B4 PASS — the sim storm bed is neutral on every bar; the clause is PROMOTED (RECOMMEND_ON with the scope) and pinned in `golden_sim_tri_w6` (2026-09-12 ~11:30 local; manager grade with `scripts/simnet_storm_grade.py`)

**Arms.**  `runs/golden_sim_campaign_zwdblk_refarc` = `config/test_suite/_golden_sim_campaign_zwdblk_refarc.json`
(the increment-1 `sts` arm + `common_band_ref_arc: true`), exe `gnss_processor_cbref.exe` (main `904b89e` + the
clause), exit 0, 720/720, 1507 s on a shared box; against `runs/golden_sim_campaign_zwdblk_sts` (exe
`stspromo`, 1303 s).  **Exe certification without a smoke:** `products.txt` is byte-identical between the two arms
for the first **283 epochs** — the first differing byte is the clock record after the E record for tow 310890
(2.35 h in), where the clause first actuates — so the OFF path across the two builds is certified on the storm
bed itself.  The 286/286 symmetric event-row differences are the same events with different numbers after that
epoch (PBGAUGE 224, CLK_ANCHOR 19, READMIT 17, STRANSFORM 2, PIVOT 2) plus 22 `T0_band_scope` rows carrying
`ref_arc`; the first three differing rows (tow 302430) are `all → all` with the basis tail only.

| channel (§22.3 bar) | sts | refarc | bar |
|---|---|---|---|
| ZWD settled truth rms | 0.0059 | **0.0059** | ≤ 0.0075 |
| ZWD hourly | 0.17, 0.007, 0.007, 0.006, 0.006, 0.006 | identical | — |
| consumer settled p50 / rms | 0.0807 / 0.09006 | **0.0807 / 0.09004** | p50 ≤ 0.13 |
| consumer hourly rms | 0.645, 0.109, 0.116, 0.101, 0.103, 0.059 | identical | — |
| T4 attribution events / ambient | 11 / 0.193 | **11** / 0.190 | ≤ 15 |
| split-referenced satellites mean / max | 0.0 / 0 | **0.0 / 0** | ≤ 0.5 |
| WL / EWL / L1C ref-free settled | 99.2 / 100.0 / 90.4 | identical | — |
| IF-TD vetoes (by pair) | 116 (L1C_L2W 79, L1C_L5Q 37) | **116** (identical) | not fewer |
| IF-TD pairs eval / flag / refloat | 2055853 / 4747 / 4722 | identical | — |
| PB-GAUGE handovers continuous / cold | 362 / 100 | identical | — |
| S-transform edges / re-expressed / retained | 2 / 52 / 376 | identical | — |
| scope: verdicts / narrowed / no-exon / inconsistent | 4208 / 3147 / 807 / 202 | 4208 / **3150** / **820** / **186** | — |
| **ref-arc refusals** | — | **29 [L1C_L5Q 12, L1C_L7Q 5, L2W_L5Q 12]** | > 0 (invariant #4) |

Every pre-registered B4 bar passes with the two arms equal to the digit; the only movement is inside the scope's
bookkeeping — 16 `inconsistent` fallbacks become 13 `no_exoneration` (both all-band, no action change) and 3
narrowings — and the solve diverges only from the first of those narrowings on.  The sim bed never carries the
reference-arc trap (its injected and campaign slips do not land equal-count on a reference), so the clause is
neutral there and fixing on the field (§22.4).

**Decision.**  RECOMMEND_ON together with the scope: pinned in `golden_sim_tri_w6` through the generator
(`scripts/_rs2_refarc_ledger_oneoff.py refarc` → ledger row + md + posture row + manifest pin; `--verify` PASS;
`config_posture_check.py --verify-loader` 0 gaps).  The field golden needs no exception — the scope itself is a
field exception there, so the clause is out of regime rather than suppressed; the field pin of scope + pairs +
clause rides on the RS-2 seed (§21.5) once the §20 wire/consumer bars can be read.  Grader hygiene noted, not
fixed here: `simnet_storm_grade.py`'s `bscope_clause` reads `None` on both
arms.  **Resolved 2026-09-13 (main `dcf04dd`, worker commit `ee33550`): the regex was not stale — the `§Z`
response-scope clause (`raim_erase_band_scope` / `refloat_band_scope`) is out of regime on the campaign lineage, which
emits the detector-side `Network (common-band scope): verdicts …` census instead; the grader now tries both, records
`bscope_clause_kind`, and the captured census reproduces B4's table to the digit (4208 / 3150 vs 3147 / 820 vs 807 /
186 vs 202 / 116 / ref-arc refusals 29).**

