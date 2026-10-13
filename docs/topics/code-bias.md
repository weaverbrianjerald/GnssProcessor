# `code-bias` — Code bias — satellite OSB (W6), the receiver carry, admission

The satellite code-bias slice (the W6 OSB scope, per-signal), the receiver code-bias carry
and its S-consistent harvest, CB admission and the excursion re-init, plus the
constellation-specific bias work (BeiDou W6, GLONASS IFB).

**Conventions and invariants**

- The rx-CB carry's rolling common-mode memory is what TRACKS the ionosphere level; the
  ±6 TECU breathing is its price. It is an accidental crutch, not a designed authority.
- The rx-CB inject gate is FRAME-BLIND — it compares `|value_abs − x_lin|` against a
  frozen first-epoch seed, and 7 of 61 field stations were refused all day because of it.
  Fixing it is a solve-affecting card.
- Estimability traps are the norm here: a code-bias column can be registered and
  unobserved (dual-frequency under W6 leaves L5's columns registered but unobserved).
- `cb_excursion_reinit` is conditional-fingerprinted — seed and legs must ALL carry it;
  never flip it mid-chain.

**Current posture (2026-09-14)** — W6 OSB on the field golden, GPS dual-frequency under
the re-founding's scope cut. See [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).
From 2026-10-13 the goldens carry Galileo E1 / E5a in both code families with one Galileo clock datum across the
families (card [GAD](../refound/GAD_galileo_adoption_2026-10-13.md); cards GAL2 and GFC).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [S2H](../cards/code-bias/S2H-HARVEST.md) | rx-CB harvest de-mean: frame hygiene validated to 3 h, 12 h level bar MISSED, LV-6 next | closed | 2026-08-21 |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_beidou_w6_datum.md](../SPEC_beidou_w6_datum.md)
- [SPEC_cb_admission_card.md](../SPEC_cb_admission_card.md)
- [SPEC_cb_carry_physical_q.md](../SPEC_cb_carry_physical_q.md)
- [SPEC_rx_cb_carry.md](../SPEC_rx_cb_carry.md)
- [glonass_enablement_plan.md](../glonass_enablement_plan.md)

---

Back to [INDEX.md](../INDEX.md).
