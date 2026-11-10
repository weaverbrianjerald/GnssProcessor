# LV-3: the level authority, made explicit

**STATUS**

- **id:** LV-3
- **topic:** ionosphere
- **status:** closed
- **summary:** A documentation card: the frames stay as built (EXT-CB keeps the a00 GIM anchor, the self-contained posture the W6 sum-to-zero); the optional broadcast-Klobuchar a00 prior is deliberately NOT built.
- **dates in the text:** none
- **knobs / posture:** `network.a00_prior_from_brdc` (optional, NOT built; would feed the `mean_vtec_prior_*` channel at a loose 10 TECU sigma)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_level_information_budget.md` §5; the frame contract it points to is §30 ([LV-3 contract](../wire-contract/LEVEL-CONTRACT.md))

**Outcome.** The card makes the owner of the absolute ionosphere level explicit rather than accidental. The frames stay as built; the normative contract is §30 ([LV-3 contract](../wire-contract/LEVEL-CONTRACT.md)) and its product declaration is `SPEC_product_cbpin_contract.md` §6.6. The broadcast a00 prior was ranked LAST, only for postures without the GIM anchor: its prerequisite (retained Klobuchar coefficients) is VERIFIED, and the remaining blocker is a runtime override of `mean_vtec_prior_tecu`, a design decision of its own (§30.6). The re-founded goldens run free slant iono with no SH field and no GIM ([RF-3-prep](../../refound/RF-3-prep_golden_rebase_2026-09-15.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 5. Card LV-3 — the level authority, explicit  (mostly documentation; one OPTIONAL knob)

1. **No code change for the frames themselves**: EXT-CB keeps the a00 GIM anchor; self-contained keeps the W6
   satellite sum-to-zero. **The frame contract is §30** (absorbed from `CONTRACT_level_authority.md`); the
   consumer-facing product declaration it asks for is `docs/SPEC_product_cbpin_contract.md` §6.6.
2. **OPTIONAL — `network.a00_prior_from_brdc`**: compute the a00 target from the broadcast Klobuchar
   coefficients already in the ingested navigation data (area-weighted global mean of the Klobuchar vertical
   delay, converted to TECU at L1 — deterministic, no download), applied through the existing
   `mean_vtec_prior_*` channel at LOOSE sigma (10 TECU). **Rank LAST**: only worth doing if ignition speed
   still matters after LV-1/LV-2, and only for postures without the GIM anchor. Prerequisite VERIFIED; the
   remaining blocker is a runtime-override design, not parsing — §30.6.

