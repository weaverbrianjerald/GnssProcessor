# `config-ledger` — The config ledger: one verdict per config key

**The ledger is [`CONFIG_promotion_ledger.json`](../CONFIG_promotion_ledger.json): one row,
one verdict, for every config key the network loader reads** (plus the few
other-section keys a network posture depends on).
[`CONFIG_promotion_ledger.md`](../CONFIG_promotion_ledger.md) is its reading view: a
hand-written head (procedure, regime vocabulary, bundles, history) and tables rendered by
`scripts/ledger_render.py`.

**Conventions and invariants**

- Verdicts: RECOMMEND_ON and RECOMMEND_SCOPED (the machine tiers: a regime predicate in
  the JSON and a row in the loader's posture table, `GnssConfigNetworkPosture.cpp`),
  ARMED (pinned in the goldens by a card decision), UNVALIDATED, KEEP_OFF, REMOVE (delete
  candidate), PARAM (a setting of a parent knob), TRACE (solve-inert output), CORE
  (structural input).
- A change is a JSON edit, `python scripts/ledger_render.py`, then
  `python scripts/make_golden_config.py --verify` and
  `python scripts/config_posture_check.py --verify-loader`. A new knob gets its row in
  the commit that adds it.
- There is no promotion step separate from the ledger: a RECOMMEND row in regime for a
  golden that the golden does not arm fails `make_golden_config.py --verify`.
- A verdict is only as good as its regime and its evidence: each row names the card that
  holds the measurement.

**Current posture (2026-10-13).** Rebuilt from the prose ledger (240 rows, 1.46 MB with its
JSON twin) into one row per key; stale pre-refound recommendations were demoted with their
posture rows. The goldens remain the posture
([GAD](../refound/GAD_galileo_adoption_2026-10-13.md)).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

_No split cards in this topic._

**Standing documents** (still at their original paths, indexed not moved):

- [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md)

---

Back to [INDEX.md](../INDEX.md).
