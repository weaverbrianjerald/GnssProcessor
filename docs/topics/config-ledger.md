# `config-ledger` — The config promotion ledger — every knob's verdict and regime

**The ledger is [`CONFIG_promotion_ledger.md`](../CONFIG_promotion_ledger.md), and it is a
LIVING REGISTER kept whole at that path.** One row per default-OFF knob in four verdict
tiers (RECOMMEND_ON, RECOMMEND_SCOPED, UNVALIDATED, KEEP_OFF), plus the cross-cutting notes
and the A1–A11 session appendices. This is the record the goldens are generated from.

**Conventions and invariants**

- **`docs/CONFIG_promotion_ledger.json` is the machine-read twin** and the authority for
  `scripts/make_golden_config.py --verify` and `scripts/config_posture_check.py`. The
  `.md` is the prose half; the two are edited together, in that order.
- **It is never split.** The `scripts/_*_ledger_oneoff.py` family (`_coldfb_`,
  `_fieldharness_`, `_gapreentry_float_`, `_grs_csl_cpa_`, `_rs2_refarc_`, `_scopedpromo_`,
  `_stspromo_`, `_study7_`) and every future promotion APPEND a row at that exact path, in
  step with the `.json`. The 2026-09-14
  documentation reorganisation left it whole for that reason, and
  `scripts/docs_index_check.py` carries a named size exemption for it (`SIZE_EXEMPT`) —
  a living register grows one row at a time and the 60 KB bar does not apply.
- There is no knob PROMOTION step separate from the ledger: a validated card that the
  goldens do not carry breaks `make_golden_config.py --verify`, and that is the promotion.
- A verdict is only as good as its regime. "Helps" without a stated scale, bed and
  lineage is not a verdict.

**Current posture (2026-09-14)** — [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md) freezes an explicit posture
rather than inheriting the armed set; the ledger remains the reference for what each knob
was measured to do, and RF-0's config is the new baseline a promotion is graded against.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

_No split cards in this topic._

**Standing documents** (still at their original paths, indexed not moved):

- [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md)

---

Back to [INDEX.md](../INDEX.md).
