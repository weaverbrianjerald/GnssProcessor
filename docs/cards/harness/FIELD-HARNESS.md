# FIELD harness — the 6 h field seed, the owed field legs for the sim-validated slip cards, and FP-1 / FP-2

**STATUS**

- **id:** FIELD harness
- **topic:** harness
- **status:** closed
- **summary:** the 6 h field seed, the owed field legs for the sim-validated slip cards, and FP-1 / FP-2
- **dates in the text:** 2026-09-11 → 2026-09-13
- **knobs / posture:** `network.cycle_slip.mw_evidence_refloat`, `network.cycle_slip.common_band_scope`, `network.cycle_slip.if_phase_td_pairs`, `network.trace_rx_states`, `network.trace_zwd`, `network.trace_ionosphere`
- **hashes named in the text** (commit / exe / byte-cert): `8d5dad6`, `e90086f`, `109a6b2`, `2587a51`, `545b8bdd2b638231`, `fb6f063ea23adf10`, `e6071f43`
- **run dirs named in the text:** `runs/evseed_field6h`, `runs/evseed_field6h_on`, `runs/evseed_field6h_ev`, `runs/evleg_fp1_`
- **source:** `docs/archive/SPEC_event_harness.md` — 20, 20.1, 20.2, 20.3

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 20. The FIELD harness — a 6 h seed on the field golden and the owed field legs for the three sim-validated slip cards (pre-registered 2026-09-12 ~01:00 local, BEFORE any field leg is read)

**Question.**  Three slip cards are RECOMMEND_ON and pinned in `golden_sim_tri_w6` but carry documented
exceptions on `golden_field_gps_dual` (manifest, 2026-09-11): `network.cycle_slip.mw_evidence_refloat`,
`network.cycle_slip.common_band_scope` and `network.cycle_slip.if_phase_td_pairs = all`.  Each exception
names the same mechanism: on the field the L5 lanes may be BIA-corrected under the real COD OSB product and
then run the between-satellite MW path (the evidence rule's bias-free clause EXCLUDES that path; the scope's
silence semantics were never priced on it; the pairs' (L2W,L5Q) pool amplifies phase noise 12x and was never
priced on real data).  The field harness answers each with ONE injected event and a null, exactly as the sim
harness did, and the outcome per knob is a field pin through the generator or an exception WITH THE MECHANISM.

**What is different on the field (facts checked before the brief, not assumptions).**
- The field golden is a `carry_invariant` + `phase_datum_eq34` lineage with `phase_leveling: true`,
  `mw_detect` + `if_phase_td` + `if_phase_td_ref_gate` + `mw_lli_corroborate` armed, GPS L1C/L2W/L5Q at
  61 stations, clock datum MBAR00UGA, closed-loop consumer WTZZ00DEU (undifferenced, float).  There is NO
  prioritized phase-anchor election (eq34 supersedes it), so `anchor_slip_handover` is out of regime and the
  scope's anchor clause is structurally inert here (`split_referenced_sats ... of 0 anchored` is the expected
  run-end reading; record it).  T4 on this lineage is the ATTRIBUTION erase (`T4_attrib/attrib_arc` rows).
- The field has NO truth: every grade is event MINUS null (the seam transient, the bed and the posture cancel)
  on datum-invariant channels, and the consumer is graded DE-BIASED (the null carries the static bias).  The
  DD-fractional trace is uniform on the field (SPEC_amb_iono §6.13) and grades nothing here.
- `resume_coupling_restore` THROWS at load without `carry_coupling_gauge_project` (absent from the golden):
  field legs arm `resume_frame_restore` only (inert on a v1 snapshot — logged) and set
  `resume_coupling_restore: false` explicitly.  `zwd_seam_reseed` is armed by `event_leg.py` as on every leg.
- `logging.enable_mw_slip_detector` defaults to FALSE once a `logging` section exists: the seed and every leg
  arm it explicitly (the lane bias class `bia_corrected` is a column of that log and is the whole question).
- Loop cost on this lineage was 307 ms/epoch on a 4 h carry_invariant arm (`fld63_relevel4h_on`); the seed's
  own `network_run_summary.csv` is the number to quote.

**Seed.**  `runs/evseed_field6h` = `python scripts/event_leg.py write-seed --name field6h --base
config/golden/golden_field_gps_dual.json --hours 6 --interval-ep 60` with `--extra` (added to write-seed for
this card) arming `network.trace_rx_states`, `network.trace_zwd`, `network.trace_ionosphere` and
`logging.enable_mw_slip_detector` — all trace-class.  Exe = main HEAD (8d5dad6; last code commit e90086f)
Release, pinned as `build/x64-Release/gnss_processor_fld.exe` (hash recorded; the same binary runs every leg).
Launched detached (`run_seed.cmd` + Start-Process, `exitcode.txt` watched).  The seed's OFF run-end clause
`Network (MW detect-only admission): legacy N, evidence 0 [knob off; would-admit N]` is the FIRST field
measurement of the evidence card: the size of the population the knob would refloat on real data (gate G-F).

