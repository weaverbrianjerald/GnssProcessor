# Study 1: erase vs refloat, what costs the 0.45-cycle floor

**STATUS**

- **id:** Study 1
- **topic:** harness
- **status:** closed
- **summary:** "The erase path is the expensive one" is FALSIFIED: freeing every band of a LOS buys the floor; scoping either path to two of three bands cuts it 0.445 → 0.050 cyc.
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.inject.events` (`arc_erase` diagnostic `mode` and `signal` band scope, injector-only); new PASS-A drain `networkSlipDrainInjectedFree`
- **hashes named in the text** (commit / exe / byte-cert): byte-cert `products.txt` `95DC556F…` (`runs/evleg_s1_nullgate` = `runs/evleg_null`), `7027918C…` (`erase`, `erase_keep_iono`, `erase_no_funnel`, byte-identical)
- **run dirs named in the text:** `runs/evseed_quiet6h`, `runs/evleg_null`, `runs/evleg_s1_nullgate`
- **source:** `docs/archive/SPEC_event_harness.md` §9 (9.1 to 9.7)

**Outcome.** The study asked which action a T4 erase performs, and a detector refloat does not, costs the floor. None does: `free_epoch`, a literal detector refloat, reproduces it (0.496 cyc on arc 1); both paths re-level the LOS from one epoch of code (invariant #1's re-coupling edge). The LOS-iono co-erase and C3c funnel are inert on the sim golden; their `carry_iono` / `carry_invariant` leg is named as owed. The floor scales with elevation (0.130 cyc at 81.3°); no QC tier re-flags the arc. The production knobs built on this, `raim_erase_band_scope` and `cycle_slip.refloat_band_scope`, are both KEEP_OFF ([Study 3](../slip/STUDY-3-band-scoped-responses.md)): the mechanism stands, that cure does not.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 9. Study 1 — erase vs refloat: what actually costs the 0.45-cycle floor

**Answer, up front: none of the four actions the T4 erase performs and a detector
refloat does not. The floor is bought by the BAND SCOPE — freeing (or erasing)
ALL of a satellite's signals at one station at once. Restrict either path to 2 of
3 bands and the 0.445-cycle floor becomes 0.050.** Measured 2026-09-10, 14 legs,
~14 min of wall clock.

### 9.1 The question

§6(2) and §6(3) measured, on the SAME healthy arc, that a detector-handled
10-cycle `slip` leaves ~0.04 cyc of residual level error while an `arc_erase`
leaves a peak −1.9 cyc and a persistent ~0.45 cyc floor from +14 through +45 —
11×, with ZWD +3.8 mm. On the slipped storm bed the attribution tier erases ~21
arcs per station-hour, so that floor is the ambient 1.7-cycle plateau priced per
event. Which extra thing does the erase do?

### 9.2 The exact difference list (read from the source, not inferred)

`refloat_sat_at` in `src/core/filter/NetworkRaim.cpp` (~:751-880), per matching
ambiguity of the flagged (station, satellite):

| # | action | T4 erase | detector refloat (`refloat_signal`, `slip/NetworkSlipQc.cpp` ~:415-470) |
|---|---|---|---|
| 1 | `LocalCarry` entry | **`local_carry.erase(...)`** — deleted; the re-stash is a NEW entry (birth reset, `var_first` reset, F3 block membership restarts) | **KEPT** |
| 2 | LOS iono carry | **`ctx.carry.iono_carry.erase({i, sat})`** (`n_iono_carry_erased_t4`) | not touched |
| 3 | pending-reseed | `markAmbiguityPendingReseed(amb_key)` | `markAmbiguityPendingReseed(amb_key)` — **the same call** |
| 4 | C3c funnel | `raim_refloat_pending.insert(...)` under `raim_ci_refloat`, consumed NEXT epoch | `carry_slipped_ambs.insert(...)` under `carry_invariant`, consumed THIS epoch |
| 5 | free-epoch latch | **none** — the RAIM path never touches `free_ambs` | **`relevel->free_ambs.insert(...)` + `free_cause`** — the set the PASS-B leveling inject actually reads |
| 6 | bridge pend | none | voids any outstanding `relevel_pend` on that (station, sat) |
| 7 | seam / timing | POST-fit (`networkPostFitRaim`), so the event epoch's own solve is untouched and the damage starts at +1 | PASS A, so the event epoch's own solve is already freed (visible at +0) |

Two of those are not differences at all: **(3) is the same call on both paths**,
and **(4) is the same idea on two vehicles**. The real asymmetry is (1) vs (5):
the erase *removes the level*, the refloat *keeps the level and skips injecting it
for one epoch*. Both end with the arc re-levelled from one epoch of data — which
is the point the measurement lands on.

### 9.3 The diagnostic sub-knobs (harness-only)

`network.inject.events[].mode` on kind `arc_erase`, default `"erase"` = the
production path, so an event that names no mode is exactly what it was before:

| mode | LocalCarry | LOS iono | pending-reseed | C3c queue | free-epoch latch | seam |
|---|---|---|---|---|---|---|
| `erase` (default) | ERASED | co-erased | marked | queued | no | post-fit RAIM |
| `erase_keep_iono` | ERASED | **KEPT** | marked | queued | no | post-fit RAIM |
| `erase_no_reseed` | ERASED | co-erased | **NOT** | queued | no | post-fit RAIM |
| `erase_no_funnel` | ERASED | co-erased | marked | **NOT** | no | post-fit RAIM |
| `free_epoch` | **KEPT** | KEPT | marked | detector's | **YES** | PASS-A drain |

Plus an optional BAND SCOPE: `"signal": "L1C;L2W"` restricts the action to those
signals (empty / `"all"` = every signal, which is what BOTH production paths do).

`free_epoch` is routed to a new PASS-A drain, `networkSlipDrainInjectedFree`
(`slip/NetworkSlipQc.{hpp,cpp}`, called from `ApplicationNetworkAssembly.cpp`
beside the other slip drains). It has to live there, not in the RAIM block:
`relevel.free_ambs` is cleared at the top of PASS A and read by the PASS-B
leveling inject, so a `free_ambs` insert from post-fit RAIM would never be seen.
Run-end census `Study-1 path census (mode=chords acted)` says which path ran —
invariant #4.

### 9.4 The legs

Seed `runs/evseed_quiet6h` `state_snapshot.txt.ep0665` (tow 279150), 61 epochs,
event at +10 (tow 279480), everything graded against the ONE null leg
`runs/evleg_null`. Three arcs, all GPS triple (L1C / L2W / L5Q):

| arc | station / sat | mean elevation |
|---|---|---|
| arc 1 | `S21000SIM` / `G10` (the §6 validation arc) | 14.2° |
| arc 2 | `S50000SIM` / `G31` | **81.3°** |
| arc 3 | `S40000SIM` / `G31` (arc 2's satellite, low station) | 13.8° |

**Two structural gates first.**

* **Byte-identity (invariant #6).** `runs/evleg_s1_nullgate` — a NULL leg on the
  new exe — reproduces `runs/evleg_null`'s `products.txt` SHA256 exactly
  (`95DC556F1EDBB6EBB06D2E6BDC03282F9169887C63BBCEF0…`). The sub-knobs are
  unreachable unarmed, and the §6 validation legs stay comparable.
* **Two of the four actions are STRUCTURALLY INERT on this lineage, and the
  harness proves it rather than assuming it.** `erase_keep_iono` and
  `erase_no_funnel` are **byte-identical to `erase`** (`products.txt`
  `7027918C0DE61D0E11C67646AB023A1179B2AB03…` for all three), because the sim
  golden runs `carry_iono: false` (⇒ `ctx.knobs.iono_on` false, so there is no
  iono carry to co-erase) and `carry_invariant: false` (⇒ `raim_ci_refloat`
  false, so nothing is queued). **The LOS-iono co-erase and the C3c funnel
  therefore CANNOT be the cause of the measured floor** — they never execute.
  What their arms certify is the mode dispatch: exactly the named action moved
  and nothing else did. On a `carry_iono` / `carry_invariant` lineage they are
  live and untested — that is an owed leg, not a closed question.

### 9.5 The table

Arc level error is the chord-median DD error of the arc, **event − null**, in
cycles, from `logs/network_dd_fractional.csv` joined to the fixture's true
integers (`scripts/ev_study1_table.py`). `floor` = mean |e| over +14..+45. ZWD
and iono are event − null too; iono is `iono_total_m` for that (station, sat)
at +45. `T4/T3` counts post-event re-flags on the arc inside 45 epochs.

| leg | peak \|e\| (cyc) | @ep | +14 | +30 | +45 | **floor** | ZWD pk (mm) | Δiono +45 (m) | T4/T3 | cons pk (m) |
|---|---|---|---|---|---|---|---|---|---|---|
| **arc1** `slip` 10 cyc (detector freed L1C;L2W) | 0.054 | +11 | 0.051 | 0.044 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | −0.001 |
| arc1 `erase` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_keep_iono` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_no_reseed` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_no_funnel` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `free_epoch` (all 3 bands) | 2.018 | +0 | 0.515 | 0.633 | 0.447 | **0.496** | +4.0 | −0.0890 | 0 | −0.001 |
| **arc1 `erase` scoped L1C;L2W** | 0.067 | +1 | 0.059 | 0.050 | 0.041 | **0.050** | −1.9 | +0.0061 | 0 | −0.001 |
| **arc1 `free_epoch` scoped L1C;L2W** | 0.054 | +11 | 0.051 | 0.044 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | −0.001 |
| arc2 `erase` | 1.052 | +1 | 0.270 | 0.067 | 0.073 | **0.130** | −4.2 | +0.0206 | 0 | −0.011 |
| arc2 `erase_no_reseed` | 1.052 | +1 | 0.270 | 0.067 | 0.073 | **0.130** | −4.2 | +0.0206 | 0 | −0.011 |
| arc2 `free_epoch` (all 3) | 0.434 | +1 | 0.213 | 0.088 | 0.077 | **0.116** | −3.8 | +0.0221 | 0 | −0.012 |
| **arc2 `free_epoch` scoped L1C;L2W** | 0.019 | +1 | 0.007 | 0.004 | 0.003 | **0.004** | −1.8 | −0.0004 | 0 | −0.005 |
| arc3 `erase` | 0.803 | +1 | 0.448 | 0.379 | 0.207 | **0.375** | −2.0 | +0.0405 | 0 | −0.001 |
| arc3 `erase_no_reseed` | 0.803 | +1 | 0.448 | 0.379 | 0.207 | **0.375** | −2.0 | +0.0405 | 0 | −0.001 |
| arc3 `free_epoch` (all 3) | 0.498 | +10 | 0.372 | 0.325 | 0.179 | **0.319** | −2.5 | +0.0353 | 0 | −0.001 |

