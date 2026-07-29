# REPORT — clock-gauge realization A/B: pivot pin vs sum-to-zero (SPEC_s2z_realization_ab)

Execution record for `docs/SPEC_s2z_realization_ab.md`.  Lineage:
`docs/REPORT_night305_day129_investigation.md` §Q2 →
`docs/REPORT_day129_frame_guards_phase01.md` (§6.1 is this card's D-A grading
pass) → this.  Tags: **[M]** measured here; **[H]** hypothesis.

**Verdict up front (decision rules D-D):** **H1 is FALSIFIED on the wire; the
outcome is H2 (redistribute on-wire) with a consumer-side nuance H2 did not
predict.**  Under sum-to-zero the 20:47:00 excursion still enters the emitted
CLK+CB split and still ratchets — 25 of 32 satellites sit outside their own
pre-event bands at end of day (worst +28.5 m, re-track NEVER) — so "the wire
comes out clean" is dead, and per D-D **the pin + Phase-2 guard posture
stands**; s2z-vs-ratchet is **CLOSED WITH DATA** and joins the
falsified-approaches archive with this report as evidence.  The nuance worth
keeping: the *consumer* damage de-ratchets (the reference arm-A 46 m
era-to-EOD becomes an ~23 m transient that decays to quiet by 23:00, at the
price of the second signal pair transiently paying 2.4× more), and the onset
misfit localizes to a single station (GOLD00USA, z ≈ 1.0e5) — an attribution
lead the pin realization structurally could not show.  No default changes.

---

## 0. Execution shape, and one deliberate deviation

Phase 0 (pre-flight), the 10-epoch smoke, and the production run all executed
config-only on the same Release binary as the guards chain
(sha256 `7859D68F7DD13BE8…`, `gnss_processor_s2z.exe`).  **No code changes.**