**Event site rule (from the seed's own traces, never from truth or visibility).**  Snapshot = the one nearest
5 h (ep ~0605).  Station: a European station near the consumer so the wire effect reaches WTZZ —
GOPE00CZE preferred, then GRAZ00AUT / LEIJ00DEU / POTS00DEU; never the clock datum (MBAR00UGA).  Satellite:
from `network_slip_raw.csv` at the snapshot tow, a GPS satellite with L5Q rows at that station (a triple-band
arc), elevation 35–65°, present every epoch for >= 60 epochs before the snapshot with lli1 = lli2 = 0 (mature,
receiver-clean).  The chosen (station, sat, elevation, arc age) is recorded in the leg meta.

**Legs (60 epochs, event at +10, one producer at a time — box cap 2 shared with another session).**
Postures via `--extra` (every leg also sets `resume_coupling_restore: false`):
P0 OFF (golden defaults) · P1 evidence ON · P2 scope ON + pairs `all` · P2f scope ON + pairs `first` ·
P3 evidence + scope + pairs `all` (= what a field pin would arm).

| leg | event | postures |
|---|---|---|
| null | none | P0, P1, P2, P3 |
| E1 | L5Q −99 cyc, no LLI (the sim's exact unflagged-L5 miss) | P0, P1, P2, P3 |
| E2 | L1C +37 cyc, no LLI | P0, P2, P3 |
| E3 | L1C +29 AND L5Q +29 at the same epoch (Study 7's equal-count shape on the (1,5) lane) | P0, P2f, P2 |
| E5 | L5Q −31 cyc WITH `flag_lli` (rule F3's door) | P0, P2 |

**Gates, frozen.**  All read at epochs relative to the event, event − null at the SAME posture; ON vs OFF
compares those differences.  Floor = |ON-null − OFF-null| on the same channel and window.
- **G-A identity (invariant #4).**  From `network_events.csv` (T0_mw_detect rows `mw_wl_slip`/`lli_slip`/
  `band_scope`, `T0_band_scope/common_band`, `T1_if_slip/if_slip`), the MW log and `network_if_slip.csv`
  at (station, sat, k in [-1, +5]): which detectors fired, the per-lane evidence ratio and `bia_corrected`,
  and WHICH BANDS were freed.  Expected: E1 P0 = all three bands (two MW pairs fire); E1 P2/P3 = {L5Q} only;
  E2 P2/P3 = {L1C}; E3 P2f = a `keep` verdict with implicated {L2W} (the trap reproduced on the field) or an
  all-band fallback (the trap absent on the field — also an answer); E3 P2 = IF-TD (1,5) veto → all bands;
  E5 P2 = F3 verdict {L5Q}.  A narrowing FOLLOWED by a `T4_attrib/attrib_arc` at that station within +45 is a
  wrong narrowing = FAIL for the scope.
- **G-B station frame.**  `network_rx_states.csv` (CLK), `network_zwd.csv`, `network_ionosphere.csv`
  (the injected LOS): event − null at +1, +10, +45.  Bar: ON |Δ| at +45 <= OFF |Δ| at +45 on every channel,
  and the station's attribution-erase count (event − null, +0..+45) ON <= OFF.
- **G-C wire.**  `products.txt` event − null per (sat, kind, signal): the injected satellite's max |Δ| over
  +0..+45 for CLK and PB (ON <= OFF), and the max over every OTHER satellite (<= 2x the floor).
- **G-D consumer, de-biased.**  `closed_loop_trace.csv` ENU event − null: max |Δ3D| over +0..+50 and mean
  |Δ3D| over +30..+50.  Bar: ON mean <= OFF mean + floor.
- **G-E quiet cost (ON-null vs OFF-null, 60 epochs).**  No new withhold; network-wide `attrib_arc` within
  ±3; wire CLK max |Δ| <= 0.02 m; consumer max |Δ3D| <= 0.03 m; the run-end clauses of both nulls quoted.
- **G-F evidence population (seed + E1).**  The seed's `would-admit` count and the MW log's `bia_corrected`
  share per lane (L1C/L2W, L1C/L5Q, L2W/L5Q).  Pin condition for the evidence card: would-admit >= 1 on a
  bias-free lane in 6 h, AND on E1 the evidence path admits a detection the legacy gate drops (or legacy
  already admits it and P1 == P0 byte-for-byte — then the knob is harmless and the pin rides on G-E alone).
  If every lane reads `bia_corrected = 1` and would-admit = 0: EXCEPTION, "structurally inert on the field —
  every MW lane is between-satellite under the real OSB product; the population this rule admits does not
  exist here; T4 attribution is the field's catch for an unflagged L5 slip (E1 P0 tells how fast)".
- **G-G the field twin (only if G-A..G-E pass for the scope + pairs).**  ONE 6 h run of the seed config with
  the pins under test (`runs/evseed_field6h_on`, same exe) against the seed: `attrib_arc` total not up by
  more than 10 %, withheld epochs equal or fewer, consumer de-biased settled (h3–6) p50 and rms not worse by
  more than 5 %, `ev_consumer_invariant_grade.py` pbstep fraction not worse.  PASS → pin (manifest pins +
  exception text removed, ledger evidence line, regenerate, `--verify`, `config_posture_check.py
  --verify-loader`).  FAIL → exception with the failing channel as the mechanism.

**Decision rules.**  Scope + pairs are pinned TOGETHER or not at all (the veto is the scope's coverage).
Evidence is decided by G-F first (population), then G-B..G-E on E1.  An identity FAIL on E3 P2f with a PASS
on E3 P2 is the field reproduction of Study 7 and counts FOR the pair (it is what `all` exists to close).

**Traps carried over.**  Knob-on legs against knob-on nulls (a dosed or natural-slip leg differs);
`--from-leg`/`--extra` copy nothing you did not pass; the renamed exe is a snapshot (hash it); traces are
CWD-relative to the leg dir; a 0xC0000409 with no message is an uncaught runtime_error (read stdout.txt);
check `tasklist` before EVERY launch; grade only after the gates above are on disk (this section).
### 20.1 RESULTS — the field seed, the legs, the twin, and the two defects the harness found (2026-09-12 ~01:00–04:00 local; worker on the main tree, exe `gnss_processor_fld.exe` 3FDC5F8D… = main 109a6b2 / last code commit e90086f; manager re-grade)

**Seed `runs/evseed_field6h`** (golden + trace-class extras, `--extra` added to write-seed in 2587a51): exit 0,
723 synced / **720 emitted / 0 withheld**, 1909 ms/epoch on a box shared with another producer (6× the 307 ms
quoted from the 4 h relevel arm — the seed arms the full trace set), 12 snapshots ep0001…ep0665.  Run-end,
verbatim: `Network (MW detect-only admission): legacy 160, evidence 0 [knob off; would-admit 10]; dropped
loose 48 / unknown 1174.` · `IF-TD pairs [first]: 6 pair kind(s), eval 340307 / flag 149 / refloat 146`
(L1C_L2W 113 flags; every L5-bearing pair 0) · common-band scope (OFF, would-narrow 23; `… of 0.0 anchored`,
the predicted inert anchor clause) · **T4 attribution 0 for the whole 6 h and in every leg** (`network_raim.csv`
0 bytes — the RAIM-pool re-host gap on `carry_invariant`; the §20 wrong-narrowing tripwire cannot fire on this
lineage).  Natural slips per hour (h0…h5): mw_wl_slip 41/4/11/4/5/6, lli_slip 91/16/11/17/37/33, if_slip
51/14/13/10/42/19.  Consumer WTZZ (L1/L2 float): 0 % fix, settled h3–6 p50 6.24 m.

**G-F, the lane bias class — the OPPOSITE of the exception's fear.**  `mw_slip_detector.log`, 380 detections:

| lane | detections | bia_corrected share |
|---|---|---|
| L1C/L2W | 77 | **0.753** |
| L1C/L5Q | 97 | **0.000** |
| L2W/L5Q | 206 | **0.000** |

Under the real COD OSB product it is the L1/L2 lane that runs the between-satellite path; **the L5 lanes are
the bias-free population the evidence rule is written for**, and `would-admit 10` says that population is
non-empty on real data (~1.7 per hour network-wide).

**Event site.**  GOPE00CZE / G24, snapshot ep0604 (tow 277290), event tow 277620.  Two forced deviations,
both from the seed's own traces: GOPE's third band is **L5X** (RINEX C5X/L5X; priorities WCSLXPYMIQ), so every
event signal is L5X; and `network_slip_raw.csv` lists only the first MW pair under `mw_chan=single`, so the
triple-band test was read from the P2 null's `network_if_slip.csv` (pairs=all enumerates every pair).
Candidates: G12 76.6° (dual only), **G24 57.5° (triple, 60/60 present, 0 LLI)**, G25 44.9°, G19/G32 < 35°.

**⚠ DEFECT 1 — the field golden cannot warm-resume.**  Every leg exits 0 and resumes WARM but withholds
**61 of 61 epochs** (`sh_gate_withheld`): at the first leg epoch (tow 277320) the SH a₀₀ reads **−4810 TECU**
against the seed's **+38.07** at the same tow, clock_std 2819 m (seed 9.9), σ̂₀_pf_code 17219 (seed 4.9),
674 chord births in the second epoch.  So `products.txt` is never written and `closed_loop_trace.csv` is
empty — **gates G-C and G-D are dark on every leg, and G-E's wire/consumer halves with them.**  Discriminators
(worker): the plain RTB-7 resume (`resume_frame_restore: false`) is *worse* (3/61 emitted, a₀₀ −6429, the
three emitted epochs put the consumer at 5.0–5.5 km); plain + `zwd_seam_reseed: false` 7/61 (a₀₀ −5700).
The loader's own text names the coupling path: `resume_coupling_restore` is the only knob that rebuilds the
QK/QP regressors at the seam and it is illegal without `carry_coupling_gauge_project` (Card 1f) — which the
golden does not carry, has no ledger row, defaults false, **and sits in the warm-resume fingerprint** (probe
`evleg_fld_probe_cpl` refused before its first epoch: 545b8bdd2b638231 vs fb6f063ea23adf10).  The old
validated field warm-resume config (`_fld50ws_resume.json`, RTB-7) carried `carry_coupling_gauge_project: true`.
**Owed card (out of this card's scope): 1f on the field golden as a FRESH-CONVERGENCE pair, then one warm-resume
leg; if that does not cure the seam, bisect the seam (the cause is attributed by the loader's text, not isolated).**

**G-A identity — clean on single-band slips, and a trap on the reference arc.**  Bands freed:

| event | injected | P0 OFF | P1 evidence | P2 / P2f / P3 | expected |
|---|---|---|---|---|---|
| E1 | L5X −99, no LLI | L1C+L5X | L1C+L5X (≡ P0) | **L5X** | {L5} ✓ |
| E2 | L1C +37, no LLI | L1C+L2W+L5X | — | **L1C** | {L1C} ✓ |
| E3 | L1C +29 and L5X +29 | L1C+L2W+L5X | — | **L2W** under first AND all | P2f trap / P2 all-band ✗ |
| E5 | L5X −31 with LLI | L1C+L5X | — | **L5X** (`rule=lli_epoch`, F3) | {L5} ✓ |

Verdict bases, verbatim: E1 `implicated=L5X;exonerated=L1C;L2W;basis=mw(L1C+L2W)silent(n=10;thr=1.5WL);
mw(L1C+L5X)flag;mw(L2W+L5X)flag;iftd(…)silent×3;lli=none`; E2 `implicated=L1C;exonerated=L2W;L5X;basis=
mw(L2W+L5X)silent(n=10;thr=0.2WL);mw(L1C+L2W)flag;mw(L1C+L5X)flag;iftd(L1C+L2W)=flagged`; E3
`implicated=L2W;exonerated=L1C;L5X;basis=mw(L1C+L5X)silent(n=10;thr=1.4WL);mw(L1C+L2W)flag;mw(L2W+L5X)flag;
iftd(L1C+L2W)=flagged;iftd(L1C+L5X)silent;iftd(L2W+L5X)silent;lli=none` — the equal-count trap reproduced on
real data: the scope freed the one band that never moved and left 29 cycles standing on both guilty bands,
where OFF freed all three.  P1 ≡ P0 bit-for-bit on the null and on E1 (the legacy gate admits a −99 L5 slip
on the field: the lane's σ is ~5× smaller than the sim's); P3 ≡ P2 everywhere.

**⚠ DEFECT 2 — why `pairs: all` did not veto E3: the IF-TD reference arc is UNTESTED in the additional
chains.**  `network_if_slip.csv` (P2 null, 60 epochs): the L5-bearing pairs at GOPE/G24 read
`abs_dev_m = 0.000000` at every epoch of the window while their pools fill normally; network-wide, 41
(station, sat) arcs are zero on every row — **all of them on the (L2W, L5·) pair, exactly one per station,
21 L5Q + 20 L5X (8.4 % / 7.4 % of arcs, so not a tracking-code effect)**, and none of them zero in the
companion L1/L2 chain.  Per (station, pair, epoch) group there is exactly one zero-deviation satellite in
92.7 % of groups; pooling both L5 chains, one satellite in 97.6 %.  It is the chain's REFERENCE: the statistic
is ΔOMC(sat) − ΔOMC(ref), zero for the reference itself.  The first chain tests its reference through the
reference arm; the additional chains do NOT re-run it (§14.10.4, the flood guard).  Correction to the naive
reading: the L5 chains adopt the (1,2) chain's reference only when it tracks L5 (46.7 % of station-epochs
shared; of the 53.3 % that differ, 98 % are a (1,2) reference with no L5), otherwise they elect the highest
L5-capable satellite (rank 1 in 80.6 %, rank ≤ 2 in 99.0 %).  At GOPE: (1,2) reference G12 (76.6°, Block
IIR-M, no L5); both L5 chains reference **G24** for k ∈ [−9, +41], then G25.  **So E3 at G24 was confounded —
the (1,5) veto was structurally unavailable, not merely silent — and the dead zone is every station's
highest L5-capable satellite: the best-placed arc of the constellation.**  G25 at GOPE has live L5 deviations
at k = 0 (0.0086 / 0.0044 m against thresholds 0.109 / 0.170 m; an equal +29-cycle L1+L5 slip gives ~3.1 m
of IF deviation, 28× the bar) and becomes the reference only at k = +43.

**The equal-count leg OFF the reference arc (GOPE00CZE / G25, same tow, L1C +29 and L5X +29) — the trap
is CLOSED there by `pairs: all` and OPEN under `pairs: first`.**  P0 freed L1C+L2W+L5X (MW (2,5) + IF-TD (1,2)
drains).  P2f: `keep implicated=L2W;exonerated=L1C;L5X;basis=mw(L1C+L5X)silent(n=10;thr=1.5WL);mw(L1C+L2W)flag;
mw(L2W+L5X)flag;iftd(L1C+L2W)=flagged;lli=none` → **L2W only** (the innocent band; 29 cycles left standing on
both injected bands).  P2: `all reason=no_exoneration;…;iftd(L1C+L5X)=flagged;…;iftd(L2W+L5X)=flagged` → all
three bands freed, three `T1_if_slip` refloat rows (one per pair).  `network_if_slip.csv` GOPE/G25 at k = 0:
L1C/L2W 14.044 m (thr 0.107, flagged), **L1C/L5X 3.1508 m (thr 0.1089, flagged — 29×; the prediction
29 cyc × 0.107 m/cyc = 3.10 m holds to 2 %)**, L2W/L5X 83.17 m (thr 0.171, flagged); under `first` the two
L5 rows do not exist.  The MW (1,5) lane is silent (n=10, thr 1.5 WL) exactly as the trap requires.  The
close-spaced pools are healthy on real data (thresholds 0.109 / 0.171 m — the 12× amplification is absorbed).
Caveat: G25 takes the L5 reference at k = +43, so a future B/C/D grade on this arm stops at +40.

**G-G, the 6 h field twin for the evidence knob (`runs/evseed_field6h_ev` = seed config + `mw_evidence_refloat`,
same exe; manager-launched and graded).**  Exit 0, **720/720 emitted, 0 withheld (= the seed)**; run-end
`legacy 160, evidence 10 [knob on; would-admit 0]; dropped loose 44 / unknown 1168`; every other detector
count identical (lli_slip 205, if_slip 149, eq34 reref 746 / reattach 337; mw_wl_slip 71 → 73; attribution 0
both).  The admissions (`src=evidence` rows): BOGT/G32 L1C;L5X ratio 7.0 n=67 int=unknown · BSHM/G30 L1C;L5Q
4.5 n=349 known · KIT3/G06 L1C;L5Q 5.0 n=56 unknown · SYOG/G26 L2W;L5X 7.7 n=185 known · CHUR/G03 L2W;L5Q
5.1 n=25 unknown — **all on the bias-free L5 lanes, all mature, three with an integer the legacy gate can
never admit.**  Wire (products ON − OFF): satellite clocks ≤ 3 mm at every epoch; four admissions move the
satellite's emitted phase bias ≤ 0.14 m within 30 min; the SYOG/G26 admission **re-realizes the (G26, L5X)
phase-bias datum by 9.2 m in the admission epoch and it is DECLARED** (`disc_counter` 0 → 1 — invariant #7
holds).  Consumer (`ev_consumer_invariant_grade.py`, t0 259200, settled h3–6): pb_reset / cb_disc / clk_disc
**29 / 590 / 6 identical**; stepping-epoch fraction **0.1181 identical**; p90 of the per-epoch max emitted
step 0.127 → 0.257 m (recorded — the consumer runs L1/L2 and did not move); raw p50 6.24 → 6.42 m; de-biased
p50 **5.28 → 5.49 (+4.0 %)**, rms **6.48 → 6.16 (−5.0 %)**; per-hour de-biased p50 3.53 → 3.21, rms
4.42 → 3.90.  **Every G-G bar passes** (withheld equal, consumer within ±5 %, step fraction equal, admissions
identified and on the lanes the rule targets).

**VERDICTS (per the §20 decision rules).**
- `network.cycle_slip.mw_evidence_refloat` — **FIELD PIN.**  G-F: the L5 lanes are 0 % bias-corrected and
  would-admit = 10 in 6 h; G-E: P1 ≡ P0 bit-for-bit on the quiet leg and on E1; G-G: pass on every bar.  The
  fear in the exception text was inverted by measurement.  Field cost signature to watch: an admission on the
  arc that realizes a (sat, signal) phase-bias datum re-realizes the family (declared) — on this lineage there
  is no anchor election to hand it over, so the consumer pays one declared discontinuity per such admission.
- `network.cycle_slip.common_band_scope` + `if_phase_td_pairs = all` — **EXCEPTION, with the mechanism.**
  Single-band narrowing is correct on real data (E1, E2, E5 by the receiver's bits), but the equal-count trap is
  OPEN on every station's IF-TD reference satellite (the highest L5-capable one), where the additional pairs
  cannot veto because the reference arc is untested (DEFECT 2), and on this lineage the wrong narrowing has
  no backstop (T4 attribution structurally 0).  The wire/consumer cost of the trap could not be measured
  (DEFECT 1).  Fix card (stub, not built here): a pair whose IF-TD chain holds the satellite as its reference
  is NOT silent for the verdict (untested ⇒ immature ⇒ all-band on that arc — one clause in
  `NetworkCommonBandScope`, no detector change, no re-opening of the §14.10.4 flood), re-priced on G24 and G25;
  then the field pin rides on a seed that can warm-resume (DEFECT 1's card).
- Both defects are HARNESS findings of the kind the field seed exists for: neither is visible on the sim (the
  sim's S2 leg injected on a non-reference satellite; the sim golden resumes through the F3 coupling path).

### 20.2 RESULTS — FP-1, the §20 matrix on the warm-resuming seed: defect 1 is cured, every identity row reproduces, and the pin is REFUSED by a defect the gates could not see — the reference-slip fan-out (2026-09-13 ~11:30 local; worker on worktree-agent-a8ac220a59ca83a04, exe `gnss_processor_rs7.exe` 23C628B3… = main's producer after RS-7, seed `evseed_field6h_v5` ep0604, legs `runs/evleg_fp1_*`; manager grade)

**Legs.**  {null, E1, E2, E3/G24, E3/G25} × {OFF, PIN = scope + pairs `all` + the §22 clause}; the E5 pair was not
run (slot contention; F3's door is not what decides this card).  Every leg 61/61 emitted, 0 withheld, no cold start,
products + 61 consumer rows — **§20.1's defect 1 is cured on v5**, and G-B / G-C / G-D are readable on the field for
the first time.

**G-A PASS on every pre-registered row.**  E1 PIN → `{L5X}` (`iftd(L1C+L5X)=ref_arc;iftd(L2W+L5X)=ref_arc`); E2 PIN →
`all` (`no_exoneration`, the clause's named cost); E3/G24 PIN → `all reason=no_exoneration` with both L5 pairs
`ref_arc` (§22.4 B2 verbatim — the trap closed); E3/G25 PIN → `all` via `iftd(L1C+L5X)=flagged` (the (1,5) veto).
PIN null run-end `narrowed 7 (bands kept 14), all-band 44 (no exoneration 34 / inconsistent 9 / anchor co-locate 1),
IF-TD vetoes 0, ref-arc refusals 6 [L1C_L5Q 1, L1C_L5X 1, L1W_L5X 2, L2W_L5Q 2]` = §22.4's null to the pair; OFF null
`narrowed 1, inconsistent 15, ref-arc refusals 0 [would-narrow 1]`.  T4 attribution 0 on every leg (structural on
`carry_invariant`), so the wrong-narrowing tripwire cannot fire on this lineage.

**The blocker — the reference-slip fan-out (§22.6).**  GOPE's chains from `is_ref`: (L1C,L2W) → G12 all leg; both
L5 chains → **G24 for k ∈ [−9, +41]**, handing to G25 at +42.  An IF-TD sample is `dOMC(sat) − dOMC(ref)`, so the
injected slip reads `abs_dev = 0.000000` on G24's own L5 rows and full-size on everyone else's:

| leg | G24's own L5 rows | other satellites flagged → refloats | E3/G25 control (G25 not a reference) |
|---|---|---|---|
| E1 PIN (L5X −99) | 0.000000 ×2 | **6** (G11 / G25 / G32 × both L5 chains; 31.8 m at 253–292×, 283.9 m at 605–1580× the threshold) → 6 | — |
| E2 PIN (L1C +37) | 0.000000 ×2 | **3** ((1,5) only — an L1C slip leaves IF(L2W,L5X) alone) → 3 | — |
| E3/G24 PIN | 0.000000 ×2 | **6** (3.16 m at 25–29×, 83.2 m at 177–463×) → 6 | G25's own rows 3.15 / 83.2 m flagged, **0** collateral |
| every OFF arm | — | 0 | 0 |

No backstop: card R's reference gate reads `3600 opp, 1 flag` identically on all four legs (it tests the first chain
only), T4 is 0, and the collateral satellites have no `T0_band_scope` row to narrow.  Cost: blast radius at k = 0 OFF
1 product key (`P/G24/L5X`), PIN 2 — the extra is `P/G32/L1C`, an innocent satellite's phase bias re-realized
network-wide by **13.71 / 13.73 / 13.73 m** on E1 / E2 / E3g24 (declared — disc 1 → 2, σ 0.039 → 0.449 m — so the
declaration edge holds, but the response is wrong); GOPE ZWD at +45 2.02 m PIN vs 0.25 OFF on E1; consumer mean
|Δ3D| over +30..+50 E1 4.71 vs 0.98, E2 3.16 vs 1.18, E3g24 2.80 vs 2.43 — OFF recovers by +15, PIN does not.  Field
exposure: 12.7 % of IF-TD samples sit on a reference arc, 25.9 % of (station, satellite) arcs are a chain reference
at least once over 6 h, and of 149 flagged rows zero are on a reference arc (the detector cannot flag there); under
`all` the exposure roughly triples (§22.4: 107 of 507 arcs).

| event | G-A | G-B | G-C | G-D | fan-out PIN / OFF |
|---|---|---|---|---|---|
| E1 | PASS | FAIL (clk, zwd) | FAIL (PB L2X) | FAIL (4.71 vs 0.98 + 1.99) | 2 / 0 |
| E2 | PASS | FAIL (zwd) | PASS | PASS (3.160 vs bar 3.168) | 1 / 0 |
| E3/G24 | PASS | FAIL (zwd) | FAIL (PB L1C / L1W / L5Q) | PASS | 2 / 0 |
| E3/G25 | PASS | PASS | PASS | PASS | 0 / 0 |

**G-E fails its numeric bars for a defensible reason — the counter-weight.**  Withheld 0 / 0, `attrib_arc` 0 / 0,
but wire CLK max |Δ| 0.088 m (bar 0.02) and consumer max |Δ3D| 4.43 m (bar 0.03).  The floor series is EXACTLY zero
on every channel through k = +38; at +39 (tow 278790) JPLM00USA/G10 shows `L1C/L5Q dd = 5.046 m` against a 0.109 m
threshold on a 189-sample mature pool → flagged and refloated in PIN, while in OFF the same dd appears with
`count = 0, med = dd, abs_dev = 0, flagged = 0`: G10 lost L2W that epoch, its "first pair" rotated to (L1C,L5Q), and
the new chain restarted with the slipped sample as its own origin.  Every other satellite at JPLM is quiet (≤ 0.004 m)
and the reference G28 is quiet: **a real 46σ slip that `all` catches and `first` structurally cannot** — a
pool-restart blind spot at pair rotation, the second defect this card names (under `first` the additional chains'
histories do not exist; under `all` they are warm).

**Decision.**  Scope + pairs `all` + the clause stay FIELD EXCEPTIONS with the fan-out as the mechanism (the manifest
texts are rewritten through the generator; the ledger rows carry this section); G-G not run.  The pin is re-run
(§20's bars on the fixed posture) once REF-FAN (§22.6) passes F1–F4 — and the pair-rotation blind spot is the
argument FOR `all` once the fan-out is closed.  Graders merged: `scripts/fp1_gate_table.py`,
`scripts/fp1_refslip_census.py`.

### 20.3 RESULTS — FP-2, the matrix on the FIXED posture: the 60-epoch matrix is a clean sweep, the 6 h twin refuses the pin on the response cost (2026-09-13 ~23:20 local; worker on worktree-agent-a5c519cce79cf30c2 `5800768`, merged; exe = the certified `AF418A9F…`; seed `evseed_field6h_v8` ep0604; harness `e6071f43…` unmoved for the batch; manager grade)

**Legs.**  {null, E1, E2, E3/G24, E3/G25, E5} × {OFF, PIN = scope + pairs `all` + the clause + `if_phase_td_ref_arm_all`},
all 12 clean (61/61, no cold start, RS-8 / RS-9 restores on, the four ARMED lines present on every PIN leg, every pair's
config diff = the four knobs + paths).

**G-A PASS on every row** (E1 `keep implicated=L5X` with both L5 pairs `ref_arc`; E2 `all reason=no_exoneration`, the
clause's cost; E3/G24 `all` via `ref_arc`; E3/G25 `all` via the (1,5) veto, its three detections bit-identical to §22.7's;
E5 `keep implicated=L5X` via `lli=L5X`), T4 attribution 0 everywhere.  **The fan-out census: 0 collateral on every G24
event** (FP-1 read 6 / 3 / 6), the reference's own arcs reset instead (2 / 2 / 3 / 2), the innocent 13.7 m re-realization
absent; blast radius at k = 0 **1 key in both arms on every event**; the per-chain counter prices it (`fan-out suppressed`
3 → 9 on `e1_pin`, FP-1's six).  **G-B PASS 5/5** (every channel within 9 mm at a zero floor; E1 better on clock and iono);
**G-C PASS 5/5** (≤ 0.017 of the 0.123 m CLK floor); **G-D PASS 5/5** with the floor (cons_mean floor 4.29 m) — at a ZERO
floor E1 would be 6.5× better (2.34 → 0.36), E3/G24 better, E3/G25 a tie, **E2 and E5 worse (+0.65, +1.39 m mean)**.  The
two re-reads: the E3/G24 station clock at +45 is +1.4917 OFF / +1.4918 PIN (REF-FAN's miss does not reproduce as an arm
difference); the E5 consumer max reproduces in direction, smaller (2.75 → 3.61, +31 %).  **G-E:** withheld 0 / 0 and
`attrib_arc` 0 / 0 PASS; the numeric bars are not gradeable — the two nulls are exactly zero on every channel through
k = −8 and diverge at **k = −7** on §22.7's natural `G27/SCTB00ATA` reference fire, then PIN catches what OFF structurally
cannot (MIZU/G06 at +21; **JPLM/G10's 46σ (1,5) slip at +39, which OFF carries on the wire for 8 epochs** before its own
156 m detection at +47; VOIM/G18 at +47).  Every phase-bias step > 0.5 m is DECLARED in all 12 legs (a new wire-step
census, `scripts/`); the undeclared steps are the known clock-channel gap (`emit_clock_disc` off), identical in both arms.

**G-G the twin (`evseed_field6h_v8fp2`, 6 h, the four knobs) — FAIL.**  PASS: 720 / 720 emitted = v8, `attrib_arc` 0 = 0,
**fingerprint `545b8bdd2b638231` = v8's (none of the four knobs is fingerprinted)**, the F1 flood bar at scale
(additional-chain fires 12 own + **20 inherited** = 32 vs the first chains' 21; `L2W_L5X` 704 candidates → 4 fires) — **the
adopted-reference path's first field validation**, the half §22.7 left open.  Work done: scope narrowings 23 → 55,
ref-arc refusals 0 → 143 across 10 pair kinds, pairs evaluated 340 k → 689 k.  FAIL: consumer de-biased settled h3–6 p50
**+18 % (mean-debiased) / +22 % (median)**, rms **+31 / +33 %** (bar 5 %); `pbstep` epochs carrying |step| > 0.05 m 0.118 →
**0.151 (+28 %)**, p90 max step 0.257 → **0.733 m** (bar: not worse); per-hour the PIN consumer is BETTER in h0–2 and worse in
h3–5.  Mechanism: **+107 refloats over 6 h**, each a declared phase-bias re-realization on the wire that the float,
undifferenced WTZZ consumer re-converges through — the detector side is right (it catches real slips `first` cannot see)
and the RESPONSE is what costs.  Neutral: `pb_reset` 29 = 29, `clk_disc` 6 = 6.  Anomaly worth its own leg: the first
chain's OWN decisions drift under `all` at 6 h (`L1C_L2W` 113/110 → 107/104, `L1C_L2X` 15/15 → 8/8) though identical over 60
epochs — §14.10.4's "pure addition" holds structurally, not after slow state feedback; a single-knob 6 h leg would
attribute it.  Also: a PIN-only ≈ 1.9 m declared `P/G30/L2W` re-realization at a leg-dependent epoch (a marginal trigger,
not an event response); the 0.5 m consumer shock instrument is saturated (600 / 600) on this consumer.

**Decision.**  The four stay FIELD EXCEPTIONS, now with the measured mechanism (the manifest texts rewritten through
the generator; the four ledger rows carry this section).  What would change it: a cheaper slip response — the
lane-preserving refloat (SPEC_lane_slip_response's open build) or an emitted-level-preserving re-tap — or a consumer that
carries its state across DECLARED steps; when either lands, the G-G twin is re-run on that posture and all four are
pinned together.  The fan-out fix itself (REF-FAN) stands validated on both beds and pinned on the sim golden.