Per band on arc 1 (floor +14..+45), which is where the mechanism is visible:

| band | slip (det. L1C;L2W) | erase all-3 | free all-3 | erase L1C;L2W | free L1C;L2W |
|---|---|---|---|---|---|
| L1C | 0.044 | 0.445 | 0.496 | 0.050 | **0.044** |
| L2W | 0.052 | 0.598 | 0.664 | 0.061 | **0.052** |
| L5Q | 0.042 | 0.630 | 0.698 | 0.046 | **0.042** |

### 9.6 Verdict

1. **No single erase-path action produces the floor.** Removing the iono
   co-erase, the C3c queue, or the pending-reseed mark leaves the floor at 0.445
   on arc 1 (the first two byte-identically; `erase_no_reseed`'s products differ
   but every graded column agrees to three decimals). Erasing the `LocalCarry`
   entry is not it either: **`free_epoch` — a literal detector refloat that keeps
   the entry, keeps the iono, queues nothing and latches the free epoch —
   reproduces the floor and is marginally WORSE on arc 1 (0.496 vs 0.445).** The
   premise "the erase path is the expensive one" is falsified. Both paths cost
   the same because both end in the same place: the arc re-levelled from one
   epoch of data.
2. **The variable that matters is BAND SCOPE.** Restricting either path to 2 of
   the 3 bands drops the floor 9× (erase 0.445 → 0.050) and 11× (free 0.496 →
   0.044) on arc 1, 29× on arc 2 (0.116 → 0.004), and does it on all three bands
   at once — including L5Q, which was never freed. The scoped `free_epoch` leg
   reproduces the real-`slip` leg to three decimals on every band (0.044 / 0.052
   / 0.042), which is the identity check the mechanism predicts: the §6(2) slip
   was cheap **because its `T1_if_slip` detector acted on `L1C;L2W` and left L5Q
   pinned**, not because a slip response is intrinsically gentler than an erase.