**Deviation from the spec's seed+leg shape [M]:** the arm ran as ONE
continuous segment 13:00:00→23:59:30 (1320 epochs, `max_duration_s` 39600,
no 19:00 seam).  Decided after this card's D-A grading pass measured the
guards chain's 19:00 warm-resume seam as materially non-inert on exactly this
lineage: the diagonal-only restore blew a00 to 1410 TECU at the leg's first
epoch, the SH gate withheld the wire 19:00→20:59:30 continuously, and
**neither evening excursion expressed** (clock-median Δ at 334020 = +0.31 m
vs the reference's 4581.9 m; σ̂0_pf_code z ≈ −1).  A seamed s2z arm would have
risked observing seam recovery instead of the disease.  The continuous form
also shrinks the spec's own stated confound — carry age at 20:47 becomes
7.8 h (τ = 3.79 h ⇒ variance ~settled) instead of 1.8 h — and costs only the
leg-only traces the spec explicitly marked skippable (`trace_sh_spectrum`
exonerated, `trace_dd_fractional` answered).  The reference run this A/B
grades against is itself continuous through both events.

Run facts [M]: rc 0 · 1320/1320 epochs emitted (no withholds — guards OFF per
D-C, dispositions all `emitted`) · 0 error/critical lines · ~44 s/epoch under
concurrent machine load · fingerprint `5a728ca9a9a3c98b` (the `clkdatum:s2z`
conditional append; pin lineage reads `24bf911686949cc3`) · EOD snapshot
archived `runs/s2z_ab/seed_immutable.txt`.

Phase-0/smoke gates, all PASS [M] (`scripts/s2z_smoke_check.py`, 14/14):
loader sweep clean — the SUM-TO-ZERO activation line present and **zero**
"IGNORED/INERT under clock_datum_sum_to_zero" warns (the base config carries
none of the pivot-coupled knobs; `scripts/s2z_make_config.py` asserts this
rather than trusting it); `declared_dim ≡ 0` with `gate_ok = solved = 1` on
every row — the Σ-rows fold at weight 1e3 pre-gate exactly like the code-bias
gauge, realized common-mode eigenvalue ≈ N·1e3, seven decades above the
rank-gate cutoff, so the brdc-anchored silent-empty-product mode structurally
cannot recur; first epoch exactly 13:00:00 (half-epoch back-off held); keyed
landing-site trace live with STA/SAT/SIG pools.  The s2z-aware clock-monitor
jump baseline (`NetworkClockQc.cpp:355`) is present and correct by read but
**dormant on this arm** — `network.clock_monitor` defaults false and the arm
is deliberately unguarded; posture identical to the reference run by
construction (derive-from-base).

Config delta over `_night305_conv.json`, verified by flattened diff [M]: the
treatment, the 13:00−15 s anchor, s2z-owned snapshot/product paths, and seven
fingerprint-SAFE trace knobs.  Nothing else.

## 1. P1 — where the inconsistency lands

**(a2) The gauge is real and held through the event [M].**  Per-epoch mean of
the solved satellite clocks: |mean| median 3 µm, max 23 µm over all 1320
epochs — including exactly 0.000000 m at the onset epoch.  (The spec's P1(a)
criterion as written — "the solved sat-clock MEDIAN must not jump" — is
ill-posed: Σδtˢ = 0 pins the MEAN; a skewed redistribution moves the median
freely.  Measured: the median jumped −258.45 m at 334020 while the mean held
at zero.  The exact-constraint check `s2z_p2_grade.py --legdir` P1(a2) is the
well-posed form of the spec's intent, and it PASSES.)

**(d) The excursion replicated — it is physical, not gauge [M].**  SH slant
panel at 334020: `slant_resid_mean` 65.6 → 434.9 m, rms 172 → 1540 m, max
31 180 m, LOS >3σ 1617 → **1784 of 1818** in one epoch, recovering over
~4 epochs.  Same single-epoch, network-wide signature class as the reference
(§4.5 of the frame-guards report measured 303/303 stations at the 07:32
event).  The "arms saw different data windows" stop-condition does not
trigger.  (a00 itself read differently — already at 435 TECU pre-onset,
stepping only −29 through it — consistent with the Jul-27 finding that a00 is
datum-coupled; the slant extent is the valid replication read.)

**(b) The onset misfit does NOT land on MBAR00UGA — it concentrates on
GOLD00USA [M].**  Keyed post-fit robust-z at 334020
(`p2_gate_verdict.py --legdir runs/s2z_ab/seed`):

| key | s0_code baseline → onset | z |
|---|---|---|
| **STA GOLD00USA** | 2.73 → **96 897 m** | **101 184** |
| SAT G01 | 4.88 → 4 487 | 2 397 |
| SAT G18 | 5.40 → 3 100 | 2 707 |
| STA JPLM00USA | 2.64 → 3 352 | 2 920 |
| STA DUBO00CAN | 2.92 → 2 068 | 1 925 |
| SAT G12 / G25 / G05 | → 1 534 / 1 739 / 1 891 | 3 334 / 2 799 / 1 887 |
| STA MBAR00UGA (H1's site) | 6.1 → 110 | small |

One-epoch profile at GOLD (n_code stable 31–35 throughout — no tracking
loss): 2.96 → 96 897 → 52.8 → 39.3 → ~5 within 8 epochs.  The iono-deweight
gate independently engaged on GOLD00USA at exactly 334020 (statistic 39.7 vs
threshold 3.0).  **GOLD's raw RINEX is clean through the onset [M]**: C1W
second differences smooth and magnitude-identical to JPLM's, geometry-free
C1W−C2W stable at TEC scale, code-minus-phase stable at metre scale, no gap —
and GOLD does not even track G01 there, while the G01 SAT pool spiked
hardest, so the concentration is **solution-side coupling** (whose rows
resist the moved shared states), not a station data fault.  H1's specific
prediction (pressure lands in the datum receiver's local states) is falsified
alongside; the multi-SAT spread is the H2 signature.  The 29× dominance of
one station over its 200-km neighbour is a NEW, unexplained localization —
recorded as a follow-up lead for the trigger question (Q1 lineage), not
adjudicated here.

**(c) σ̂0_pf_code jump [M]:** 4.32 → **288.18** at onset (own-baseline robust
z = 350), decaying 10.8 / 8.2 / 6.7 / 6.3 / 6.1 over the next five epochs.
The guarded-pin leg's same statistic at the same tow read z ≈ −1 — not
comparable as an event measurement (its event never expressed; §6.1), but it
bounds the comparison the spec asked for: on lineages where the event
expresses, the code-pool misfit is enormous and brief.

**The 19:21:30 event did not express on this lineage [M]** — σ̂0 z = 1,
clock-median Δ −0.06 m, slant extent at its chronic background — while a
distinct disturbance at **19:34:00** did (G07 total-IF −1836 m transient,
detector-statistic median|ΔCLK| 84 m, keyed STA panel z ≈ 5–9 spread across
European-sector stations).  The guards chain's largest deltas sat at 19:34:30
too.  Event expression is a function of the accumulated filter state, not of
the observation data alone — both 13:00-lineage arms agree on this against
the 00:00-lineage reference.

## 2. P2 — wire damage: redistributed, not removed

Between-satellite total-IF deviation from the per-epoch constellation median
(`s2z_p2_grade.py`, network_solution ≡ the wire on this ungated arm; every
number datum-invariant, invariant #5):

* **G01: peak +8 681 m at the onset epoch; +46.8 m at +10 ep; +31.6 m at
  +1 h; +28.5 m at EOD; re-track NEVER** (band ±2 m).  Non-exponential decay
  (fast first hour, then nearly flat) — the carry stores the one-epoch insult
  exactly as on the pin.
* **25 of 32 satellites end the day outside max(2 m, 3σ) of their own
  pre-20:47 quiet band** [M]: +28.4 (G01), +28.0 (G23), +27.7 (G24), +24.0
  (G10), +21.2 (G18), +18.8 (G29), +18.2 (G32), −16.7 (G17), tail to ±10 m.
  Versus the pin reference: ONE satellite at +42.7 m with the constellation
  otherwise intact (handoff §4.3's uniform-offset signature).  Same order of
  total damage, spread differently — the H2 branch, including its "every
  satellite slightly wrong" clause.
* Hourly between-sat spread: 5.0–8.3 m quiet; 10.4 m in hour 20; back to
  6.7 m by hour 23 — the SPREAD recovers while the per-sat LEVELS stay
  displaced, i.e. the constellation re-arranged coherently.
* Per-signal CB channel sums step at membership changes (hr 15, 18) and are
  not interpretable as damage without membership control — recorded, not
  graded.  Caveat: the "quiet band" 19:45→20:46:30 itself sits inside a
  mildly elevated era on this arm (SH slant mean ~65 m vs 14 m at 19:21), so
  the EOD offsets are if anything understated.

## 3. P3 — consumer: the ratchet does not survive, the redistribution does

Q5 float-consumer replays (NRMG00NCL, ITRF2020 truth both sides — reference
arms re-anchored via `q5_reanchor_hourly.py`), hourly median 3D / mean-up, m,
UTC-aligned [M]:

| UTC | ref-pin A (C1W,C2W) | ref-pin B (C1C,C2L) | s2z A | s2z B |
|---|---|---|---|---|
| 14–16 (quiet) | 7.6–8.9 | 4.7–7.4 | 8.6–10.3 | 3.5–5.4 |
| 17 | 8.1 | 5.3 | 16.2 (+16.0 up) | 2.9 |
| 18 | 11.6 | 5.9 | 17.4 | 4.7 |
| 19 | 21.6 | 8.6 | 22.4 | 14.6 |
| 20 | 24.8 | 7.9 | 22.9 | **21.2** |
| 21 | **46.0** (−20.5 up) | 14.6 | 18.3 | **34.6** |
| 22 | **46.8** (−34.8 up) | 7.5 | 16.8 | 10.9 |
| 23 | **41.8** (−30.6 up) | 9.9 | **12.8** | **8.0** |

The pin's signature consumer harm — a signal-pair-specific 45 m era that
never recovers before end of day — **is eliminated**: s2z arm A peaks ~23 m
and decays to its quiet floor by 23:00.  The redistribution's price: the
"datum-pinned" pair (arm B) transiently pays 2.4× the reference (34.6 vs
14.6 m at hour 21) — under s2z no pair is protected, both are moderately hit,
and both recover.  A float BSSD consumer largely absorbs a ±16–28 m pattern
spread over 25 satellites, where it could not absorb one satellite at +43 m.
(s2z arm A also shows an unexplained 16–17 m era at UTC 17–18, pre-events,
vertical-dominated, arm-B clean — coincident with hr-18 channel-sum
membership steps; recorded as an open observation.)

## 4. Design consequences recorded (spec's standing ask)

1. **Phase-2.1 detector realization-coupling — measured, and the spec's
   prediction is overturned in an instructive way [M].**  The spec predicted
   the median-|Δclock| statistic goes ~blind under s2z (the Σ row kills
   common jumps).  Measured at 334020: median|ΔCLK| = **257.7 m** — 5× the
   50 m threshold; 19:34 reads 84.3 m; exactly 4 epochs on the whole arm
   exceed 50 m, all in the two event eras (arm-wide median 0.103 m, q99
   2.05 m).  The THRESHOLD statistic survives the gauge change for this event
   class, because the excursion's between-satellite content is enormous once
   the common mode is annihilated.  What the gauge DOES destroy is the
   **one-sided discriminator**: max/med reads 1.23 under the pin (the
   common-mode fingerprint that predicts harm) but 32.6 under s2z —
   indistinguishable from a two-sided event.  Any future guard logic keying
   on common-vs-differential shape is realization-coupled; the plain
   threshold is not.
2. **The 19:00 warm-resume seam is not inert on 13:00 lineages** (measured on
   the guards chain, avoided here by the continuous shape) — recorded in the
   frame-guards report §6.1 with the seam-hardening follow-up owed there.
3. **Event expression is filter-state-dependent** (19:21:30 mute on both
   13:00-lineage arms; 19:34 disturbance instead).  Any future
   trigger/guard-validation run must reproduce the event on ITS OWN lineage
   before reading a guard's silence as success — this is invariant #4's
   "armed-but-never-tripped" WARN operating at the experiment-design level.
4. **GOLD00USA onset concentration** (z ≈ 1.0e5, raw data clean, deweight
   gate agreeing) — new Q1-lineage lead: what couples one station's clean
   rows to the moved shared states 29× harder than its 200-km neighbour?

## 5. Standing-invariant pre-flight, as executed

1. Zero-q/stiff event response — arm is deliberately UNGUARDED, diagnostic
   observation of the disease, reference-run class.  **NOT field-runnable; no
   posture conclusions from this arm.** ✔
2. Release-only, renamed exe (`gnss_processor_s2z.exe`, same-build hash as
   the guards chain), name-scoped zombie pre-kill before launch. ✔
3. Deferrals triaged: matched pin-unguarded 13:00 control (D-B2) NOT run —
   contingency was conditioned on P1/P2 ambiguity, and both endpoints
   resolved unambiguously (constraint held exactly; damage redistributed and
   persisted).  Field-inert for this card's question. ✔
4. Run-end counters: 1320/1320 emitted, keyed trace rows live all epochs,
   producer-complete line clean, supervisor verdict COMPLETE. ✔
5. Grading datum-invariant throughout (between-sat total-IF; consumer A/B on
   a fixed truth; the exact-Σ check is a constraint residual, not a frame
   read).  Raw frames never compared across realizations. ✔
6. No code changes; byte-identity gate not triggered. ✔

## 6. What I would not claim from this

* Nothing here validates s2z as a FIELD posture — the arm ran unguarded by
  design, and D-D closes the realization question in favour of pin+guards.
* The consumer de-ratcheting is measured on ONE float consumer at ONE site
  with AR off; an AR consumer or a network-RTK-style user could weight a
  25-satellite ±20 m redistribution very differently (H2's own caveat).
* The carry-age confound is smaller on this arm (7.8 h vs the spec's 1.8 h)
  but not zero vs the reference's 14.8 h; magnitude ratios (e.g. +28.5 vs
  +42.7 EOD) carry it.  The landing-site and consumer STRUCTURE reads do not.
* GOLD-vs-JPLM 29× is unexplained; "solution-side coupling" names the
  category, not the mechanism.
* The UTC 17–18 s2z arm-A era is unattributed.

## Reproduce

```bash
python scripts/s2z_make_config.py --smoke
python scripts/s2z_smoke_check.py
python scripts/s2z_p2_grade.py --legdir runs/s2z_ab/seed --events 328890 334020
python scripts/p2_gate_verdict.py --legdir runs/s2z_ab/seed --events 328890 334020
python scripts/q5_make_replay_arm.py --arm A --outdir runs/s2z_ab/q5_armA --products runs/s2z_ab/seed/products.txt
```

Artifacts: `runs/s2z_ab/{smoke,seed,q5_armA,q5_armB}/`, chain log
`runs/s2z_ab/chain.log`, EOD snapshot `runs/s2z_ab/seed_immutable.txt`,
products `runs/s2z_ab/seed/products.txt` (13:00→23:59:30, 0 gaps).
