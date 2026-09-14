# Card G response — `gap_reentry_relevel` — round 1 was inert on the live-state walk, round 2's verdict, and bridge v3's flat result

**STATUS**

- **id:** Card G response
- **topic:** slip
- **status:** closed
- **summary:** `gap_reentry_relevel` — round 1 was inert on the live-state walk, round 2's verdict, and bridge v3's flat result
- **dates in the text:** 2026-09-09
- **knobs / posture:** `network.cycle_slip.gap_reentry_relevel`, `network.phase_leveling`, `network.carry_invariant`
- **hashes named in the text** (commit / exe / byte-cert): `be523008`
- **run dirs named in the text:** `runs/fldf3_pl2`, `runs/fldf3_pl2_ctl_new`, `runs/fldf3_pl2_gapfree2`, `runs/fldf3_pl2_gapfree`, `runs/fldf3_pl2_bridge`
- **source:** `docs/archive/SPEC_slip_qc_hardening.md` — 12, 12.1, 12.2, 12.3, 12.4, 12.5, 12.6, 12.7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_slip_qc_hardening.md -->
## 12. Card G RESPONSE — `network.cycle_slip.gap_reentry_relevel` (2026-09-09)

**One-line status: the DEFECT is closed and proven live; the KNOB is KEEP_OFF as a promotion on
field lineages.** The response removes the whole identified poison population (88 → 0 bridged arcs)
and every level/DD-side instrument improves, but the float consumer pays **+56 % settled rms**,
because a bare refloat re-levels each freed arc from ONE epoch of code — standing invariant #1's
RE-COUPLING edge, charged at 129 arcs / 12 h to kill 88 bridged ones.

### 12.1 The defect (Card G's own G3 rule, come back nonzero)

`runs/fldf3_pl2` (12 h, DOY-129, 63 field stations, the converging leveling posture of
`docs/SPEC_amb_iono_split_overcount.md` §6.15.4 probe 7), run-end Card-G line
(`runs/fldf3_pl2_ctl_new/console.txt`, verbatim):

> 453 RESUMPTION(s) … = 364 SHIFTED (raw MW/GF level moved across the break) + 31
> measured-and-unshifted + 58 UNMEASURABLE … SHIFTED → **88 arc(s) re-entered with BRIDGED carry
> state (THE POISON CASE)** + 640 fresh = 728; unshifted → 23 bridged … UNMEASURABLE → **24 bridged**
> … + 49 fresh = 73.

**Why no detector fires.** An MW/GF verdict needs `epoch_idx == prev + 1`
(`src/core/slip/NetworkRawObservables.cpp:221`) and the IF-phase time-difference tier likewise needs consecutive epochs, so a
RETURN epoch is structurally outside every time-difference detector's domain. Card G measured the
consequence and, by design, only counted it (`slip_marked` reads `carry_slipped_ambs`,
`src/core/app/ApplicationNetwork.cpp` Card-G block ~:1900-2000).

**The worked case.** BSHM00ISR / G22 at TOW 271200 (`runs/fldf3_pl2/logs/network_events.csv`): gap
7 epochs, ΔMW **−1250.98 m** (≈ 1451 WL cycles), ΔGF **−276.67 m**, LLI asserted on both bands of
the return epoch, classified `shifted_bridged … pl=1;dd=0;slip=0`, no `T0_mw_detect` / `T1_if_slip`
that epoch. The pre-gap zero-q level was re-injected at fold_var ~1e-5 m² and the solve absorbed it
along the wet-mapping null (ΔCLK/ΔZWD −1.35, R² 0.9996): that station's ZWD went
**166.495 → −2445.967 m** in one epoch (`runs/fldf3_pl2/logs/network_zwd.csv`, TOW 271170 →
271200; the control arm reproduces it exactly). Invariant #1 (stiff zero-q carry crossing a jump with no wired response) and
invariant #7 (a re-coupling edge that re-validates nothing).

### 12.2 The build

