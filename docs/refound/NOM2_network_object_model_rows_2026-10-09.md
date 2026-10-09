# NOM2: the network object model, continued (row kinds and onward)

**STATUS**

- **id:** NOM2
- **topic:** network-vehicle
- **status:** open
- **summary:** Card NOM's continuation (that file nears the size bar). Step N2 as card NOM section 17 pre-registered it: N2a declares a kind for every contribution to the normal equations and tags every row and fold where it is created; N2b moves the readers onto the tags. N2a LANDED, byte-identical (section 2); N2b next.
- **source:** [NOM](NOM_network_object_model_2026-10-08.md) sections 3, 4, 10, 13, 16 (the row inventory and the confirmed defects) and 17 (the N2 contract); [RPA](RPA_raim_pool_data_rows_2026-10-08.md) (the science card that follows N2b)
- **knobs / posture:** none; N2a byte-identical including traces, N2b with explained trace moves and an unchanged solve
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** main `f1ff6a8` (the pre-step tree); the pre-change reference build `gnss_processor_gal2.exe` (`df936f39`, clean `a4cda77`), byte-identical to main on every golden arm so far
- **gates (frozen at pre-registration):** card NOM section 17

**Outcome.** Question: can every contribution to the network's normal equations carry a declared kind a reader can follow? N2a: 57 kinds, a tag on every row and fold, and a "contributions as armed" table, byte-identical on six arms, the eq34 posture and 25 family smokes, with every integrity line at 0 (section 2). The readers move onto the tags in N2b.

## 1. The contract

Card NOM section 17 is the contract and is not repeated here. Two additions fixed before the build:

- The tags are plain data: a row tag is a `ContributionKind` with the row's satellite and signal (the N0a types), held in a vector aligned with the station block's rows; a shared fold carries its kind. No row or fold object owns a matrix.
- The user-side measurement header (`include/measurement/MeasurementModel.hpp`) stops including network headers: the absorbed-instance record of step N1b moves to its own small header.

<!-- Results are appended below this line under dated headings. -->

## 2. 2026-10-09: N2a landed (`cad880f`, `c0be47f`)

Built on branch `nom/n2a` from main `2bf6352`, reviewed (land after five fixes), fixed, and fast-forwarded onto main. Exe `gnss_processor_nom14.exe` (`82af0231`).

**What a reader gets.** `include/filter/NetworkContributionKind.hpp` (the only contribution header the estimator includes) declares `ContributionKind` (57 kinds, `Unset` the default of every tag) and the shared-fold log; `include/filter/NetworkContributions.hpp` holds one `ContributionSpec` per kind (name, channel, stage relative to the rank gate, subject, parameter kinds touched, weight source, builder, whether it is observation data, the unit it counts) in one table checked by `static_assert`. Every row of every station block carries a `RowTag` (kind, satellite, signal) aligned with `A_r` and `A_s`, through the sort and coalesce of the diagonal prior channels and the within-epoch replay's re-whiten (stored for the readers of N2b). Every shared fold names its kind. The "contributions as armed" table (after the first emitted epoch and at run end; a `contributions` record in the run manifest) lists every kind with its count, unit, builder and weight, zero-instance kinds included, and closes with five integrity lines. The absorbed-instance record of N1b moved to `include/measurement/AbsorbedSharedStates.hpp`, so the user-side measurement header includes no network header.

**Gates.** G0 IDENTICAL against `gnss_processor_gal2.exe` on the four arms and against `c7e0a9a`'s build on the clock-clone and border-clone arms; every CSV trace byte-identical to the previous build (only the timing columns move); `main_log` gains the table lines only. Integrity lines at 0 on every arm. Coverage beyond the goldens: the PRM harness eq34 posture on both beds (IDENTICAL; eq34 pins 720 and 482, rank-floor and time-link rows non-zero) and 25 smokes, one per family the goldens do not arm (GLONASS anchors, SH and GIM rows, SD-EWL, the phase-colour rows, joint coupling with landed replays, Kaula, phase hold, pin bridge, clock oscillator, cold clock anchor, ensemble gauges, DD blocks on the retired golden, the in-block and diagonal carries, the post-PD datum, the clock sum-to-zero, the gauge-free border carry, shared history, the second phase channel): each IDENTICAL to `c7e0a9a`'s build, each kind non-zero where armed, all integrity lines 0. Unit suite 2 935 run, 2 911 passed, 24 skipped, 0 failed (`NetworkContributionsTest` 18 cases; the coupling kept-row test).

**Deviations from the contract, recorded.** The attribution tag strings stay as they were (card NOM section 17 said the kinds replace them; N2b does it, with a kind-to-tag map). A subject `station_los` was added. The table describes the primary build of each epoch: the within-epoch replay's re-folds are counted on their own line, so after a landed replay the table is not the system that was solved. The GIM builder takes the tag vector (its signature changed). The deweighted rows (extra-band code, gross line of sight, unvetted phase, second code channel) appear in the weight texts, not as counts. Wall time (G3) is owed on an idle box.

**What the loader refuses on the golden posture** (found by the smokes): GLONASS with the satellite code biases; SH and GIM rows with the W6 constraint `none` and the broadcast group-delay datum; joint whitening with the float vehicle, the exact update or the in-block receiver code bias; the invariant DD carry with the float vehicle; the two ionosphere carries together.
