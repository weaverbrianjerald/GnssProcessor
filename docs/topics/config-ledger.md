# `config-ledger` — The config promotion ledger — every knob's verdict and regime

One row per default-OFF knob, in four verdict tiers (RECOMMEND_ON, RECOMMEND_SCOPED,
UNVALIDATED, KEEP_OFF), plus the cross-cutting notes and the A1–A5 session appendices.
This is the record the goldens are generated from.

**Conventions and invariants**

- **`docs/CONFIG_promotion_ledger.json` is the machine-read twin** and the authority for
  `scripts/make_golden_config.py --verify` and `scripts/config_posture_check.py`. The
  cards below are the prose half; the two are edited together.
- ⚠ The `scripts/_*_ledger_oneoff.py` family opens `docs/CONFIG_promotion_ledger.md` **by
  path**, which is now a routing stub. A future one-off must target the tier card, not the
  stub — this is the one follow-up the split created, and it is in
  [status/OPEN_ITEMS.md](../status/OPEN_ITEMS.md).
- There is no knob PROMOTION step separate from the ledger: a validated card that the
  goldens do not carry breaks `make_golden_config.py --verify`, and that is the promotion.
- A verdict is only as good as its regime. "Helps" without a stated scale, bed and
  lineage is not a verdict.

**Current posture (2026-09-14)** — the re-founding freezes an explicit posture rather than
inheriting the armed set; the ledger remains the reference for what each knob was measured
to do.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [A2](../cards/config-ledger/LEDGER-A2-SESSION-GATES.md) | the F-series session gates — S2H, the H-heartbeat rider, the internal 12 h stability ve… | closed | 2026-08-12 |
| [A1](../cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) | the load-time parallel_carry / parallel_reduce guard, and results R1–R6 | closed | 2026-08-15 |
| [LEDGER](../cards/config-ledger/LEDGER-00-HOW-TO-READ.md) | how to read the ledger — the verdict tiers, the evidence rules, and the relationship to… | reference | 2026-08-15 |
| [cross-cutting](../cards/config-ledger/LEDGER-CROSS-CUTTING-NOTES.md) | the audit's cross-cutting notes — the rules that govern more than one row | reference | 2026-08-15 |
| [Tier 3 (a)](../cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) | UNVALIDATED, the SH / carry / clock-datum / MDB rows | reference | 2026-09-01 |
| [A4](../cards/config-ledger/LEDGER-A4-GOLDEN-CONFIGS.md) | THE GOLDEN CONFIGS — the ledger made executable, and the smoke results | closed | 2026-09-05 |
| [A3](../cards/config-ledger/LEDGER-A3-WU93-NIGHT.md) | the wu93 night — the wind-up fix landed, the slip-response lane verdicts, and the golde… | closed | 2026-09-09 |
| [A5](../cards/config-ledger/LEDGER-A5-FIELD-SESSION.md) | the 2026-09-09 field session — the gap re-entry response, bridge v3, and the E1 probe | closed | 2026-09-10 |
| [Tier 2 (a)](../cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) | RECOMMEND_SCOPED, the iono / ZWD / carry-covariance rows | reference | 2026-09-10 |
| [Tier 3 (c)](../cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) | UNVALIDATED, the leveling-carry family and the in-block iono / rx-clock carries | reference | 2026-09-10 |
| [Tier 3 (f)](../cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) | UNVALIDATED, the oracle knobs, the producer-AR trace and the F3 station block | reference | 2026-09-10 |
| [Tier 4](../cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) | KEEP_OFF — diagnostic, falsified, or superseded | reference | 2026-09-10 |
| [Tier 4 (b)](../cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) | KEEP_OFF, continued — the PB-gauge level, GLONASS, lane fold, phase-datum and service-m… | reference | 2026-09-10 |
| [Tier 2 (c2)](../cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) | RECOMMEND_SCOPED, the cold anchor-fallback candidacy row and the snapshot capture / res… | reference | 2026-09-11 |
| [Tier 3 (b)](../cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) | UNVALIDATED, the RAIM riders and the promoted handover pair | reference | 2026-09-12 |
| [Tier 3 (d)](../cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md) | UNVALIDATED, the slip-response scope knobs | reference | 2026-09-12 |
| [Tier 1](../cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) | RECOMMEND_ON — measured benefit, no known counter-scope | reference | 2026-09-13 |
| [Tier 2 (c)](../cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) | RECOMMEND_SCOPED, the sticky pivot and the whole `resume_*` / re-admission family | reference | 2026-09-13 |
| [Tier 2 (e)](../cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) | RECOMMEND_SCOPED, the S-basis, snapshot-capture and trace rows | reference | 2026-09-13 |
| [Tier 3 (e)](../cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md) | UNVALIDATED, the common-band scope and the IF-TD pair / reference-arc rows | reference | 2026-09-13 |
| [Tier 2 (b)](../cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) | RECOMMEND_SCOPED, the snapshot-seam, clock-datum, code-datum and IF-TD reference rows | reference | 2026-09-14 |
| [Tier 2 (d)](../cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) | RECOMMEND_SCOPED, the emission, parallel, phase-datum and RAIM rows | reference | 2026-09-14 |

---

Back to [INDEX.md](../INDEX.md).