| piece | where |
|---|---|
| knob `network.cycle_slip.gap_reentry_relevel` (+ `_lli` default **true**, `_unmeasurable` default **false**) | loader `src/core/config/GnssConfigLoadNetwork.cpp:4087-4095`; struct `include/config/GnssConfigTypes.hpp:467/474/480`; per-epoch snapshot `include/slip/NetworkRawObservables.hpp:301/307/313`, wired `src/core/app/ApplicationNetwork.cpp:301-303` |
| loader warns (never disarms) | `GnssConfigLoadNetwork.cpp:224-262` — no `relevel_free_epoch` under a warm leveling carry ⇒ counted `gap_reentry_no_relevel`; neither vehicle armed ⇒ `gap_reentry_cold_skipped`, "armed-and-inert (standing invariant #4)" |
| the drain | `src/core/slip/NetworkSlipQc.cpp:1395` `networkSlipDrainGapReentry` (contract banner `include/slip/NetworkSlipQc.hpp:300-424`) |
| drain siting | `src/core/app/ApplicationNetworkAssembly.cpp:1587` — PASS-A slip band, after the wind-up restart drain, **before** the lane observatory's PASS-A half and before PASS B's inject, so the freed set is complete in the SAME epoch (a response one epoch late lets the stale level fold once, which is the whole defect) |
| counters (closure documented at the declaration) | `include/slip/NetworkRawObservables.hpp:424-496` — `gap_reentry_verdicts`, `_pivot_skipped`, `_arcs`, **`_no_carry`**, **`_state_absent`**, `_freed_{shifted,lli,unmeas}`, `_cold_skipped`, `_no_relevel`, `_dd_only`, `_dd_marked` |
| run-end RESPONSE clause (appended to the Card-G line, with the in-band ARMED-AND-INERT warning) | `src/core/filter/NetworkTeardown.cpp:2511-2560` |
| Card-G classification sees the response | `src/core/app/ApplicationNetwork.cpp:1913-1917` — the arc ledger is armed by the instrument **or** by the response, so a response-only arm cannot report its own effect as a structural zero |
| per-arc event identity | tier `T0_gap`, event `reentry_relevel`, detail `sig;gap_ep;dGF;lli1;lli2;cause;carry;state` (statistic = raw ΔMW in metres, **blank** when the break was unmeasurable — a fabricated 0.0 would read as "the level did not move") |
| run-manifest keys, emitted only when armed | `src/core/logging/NetworkRunTraces.cpp:1514+` — `gap_reentry_relevel`, `_lli`, `_unmeasurable`; verified present in `runs/fldf3_pl2_gapfree2/logs/network_run_manifest.json` |

Response = the DETECTOR response verbatim (C3c funnel insert under `carry_invariant`; pending-reseed
+ `free_ambs` latch with cause `kCauseGap` under a warm leveling carry). Nothing is repaired and no
integer is applied: across a loss of lock the slip magnitude is unbounded, so re-entry is treated as
code-anchored-fresh.

### 12.3 ROUND 1 was INERT — the live-state walk (the lesson)

`runs/fldf3_pl2_gapfree` (12 h, same posture) came back **byte-identical to its control**, and the
run-end clause said exactly why (`runs/fldf3_pl2_gapfree/console.txt`, verbatim):

> RESPONSE: gap_reentry_relevel ON — **396 re-entry(ies) judged SLIPPED** …; **0 arc(s) reached with
> a live ambiguity (775 had none)** = 0 shifted + 0 lli + 0 unmeasurable arc(s) FREED … ⚠ **ARMED AND
> INERT**: resumptions exist and ZERO arcs were reached — this is dead wiring, not a quiet run.

