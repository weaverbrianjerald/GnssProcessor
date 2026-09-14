# LV-3 — the level authority, made explicit — mostly documentation, one optional knob

**STATUS**

- **id:** LV-3
- **topic:** ionosphere
- **status:** closed
- **summary:** the level authority, made explicit — mostly documentation, one optional knob
- **dates in the text:** —
- **knobs / posture:** `network.a00_prior_from_brdc`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 5

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

