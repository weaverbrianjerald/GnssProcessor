# Q3 G4 — G4's sim half executed — the anchor truth-graded on the 12 h quiet flagship, and the sim q-ladder

**STATUS**

- **id:** Q3 G4
- **topic:** clock
- **status:** closed
- **summary:** G4's sim half executed — the anchor truth-graded on the 12 h quiet flagship, and the sim q-ladder
- **dates in the text:** 2026-08-31 → 2026-09-01
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/camp60wu_quiet12h_r2q3`, `runs/camp60wu_quiet12h_r2`, `runs/camp60wu_quiet12h_r2q35`
- **source:** `docs/archive/SPEC_clock_temporal_anchor.md` — §9, §9.1

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_clock_temporal_anchor.md -->
## §9 (2026-08-31) G4's SIM HALF EXECUTED — the anchor truth-graded on the 12 h quiet flagship, and the displaced cb tension is REAL

**Bed:** `runs/camp60wu_quiet12h_r2q3` vs `runs/camp60wu_quiet12h_r2` — single
mechanism (the full Q3 arm at the CALIBRATED rung: q 3e-6, derived floors, proj
denominator, k 6, guard+resolve), over the R2 sticky-handover flagship on the
truth-graded synthetic network (SPEC_synthetic_minimum_network §17).  1440/1440.

**Mechanism read:** 71 983 anchored folds; **ZERO fault trips (0 nis + 0 abs)**
over 12 h — at q 3e-6 on a quiet field the guard never fires, which is §7's "at
the floor QUIET IS REAL" reading one decade above the floor.  214 decouples are
all MECHANICAL (201 datum + 13 gap), each wire-marked; 266 re-coupling edges.
Guard σ̂₀ 0.443 (conservative side, matching the ladder's descent), mean |ê|
3.6 mm against the 9.5 mm √(q·Δt) allowance.

**Truth A/B (settled half), the first ABSOLUTE grade any Q3 arm has had:**

| | R2 | R2+Q3 | Δ |
|---|---|---|---|
| consumer 3D rms (m) | 0.265 | **0.204** | **−23 %** |
| ZWD rms (m) / vf | 0.073 / 123 | **0.054 / 82** | −25 % / −33 % |
| clock contrast rms (m) | 1.940 | 1.922 | −1 % |
| clock residual rms (m) | 2.000 | 2.058 | +2.9 % (level) |
| clock vf | 33 358 | **216 567** | ×6.5 |
| **code bias rms / p50 (m)** | 0.538 / 0.0248 | **0.609 / 0.0277** | **+13 % / +11 %** |
| code bias vf | 4 841 | 6 460 | +34 % |

- **The §8.8 open question is ANSWERED: the tension the descent displaced into
  the code-bias carry is REAL ERROR, not a formal artifact** — cb truth rms
  +13 % under the anchor, the sim's absolute grade confirming what the field
  ladder could only see as vf.  The anchor buys wire/consumer quality partly by
  spending the cb channel — the LV-6 level-authority successor card inherits
  this bill.
- Consumer −23 % and ZWD −25 % are the anchor working as designed (temporal
  stiffness where the physics is quiet); per-sat clock contrast is flat (the
  truth error there is the level/exchange wander, not epoch noise — Q3 was
  never its cure).
- **Clock vf ×6.5**: the anchor tightens the published σ against a level error
  it cannot see — the carry-aware σ frontier, sharpened not created.  ⚠ Do not
  read Q3's clock vf as calibration on a truth-graded arm; the ladder's
  "vf = 0.99 at 3e-6" was the INTERNAL census against the carry, a different
  statistic.

**Telemetry fix (same session):** the run-end "N trips but ZERO in-epoch
re-solves — every trip shipped an anchor-warped epoch" warning counted the
COMPOSITE trip figure; gap/datum/ext decouples are not resolve candidates (the
prior is withdrawn BECAUSE the frame or visibility moved).  On this arm it
cried 214 warped epochs against a true fault-trip count of ZERO.  The warn now
fires on `nis + abs` only (`NetworkTeardown.cpp`) — the §8.11 ratio=inf lesson,
recurring in the code written after that lesson.

**Status:** G4's sim half is EXECUTED with the anchor NET-POSITIVE on the
product channels at the calibrated rung.  The FIELD half (the DOY-129 event
window — does the guard catch a real clock event without the withdraw path
regressing?) remains the arming blocker for field lineages; this arm never
exercised a fault trip, so the resolve path is still field-unvalidated.

### §9.1 (2026-09-01) THE SIM q-LADDER — 3e-6 confirmed a LOCAL OPTIMUM on truth, from both sides

Single-knob rungs around the calibrated point on the same bed
(`runs/camp60wu_quiet12h_r2q35` q 3e-5, `_r2q37` q 3e-7, vs the `_r2q3`
flagship at 3e-6 and `_r2` without the anchor; one build, fresh convergence
each, 1440/1440 all).  Settled-half truth grades:

| q [m²/s] | consumer 3D rms / p50 (m) | ZWD rms / p50 (m) | cb p50 (m) | clk vf | quiet trips |
|---|---|---|---|---|---|
| — (R2)   | 0.265 / 0.177 | 0.073 / 0.0160 | 0.0248 | 33 358 | — |
| 3e-5     | 0.209 / 0.143 | 0.062 / 0.0118 | 0.0275 | 112 736 | 0 nis |
| **3e-6** | **0.204 / 0.128** | **0.054 / 0.0116** | 0.0277 | 216 567 | 0 nis |
| 3e-7     | 0.240 / 0.171 | 0.101 / 0.0112 | 0.0272 | 406 038 | **5 nis** |

- **The §8.1-derived rung is the consumer optimum from BOTH sides**: loosening
  a decade gives back a third of the ZWD win and +12 % consumer p50; stiffening
  a decade costs +18 % consumer rms and DOUBLES ZWD rms on tails (p50 flat —
  the 5 quiet-bed nis trips' re-realization shocks, landing exactly at the
  §8.1 floor-crossover rung, are the tail source).  The sim independently
  confirms the field ladder's calibration with an absolute grade.
- **The cb bill is STIFFNESS-INDEPENDENT** across the two-decade window
  (p50 0.0272-0.0277 at every rung vs 0.0248 unanchored): the +11 % is the
  price of ARMING the anchor at all, not of its q — retuning q cannot buy it
  back; only the LV-6-class level authority addresses it.
- **Clock-σ overconfidence grows monotonically with stiffness** (vf 33 k →
  113 k → 217 k → 406 k): each decade of q tightens the published σ against
  the same level error it cannot see.  Consistent with §9's "sharpened, not
  created" reading; a stiffer-than-calibrated rung is strictly worse on
  honesty with no consumer payoff.
- Event census: identical mechanical decouples at 3e-5/3e-6 (214); 3e-7 adds
  its 5 nis trips (219 total).  Handovers, slips, refsat resets byte-flat
  across rungs.

**Status: the q choice is CLOSED on this bed** — 3e-6 stands in the flagship
and no rung retune is a live candidate.  The G4 FIELD half (guard catching a
real event) remains the field-lineage blocker, unchanged by this ladder.