The first cut mirrored the MW/LLI drain's `refloat_signal`: walk `StateManager::getAmbiguities()`,
match (satellite, signal). That is correct for a detected slip on a CONTINUOUS arc and structurally
wrong for a RE-ENTRY — the column was pruned with the gap (`pruneAmbiguitiesExcept` inside
`ensureRequiredStates`, `src/core/measurement/MeasurementModel.cpp:784`, PASS B) and is re-created
only AFTER every PASS-A drain. **This is item 4 of §11's "Still OWED", reproduced inside the card
written to answer it.**

Fix: synthesise the key with `StateKeys::makeAmbiguityKey` (the inject site's own format, and the
one the Card-G classification synthesises, so the two instruments cannot disagree), and ask "is there
anything to protect?" of the **CARRY** — a `LocalCarry` entry for (station, satellite, signal) still
inside `phase_leveling_max_gap_s`, or the `carry_invariant` vehicle. `gap_reentry_no_arc` →
`gap_reentry_no_carry`; new orthogonal counter `gap_reentry_state_absent`; `markAmbiguityPendingReseed`
only when the state exists (a mark on an absent key would be consumed by a later epoch's pre-pass —
a response acting on history).

Process note: **the pre-registered P1 caught this, not the unit tests** — every fixture created the
ambiguity state first. A response card's tests must include the state-ABSENT / carry-PRESENT case,
which is the only case a re-entry ever presents.

### 12.4 ROUND 2 — arms and results (`runs/fldf3_pl2_gapfree2/VERDICT.md`)

Single knob, everything else byte-identical. CONTROL `runs/fldf3_pl2_ctl_new`
(`config/test_suite/_fldf3_pl2_ctl_new.json`; `products.txt` sha256 `be523008…` **identical** to
`runs/fldf3_pl2`), ON `runs/fldf3_pl2_gapfree2`
(`_fldf3_pl2_gapfree2.json` = control + the master knob), exe
`gnss_processor_gapfree2.exe` sha256 `2CAF5A24…`; both exit 0, 1440/1440 epochs.

