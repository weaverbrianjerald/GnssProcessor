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
| [G-A](../cards/sim-bed/SIMNET-GA-NULL-TEST.md) | the NULL test — the first absolute grade, measured 2026-08-11 | closed | 2026-08-11 |
| [G-A 50-sta](../cards/sim-bed/SIMNET-50STA-QC-TIERS.md) | the 50-station global arm — the vf trend, the QC tiers, and the MW null space | closed | 2026-08-11 |
| [W6+SH arm](../cards/sim-bed/SIMNET-W6SH-ARM.md) | the W6+SH arm and its minimum-n ladder — the two-band wall is n-invariant, posture is t… | closed | 2026-08-12 |
| [W6+SH brief](../cards/sim-bed/SIMNET-W6SH-BRIEF.md) | the design brief for the W6+SH synthetic arm — superseded by the arm itself | superseded | 2026-08-12 |
| [G-B](../cards/sim-bed/SIMNET-GB-CLOCK-STEP.md) | event injection, satellite-clock step — first class only | closed | 2026-08-15 |
| [SIMNET](../cards/sim-bed/SIMNET-REF.md) | the truth-graded 3/5/50-station fixture — architecture, truth channels, the estimable-f… | reference | 2026-08-15 |
| [injection bench](../cards/sim-bed/SIM-INJECTION-BENCH.md) | the G-B event-injection bench — the ambiguity N-regime, the pre-flight, the arms, and t… | closed | 2026-08-15 |
| [60-sta campaign](../cards/sim-bed/SIMNET-60STA-CAMPAIGN.md) | the first long / multi-GNSS / realistic-slip grade — full record in the slip re-level w… | closed | 2026-08-20 |
| [dashboard](../cards/sim-bed/SIMNET-DASHBOARD.md) | one-command HTML report over a graded simnet run | closed | 2026-09-01 |
| [stability](../cards/sim-bed/STABILITY-CAMPAIGN.md) | the settled state must stop moving — targets 1–4, the eclipse-exclusion valve, sticky p… | closed | 2026-09-02 |
| [DD-REF](../cards/sim-bed/DD-TRACE-REF-SAT.md) | the post-eclipse network-wide DD episode was the INSTRUMENT'S reference satellite, and … | closed | 2026-09-06 |
| [slant + triple](../cards/sim-bed/SIMNET-SLANT-TRIPLE.md) | the third band and the slant-ionosphere posture — the ambiguity-integer grade, the sett… | closed | 2026-09-09 |
| [ZWD sink](../cards/sim-bed/ZWD-SINK-CENSUS.md) | the ZWD sink census, the slipped-bed golden FAIL, and where the integer information is | closed | 2026-09-10 |

---

Back to [INDEX.md](../INDEX.md).