3. **The mechanism.** Freeing every band of a LOS removes that LOS's last level
   anchor, so (ρ, I, N₁, N₂, N₃) must be re-drawn together from one epoch of
   code — the amb/iono split over-count (`SPEC_amb_iono` F1) at its worst, and
   standing invariant #1's re-coupling edge verbatim: *"a refloat that re-levels
   from ONE epoch of code CREATES a fresh zero-q value seeded with
   station-common iono error."* The iono column is the direct witness: −0.083 m
   of slant-iono error still standing at +45 on the all-3 arms against +0.006 m
   scoped. Leave ONE band pinned and the two re-tapped levels land on the
   survivor's ionosphere instead of on code noise.
4. **It scales with elevation, not with the path.** floor(erase): 81.3° arc
   0.130, 13.8° arc 0.375, 14.2° arc 0.445 — the one-epoch re-level is only as
   good as that epoch's code, and a low LOS has the worse code.
5. **The QC tiers stay silent.** 0 post-event T4/T3 re-flags on the arc in every
   leg, on both paths: the arc that has just been given a metre-class level error
   is not flagged for it. Invariant #7's re-coupling edge with no instrument.
6. Consumer 3D (event − null) is ≤ 12 mm on every leg, so one event is a wire
   defect long before it is a consumer defect; the plateau is the accumulation of
   many.

