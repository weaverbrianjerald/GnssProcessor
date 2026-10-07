# `harness` — The EVENT HARNESS — producer-side fault injection on a warm resume

The harness injects ONE scripted fault into a converged network producer, runs 40–60
epochs, and grades event-minus-null. It exists so a mechanism can be tested against a
known cause in minutes instead of against a day-long run's ambient weather.

Six events are injectable (slip, arc erase, anchor handover, outage, satellite clock
step, receiver clock step); the injection acts at exactly one site, and a never-fired
event is a HARD error.

**Conventions and invariants**

- ONE injected event + 40–60 epochs per leg; the storm arm (18 min) is the final
  validation, not the development loop. At most three legs concurrently.
- Grade **event minus null**, on datum-invariant quantities (standing invariant #5).
  A leg's own seam transient is not a result.
- Seeds are FINGERPRINTED: a seed and its legs must sit on the same side of every
  fingerprinted knob and of the wind-up sign fix. The seed re-write history is in the
  `harness seeds` card — `_v2` snapshots are unusable, `_v3` and later are the live ones.
- Every leg is Release-only (standing invariant #2).

**Current posture (2026-09-14)** — the harness is the development loop; the bars it
feeds are now the two named by [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md). Sim truth bed first, then field.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Cards A+B](../cards/harness/CARD-AB-require-and-rx-clock-step.md) | require hard-fail fixed (error line, exit 1); rx_clock_step built; rx-clock carry KEEP_OFF | closed | 2026-09-10 |
| [HARNESS](../cards/harness/HARNESS-REF.md) | event harness reference: injectable events, leg recipe, event-minus-null grader, gaps | reference | 2026-09-10 |
| [Study 1](../cards/harness/STUDY-1-erase-vs-refloat.md) | erase-path premise FALSIFIED: band scope buys the 0.45-cycle floor (0.445 to 0.050 scoped) | closed | 2026-09-10 |
| [Study 2](../cards/harness/STUDY-2-rx-clock-carry.md) | rx-clock carry INERT on slip, erase and handover (at the null floor); KEEP_OFF (Study 2b) | closed | 2026-09-10 |
| [ORACLE PIN (stub)](../cards/harness/ORACLE-PIN-stub.md) | oracle pin stub, superseded by SPEC_oracle_pin: a complete pin cuts DD-invariant iono 10x | superseded | 2026-09-11 |
| [harness seeds](../cards/harness/HARNESS-SEEDS.md) | seed hygiene: legs arm zwd_seam_reseed; fingerprinted promotions force _v2, then _v3 seeds | reference | 2026-09-12 |
| [FIELD harness](../cards/harness/FIELD-HARNESS.md) | field harness: evidence refloat FIELD PIN; scope + pairs all stay exceptions, twin FAILS | closed | 2026-09-13 |
| [DEC-field-vehicle](../decisions/DEC-field-vehicle-posture.md) | field posture c8b: 6 h twin DRIFTS, G4 INCONCLUSIVE; superseded by the float vehicle | superseded | 2026-09-14 |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_oracle_pin.md](../SPEC_oracle_pin.md)

---

Back to [INDEX.md](../INDEX.md).