Run-end clause, ON arm: **396 re-entries judged SLIPPED, 0 pivot-skipped, 129 arcs reached with a
PROTECTABLE carry, 646 with no carry to protect, 129 of 129 state-absent, = 109 shifted + 20 lli
+ 0 unmeasurable FREED**. Two closure identities pin the round-1 defect to the unit: `129 + 646 = 775`
(the pre-fix arm's own "775 had none") and `state_absent = 129 of 129` — a live-state match could not
have freed a single arc; the null was structural, not statistical.

| # | prediction | control | ON | verdict |
|---|---|---|---|---|
| P1 | SHIFTED→bridged 88 → ≤ 5 **and** ≥ 80 arcs freed | 88 bridged; RESPONSE OFF | **0 bridged; 129 freed**; 646 no_carry; 129/129 state_absent | **PASS** (both clauses) |
| P2 | BSHM ZWD @271200 within ±5 m of 271170; settled span 369 → < 20 m | 166.495 → **−2445.967** (Δ −2612 m); span **368.873 m** | 1.3148 → 1.2866 (**Δ −0.028 m**); span **33.373 m** | **SPLIT** — poison event PASS, span FAIL |
| P3 | fleet settled ZWD span p50 ≤ 0.80 m; > 10 m count 3 → ≤ 1 | p50 **0.797**, p90 6.829, > 2 m 14, > 10 m 3 (BSHM, COCO, CUSV) | p50 **0.660**, p90 **4.910**, > 2 m **12**, > 10 m **3** (BSHM, CUSV, **WROC**) | **SPLIT** — centre PASS, tail FAIL |
| P4 | consumer settled ≥ h6 ≥ 10 % better | rms **3.178**, p50 **2.416**, p90 5.090 | rms **4.946 (+55.6 %)**, p50 **2.879**, p90 8.520 | **FAIL — materially worse** |
| P5 | DD arc-wander tail (raw e1 drift > 1 cyc) 47 % → lower | 47.0 %, MAD p50 0.117; wl 28.5 % | **38.2 %**, MAD p50 **0.057**; wl **20.4 %** | **PASS** (all six statistics) |
| P6 | T4a attribution refloats 2032 → lower | **2032** / 665 332 opp | **1092** / 665 310 (**−46.3 %**) | **PASS** |
| P7 | withholds 0 → 0, solved 1440 | 1440 / 0 | 1440 / 0 | **PASS** |
| P8 | reported — prires worst \|t\| EXCLUDING the RNEG class (§ `docs/SPEC_prior_residual_census.md` §17) | **1.2649e5** at `pl/CLEAN COCO00AUS/G06/L1C @276150`; RNEG 32 177 rows, worst \|ê\| 139.44 m | **1.8172e5** at `pl/CLEAN COCO00AUS/G15/L2W @292740`; RNEG 33 644 rows, worst \|ê\| 142.73 m (same INEG row) | reported |

**Evidence split of the 129 freed arcs** (`runs/fldf3_pl2_gapfree2/logs/network_events.csv`, tier
`T0_gap` / event `reentry_relevel`, 129 rows): **95** with \|ΔMW\| > 0.5 WL cycles (p50 \|ΔMW\|
**18.095 m** — genuine breaks, not threshold grazing), **15** GF-only (gap p50 **23 epochs**, p50
\|ΔGF\| **0.447 m**), **19** LLI-only. By recorded cause: 57 `shifted`, 52 `shifted|lli`, 20 `lli`;
`carry=pl` on all 129 and `state=absent` on all 129.

Card-G classification, same epoch as the worked case, ON arm: the two `arc_resume` rows read
`shifted_fresh … slip=1` where the control reads `shifted_bridged … slip=0` — the instrument sees the
response, by construction (both sites synthesise the same key).

### 12.5 Verdict

**`network.cycle_slip.gap_reentry_relevel` = KEEP_OFF as a promotion on field lineages** (ledger
`docs/CONFIG_promotion_ledger.json`, regime unchanged: `on("network.phase_leveling") or
on("network.carry_invariant")`). It trades a **bounded, identified** poison population (88 bridged
arcs / 12 h) for an **unbounded** re-level tax, and the graded consumer says the tax is larger:
settled rms +56 %, concentrated in hours 3–9 (hourly ctl→ON: h3 4.33→5.49, h6 5.09→6.52,
h7 4.28→8.31, h9 0.79→2.42; h0–h2 and h11 flat-to-better), and one station newly damaged —
**WROC00POL 3.56 → 15.64 m** settled ZWD span — while BSHM 368.9 → 33.4, BOR1 9.03 → 0.46,
INEG 7.91 → 3.58, COCO 12.67 → 9.78. The fleet centre improves and the tail reshuffles.

This is `docs/SPEC_slip_relevel_walk.md`'s measured mechanism at a new edge: **a refloat re-levels
the arc from ONE epoch of code**, so a response that fires 129 times per 12 h buys metre-class
code-level re-seeds. The DEFECT is real and now instrumented; the bare refloat is not its answer.

**What is KEPT regardless of the verdict:** the instrument half — `gap_instrument`'s arc ledger now
armed by the response too, the `state_absent` / `no_carry` buckets (an inert wiring reads as inert),
the per-arc `reentry_relevel` event identity, the manifest keys, and the prires RNEG re-partition
those runs produced (`docs/SPEC_prior_residual_census.md` §17).

⚠ BSHM00ISR's residual 33.4 m settled span is NOT this card: that station is independently broken
from epoch 0 (`docs/SPEC_amb_iono_split_overcount.md` §6.16). The card removed its 2.6 km excursion,
not its baseline pathology.

### 12.6 The successor tried once — bridge v3 is FLAT on this posture

Pre-registered next step: a **level-PRESERVING** re-entry response (bridge the level across the gap
from the raw MW/GF step instead of dropping it). The mechanism already exists for DETECTOR slips —
`cycle_slip.relevel_delta_bridge` (bridge v2 pend-and-replace) + `relevel_bridge_gf` (v3's GF row) —
so it was graded on the same field posture first, before being extended to re-entries.

Arm `runs/fldf3_pl2_bridge` (`config/test_suite/_fldf3_pl2_bridge.json` = the same control + those
two knobs and the run paths; nothing else differs), same 12 h bed. Run-end
(`runs/fldf3_pl2_bridge/console.txt`, verbatim):

> Network (slip re-level response): 1486 detector-marked (station, amb) free-epoch latches, 1155
> inject-site skips actually taken … **Bridge v2 (pend-and-replace): 282 pend(s) created, 52 REPLACED
> (of which 2 paired same-epoch; max |ΔN| 52.3 cyc)** — those arcs recovered their pre-slip level at
> phase precision; 43 corroboration-rejected (kept the free re-level — the §8.1 wrong-bridge guard),
> 16 kernel-rejected, 109 expired unpaired, 60 voided by re-slip, 28 no-carry, 13 refsat-frame, 63
> MW-without-flag.
>
> Network (bridge v3): GF rows captured 103 (E1+E4 float-plane applies 49, wrong-WL plane rejects 0);
> EWL integers filled 0 same-epoch + 0 belated, band-3 replaces 0.

| channel (settled ≥ h6) | control `fldf3_pl2_ctl_new` | bridge `fldf3_pl2_bridge` |
|---|---|---|
| consumer 3D rms (m), n = 720 | 3.178 | **3.131** (−1.5 %) |
| consumer 3D p50 (m) | 2.418 | **2.823** (+16.7 %) |
| fleet ZWD span p50 (m) | 0.797 | **0.904** |
| stations > 2 m / > 10 m | 14 / 3 | **18 / 4** |
| worst-2 span stations (m) | BSHM 368.9, CUSV 12.8 | **INEG 327.1, BSHM 203.3** |
| T4a attribution-ARC flags | 2032 | **1801** |
| T3c RAIM consensus flags | 1105 | **896** |

(ZWD spans and consumer recomputed from each arm's own `logs/network_zwd.csv` /
`logs/closed_loop_trace.csv`; tier counts from `logs/network_flag_rates.csv`.)

**Reading: FLAT.** 52 of 282 pends replaced (18 %) and 49 GF float-plane applies over 12 h is a
small population; the consumer's rms moves −1.5 % and its p50 +17 %, and the ZWD sink gets slightly
worse (p50 +13 %, and the > 10 m tail gains a station). So the lane-preserving re-level **as built
for detector slips is not the successor either on this posture** — extending it to re-entries is not
justified by this arm, and any level-preserving re-entry response needs a design that is graded on
what it re-seeds, not only on what it stops erasing.

### 12.7 Open items this card leaves

1. **A level-preserving re-entry response is still unbuilt and now unsourced** — v2/v3 as built are
   flat here (§12.6). Design constraint from P4: whatever it does at the re-coupling edge must cost
   less than 129 code-level re-seeds / 12 h, i.e. it must carry the level across the break at phase
   precision or not act.
2. **The MW/LLI drain has the SAME live-state hole** (§11 "Still OWED" item 4,
   `src/core/slip/NetworkSlipQc.cpp:151-153`): any detector assertion raised on a not-yet-recreated
   state is silently inert there too. The fix shape is now proven; it was not applied there because
   that drain's population is dominated by measured arcs and the change would not be byte-identical.
3. **No sim reproduction — owed.** The simulator injects `cycle_slips.*` only
   (`src/sim/SimConfig.cpp:95-101`, `:526-568`); there is no tracking-gap / dropout / outage
   scenario anywhere in the tree, so this whole class is field-only until a `dropout` injector is
   added. Until then the response cannot be graded against truth, which is precisely why the P4
   consumer regression is the binding evidence.
4. **P2/P3's tails are unexplained by this card**: the response left three stations > 10 m and
   created one (WROC00POL). Whether that is the re-coupling tax alone or a second mechanism was not
   separated.

