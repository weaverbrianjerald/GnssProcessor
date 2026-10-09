# NOM2: the network object model, continued (row kinds and onward)

**STATUS**

- **id:** NOM2
- **topic:** network-vehicle
- **status:** open
- **summary:** Card NOM's continuation (that file nears the size bar). Step N2 as card NOM section 17 pre-registered it: N2a declares a kind for every contribution to the normal equations and tags every row and fold where it is created; N2b moves the readers onto the tags. Results appended here.
- **source:** [NOM](NOM_network_object_model_2026-10-08.md) sections 3, 4, 10, 13, 16 (the row inventory and the confirmed defects) and 17 (the N2 contract); [RPA](RPA_raim_pool_data_rows_2026-10-08.md) (the science card that follows N2b)
- **knobs / posture:** none; N2a byte-identical including traces, N2b with explained trace moves and an unchanged solve
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** main `f1ff6a8` (the pre-step tree); the pre-change reference build `gnss_processor_gal2.exe` (`df936f39`, clean `a4cda77`), byte-identical to main on every golden arm so far
- **gates (frozen at pre-registration):** card NOM section 17

**Outcome.** Pre-registered; no result yet.

## 1. The contract

Card NOM section 17 is the contract and is not repeated here. Two additions fixed before the build:

- The tags are plain data: a row tag is a `ContributionKind` with the row's satellite and signal (the N0a types), held in a vector aligned with the station block's rows; a shared fold carries its kind. No row or fold object owns a matrix.
- The user-side measurement header (`include/measurement/MeasurementModel.hpp`) stops including network headers: the absorbed-instance record of step N1b moves to its own small header.

<!-- Results are appended below this line under dated headings. -->
