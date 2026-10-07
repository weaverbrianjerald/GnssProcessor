# datum wander: the common clock level follows the datum station

**STATUS**

- **id:** datum wander
- **topic:** network-vehicle
- **status:** closed
- **summary:** The fleet-common clock wander follows the datum station's geometry; `clock_datum_sum_to_zero` PASSES its first validation, and a 25° producer mask is REJECTED as the remedy.
- **dates in the text:** 2026-08-11
- **knobs / posture:** `network.clock_datum_station` (S01 polar vs S26 equatorial), `network.clock_datum_sum_to_zero`, `processing.elevation_mask_deg` (10 vs 25); arms `_simnet50_ga_cd{A,B,C,D}.json` and `cd{E,F}` on the quiet 50-station sim fixture
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md`, §9.8, 9.8.1

**Outcome.** On identical data, moving the datum station from polar to equatorial cuts the common wander 0.193 → 0.024 m and the datum-free contrast 2.2×; the consumer follows the contrast, not the wander, so pivot election is a product-quality decision. Sum-to-zero pins the wire mean exactly (wander 0.036 m at the polar pivot) but does not help the PPP consumer. A 25° mask restores the polar pivot's contrast and costs about 5 m of consumer position. The [ledger](../../CONFIG_promotion_ledger.md) keeps sum-to-zero off: it does not graduate on the field consumer. Owed elsewhere: a re-read of the §9.7.1 vf plateau against cdB-class arms.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §9.8 The DATUM-WANDER arm — the common level FOLLOWS THE DATUM STATION, and both remedies measured (2026-08-11)

The §9.7.1/§9.7.9 channel: a pivot-relative fleet-common C level that does not converge with
n and is slip-independent.  On the QUIET fixture `truth_C ≡ 0`, so the fleet-common of the
emitted C records IS the wander, exactly.  Tool: `scripts/simnet_common_mode.py`
(median-over-sats series; settled, demeaned).

**Design.**  Same quiet 50-sta data, same armed posture, ONE key varied:
`network.clock_datum_station` — which the setup makes `stations[0]`, so the clock datum AND
the phase pivot AND the sync reference move together (not separable in the legacy family).
S01 is near-polar (lat −68.6°, the §2.4 "sees everything at low elevation" class); S26 is
equatorial (+1.1°).  Then `network.clock_datum_sum_to_zero` — the CLAUDE.md amendment-5
"needs its first validation run" candidate — on each.  Pre-registered: P1 (wander owned by
the datum station) ⇒ series decorrelate under datum change; P2 (shared-state) ⇒ corr ≈ +1.
Arms `_simnet50_ga_cd{A,B,C,D}.json`.

| arm | pivot / clock datum | C resid rms | C contrast rms (datum-free) | common wander std | consumer 3D rms |
|---|---|---|---|---|---|
| cdA | S01 polar, pivot-sat emit | 1.102 m | 0.966 m | **0.193 m** | 1.417 m |
| cdB | S26 equatorial, pivot-sat emit | 0.384 m | **0.443 m** | 0.024 m | 0.315 m |
| cdC | S01 + sum-to-zero | 0.636 m | 0.991 m | 0.036 m | 1.589 m |
| cdD | **S26 + sum-to-zero** | **0.286 m** | **0.434 m** | **0.014 m** | **0.208 m** |

### Findings

| # | finding |
|---|---|
| 1 | **P1 in substance: the wander follows the datum station, and its amplitude is set by that station's geometry** — 0.193 m (polar) vs 0.024 m (equatorial), 8×, on identical data; first-difference corr(A,B) = +0.15 ≈ 0 (increments unrelated).  The level-corr (−0.885) is two smooth 1 h curves trending oppositely — per the §9.7.1 amendment, effective n ≈ few for slow series; do not read that shape |
| 2 | **The §9.7.9 "0.920 m common" figure decomposes as ~0.82 m CONSTANT lump** (d_IF/anchor-class offset, reproducible, not a stability problem) **⊕ ~0.19 m true WANDER**: √(0.824² + 0.193²) = 0.846 ≈ the 0.920 measured there |
| 3 | **The datum-free contrast ALSO moved (0.966 → 0.443, 2.2×)** — the pivot is not just a gauge: its phase pins gauge the PB chain through its own (noisy, low-elevation) data, its QC exposure differs, and the estimable per-sat content degrades with pivot quality.  **Pivot election is a PRODUCT-QUALITY decision, not a labeling decision** |
| 4 | **`clock_datum_sum_to_zero` first validation: PASS, doing exactly what it claims.**  Wire mean-over-sats ≡ 0.0000 exactly every epoch (constraint holds bit-for-bit); wander 0.193 → 0.036 at fixed (bad) pivot; availability unchanged (232/240 emitted, 8 A4 withholds, same as cdA); rank gate passes (`declared_dimension` 1 → 0, self-consistently — §5's caveat applies).  The loader AUTO-ARMS `clock_disc_on_set_change` (a clock-satellite set change re-references the Σ=0 gauge and marks the disc counters — invariant #7 already wired).  My pre-registered "< 0.02 m" missed: with Σ=0 enforced the MEDIAN wander (0.036) measures distribution asymmetry, not the level; the MEAN is pinned identically |
| 5 | **The consumer feels the PIVOT, not the wander**: consumer 3D rms ranks with the contrast column (0.21/0.32 m for good-pivot arms vs 1.42/1.59 m for polar-pivot arms — a 6.8× end-to-end swing from datum election alone), and sum-to-zero alone (cdC) does NOT help the consumer — a PPP consumer estimates its own clock, so the common level was never its problem.  The wander matters to ABSOLUTE-clock consumers (timing, inter-provider comparison) and to every internal vf/URA channel |
| 6 | **The §9.7.1 vf plateau needs an asterisk**: clock vf 7866 (cdA) vs 741 (cdB) — the σ-optimism largely followed the pivot artifact too.  The matched-trend arms (n = 3/5/50) all used the first-roster-station datum, so "vf ~455 plateau at every n" conflates estimator σ-optimism with the datum-election artifact.  Owed: a re-read of that trend against cdB-class arms before quoting the plateau as an estimator property |

### What this does and does not say about the FIELD wander

The FT63 lineage's `clock_datum_station` is MBAR00UGA (equatorial — the field already has
good datum geometry by luck or design).  What transfers is the MECHANISM: under the legacy
emit, the datum receiver's own clock-estimation error goes 1:1 onto the wire's common level.
On real data that error budget is real multipath + environment + hardware, not the sim's
clean noise — and the field lineage estimates CB jointly (W6 posture), so the CB-Q
"metres/pass in unpinned inter-channel dirs" is this channel COUPLED into the bias states,
not this channel alone.  The sim numbers bound the geometry term only.

**Remedy stack, validated here:** (a) elect a good-geometry pivot (helps EVERYTHING
including the consumer, 2.2× on per-sat content); (b) arm `clock_datum_sum_to_zero` for the
common level (5×, exact mean pin; costs a set-change re-reference class, auto-marked).  Both
are datum toggles — INSIDE the warm-resume fingerprint, fresh convergence only.  Caveats: one
fixture, 1 h settled window, GPS-only INT posture (no CB states), float consumer.

### 9.8.1 The 25° mask arms — the mechanism CONFIRMED, the remedy REJECTED

Two more arms (`_simnet50_ga_cd{E,F}.json`): the SAME two pivots with
`processing.elevation_mask_deg` 10 → 25 (producer-side mask on the same data).

| arm | pivot | mask | contrast rms | wander std | consumer 3D rms |
|---|---|---|---|---|---|
| cdA | polar | 10° | 0.966 m | 0.193 m | 1.417 m |
| **cdE** | polar | **25°** | **0.403 m** | 0.067 m | **4.839 m** |
| cdB | equatorial | 10° | 0.443 m | 0.024 m | 0.315 m |
| **cdF** | equatorial | **25°** | 0.447 m | 0.078 m | **4.968 m** |

| reading | evidence |
|---|---|
| **Mechanism confirmed** | masking the POLAR pivot's low-elevation data recovers the datum-free contrast to the equatorial level (0.966 → 0.403 ≈ 0.443), while the same mask on the equatorial pivot moves it not at all (0.443 → 0.447).  The penalty is specifically THE PIVOT'S low-elevation data gauging the network — not low-elevation data in general |
| **Remedy REJECTED** | the global mask costs a ~+4.8–5.0 m SYSTEMATIC consumer position error in BOTH arms (p50 ≈ 5.1 m — a bias, not noise: the tropo/height separation lives in the low-elevation geometry the mask removed), and the wander worsens on the good pivot (0.024 → 0.078, less averaging).  A 25° mask amputates the observability to hide the artifact |
| the surgical version — scope the GAUGE, not the data | elect a good pivot (validated, free), or a future pivot-PIN elevation floor (pin only the pivot's high-elevation ambiguities; every observation stays in the solve).  The sub-network/common-pivot intuition reduces to exactly this: the gauge should ride on high-quality observations; partitioning the SOLVE would add tie-seam datum churn (invariant #7's edge class) for no observability gain over per-satellite anchor election (`phase_datum_prioritized` / eq34), which already exists |
| pivot cycle slips are NOT a factor in any cd arm | the quiet fixture injects zero slips (`rate_base` 0, no scripted slips) — the 2.2×/8× effects are slip-free by construction.  (Field-side pivot-slip exposure is §9.7.10's separate, now-closable channel) |

