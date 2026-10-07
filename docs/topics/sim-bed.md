# `sim-bed` — The simulated truth bed — the only absolute grade there is

The synthetic minimum network (3 / 5 / 50 / 60 stations, N processes one per station) is
the first absolute grade the network producer has ever had, plus the event-injection
bench, the dashboard, the stability campaign and the datum-wander arm.

**Conventions and invariants**

- Grade **into the S-basis**: n = 3 is estimable but not determinable, and a rank
  assertion on the full system is worthless.
- The fixture's ambiguity N-regime is a fixture property that changes what an experiment
  can conclude — check it before injecting.
- A sim fixture and a producer must sit on the **same side** of the wind-up sign fix;
  the mixed pairing is invisible to σ̂₀ at short horizons. Enforce by provenance.
- Every injection arm needs its mandatory negative control; the NULL arm is not
  automatically quiet.
- What the fixture CANNOT certify is written down — read that section before quoting it.

**Current posture (2026-09-14)** — the sim golden restricted to GPS dual is one of the
two beds of [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md); sim first, then field, for every card.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [G-A](../cards/sim-bed/SIMNET-GA-NULL-TEST.md) | first absolute grade: n = 3 not determinable (ZWD +4.31 m), all internal instruments clean | closed | 2026-08-11 |
| [G-A 50-sta](../cards/sim-bed/SIMNET-50STA-QC-TIERS.md) | 50-sta QC tiers: 4 of 7 live, datum-station slip blind spot fixed, MW mid-arc silence open | closed | 2026-08-11 |
| [W6+SH arm](../cards/sim-bed/SIMNET-W6SH-ARM.md) | the two-band wall is one n-invariant L2W phase-bias near-null; code-bias vf ~4.9e+05 | closed | 2026-08-12 |
| [W6+SH brief](../cards/sim-bed/SIMNET-W6SH-BRIEF.md) | design brief for the W6+SH sim arm: item 1 FALSIFIED, item 3 MOOT, the rest implemented | superseded | 2026-08-12 |
| [G-B](../cards/sim-bed/SIMNET-GB-CLOCK-STEP.md) | clock-step floor RETRACTED: 92.4 to 92.8 % recovered against the null arm, no floor | closed | 2026-08-15 |
| [SIMNET](../cards/sim-bed/SIMNET-REF.md) | sim truth-bed design: per-station processes, truth in the producer basis, trap register | reference | 2026-08-15 |
| [injection bench](../cards/sim-bed/SIM-INJECTION-BENCH.md) | slip-injection bench: 22/24 attributed, MDB 2 cycles; null arm found the gauge defect | closed | 2026-08-15 |
| [60-sta campaign](../cards/sim-bed/SIMNET-60STA-CAMPAIGN.md) | 60-sta slipped grade: detection 94.1 %, but refloat re-levels walk the levels to metres | closed | 2026-08-20 |
| [dashboard](../cards/sim-bed/SIMNET-DASHBOARD.md) | simnet dashboard built; 12 h quiet arms: S0 / BCA not promoted, carry_zwd KEEP_OFF | closed | 2026-09-01 |
| [stability](../cards/sim-bed/STABILITY-CAMPAIGN.md) | late drift is eclipse-valve edge response, not aging; sticky_pivot cuts sim consumer 74 % | closed | 2026-09-02 |
| [DD-REF](../cards/sim-bed/DD-TRACE-REF-SAT.md) | post-eclipse DD episode was the trace's reference satellite; the field histogram is not | closed | 2026-09-06 |
| [slant + triple](../cards/sim-bed/SIMNET-SLANT-TRIPLE.md) | third band closes the wall; slant cuts cb vf ~16× uncalibrated; settled EWL trio DIVERGES | closed | 2026-09-09 |
| [ZWD sink](../cards/sim-bed/ZWD-SINK-CENSUS.md) | slipped-bed golden FAIL x3; carry_zwd_in_block RECOMMEND_SCOPED; consumer AR 0/2879 fixes | closed | 2026-09-10 |

---

Back to [INDEX.md](../INDEX.md).