**The production change this points at** (not made here — the sub-knobs are
injector-only): band-scope the T4 attribution erase and the whole-satellite
detector refloats — refloat the implicated bands and leave the least-suspect
band's level pinned as the LOS's iono anchor — which is the "band-scoped
refloat" already named as the open build in `SPEC_lane_slip_response`.

### 9.7 Reproducing

```powershell
python scripts\ev_study1_legs.py make                       # 14 leg.json
python scripts\ev_study1_legs.py run  s1_free s1_keepiono s1_noreseed   # <=3 at a time
python scripts\ev_study1_table.py                           # the table above
```

Wall clock: 150–163 s per leg with 3 concurrent (211 s in the one wave that
overlapped another session's build), 14 legs in ~14 min; Release build 379 s
(+123 s incremental for the band scope), Debug suite 367 s.
Unit suite after the card: **1716 cases / 241 suites, 1699 passed, 0 failed, 17
skipped** (fixture-gated; `data/` is not bootstrapped in this worktree) — the
card adds one epoch-semantics case (`ArcEraseStudy1ModeAndBandScope`) and two
parse-rejection entries.

**Caveats.** (a) One quiet sim bed, three arcs, 60-epoch legs — the floor is
measured out to +45, not to convergence. (b) The iono co-erase and the C3c
funnel are inert here; their arms are certification of the dispatch, not of the
actions. (c) `mode` gates the INJECTED erase only; a real attribution takes the
production path on every mode, and an unarmed run cannot reach any of it.
(d) The `free_epoch` legs move at +0 and the erase legs at +1 — that is the two
seams' real timing (PASS-A drain vs post-fit RAIM), not a grading offset.

---

