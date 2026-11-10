# SSR-BIAS-COV: SSR bias coverage counted on presence, without a bias or an age

**STATUS**

- **id:** SSR-BIAS-COV
- **topic:** realtime
- **status:** open
- **summary:** `SsrCorrectionStore::coverageFor`, which feeds the engine's per-epoch coverage line, counted a satellite as code- or phase-bias covered whenever a bias record existed: a record with no bias in it, or one never refreshed, read as coverage. On 2026-11-03 the BKG and GFZ streams, whose phase-bias messages carry satellite blocks with no bias, read 18 of 18 and 16 of 18 satellites covered. FIXED: a bias record counts only when it holds at least one bias and is fresh within the age limit the orbit and clock counts use; unit-tested.
- **dates:** 2026-11-09 (finding from the 2026-11-03 coverage sessions, fix, unit tests)
- **knobs / posture:** none (a count fix, no toggle). The count is a log line of the live path (`logSsrCoverage`, called only when the real-time engine uses the live orbit and clock source); nothing in the estimation reads it, and the network products do not call it.
- **code:** `src/core/products/SsrCorrectionStore.cpp` (`coverageFor`, `holdsFreshBias`), `include/products/SsrCorrectionStore.hpp` (`Coverage`, `coverageFor` documentation), `tests/test_ssr_bias_coverage.cpp`
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_cov_2026-11-02\{smoke_bkg1,smoke_gfz1,chc1}` (the evidence, before the fix); the session after the fix in section 5
- **reference:** the register line "SSR coverage freshness" (`docs/status/OPEN_ITEMS_CONSUMER_REALTIME.md`) and card [SSR-PB-HDR](SSR-PB-HEADER.md) section 5, which asked for the age gate
- **source:** the stream coverage survey of 2026-11-03 (`D:\GnssProcessorSupport\presentation\agu26\coverage_note.md`, finding "Engine measurement")

**Outcome.** Does the engine's bias coverage count say which satellites have a bias to apply? It did not: any bias record counted, so two streams that send phase-bias messages with empty satellite blocks read as fully covered, and a record never refreshed would count forever (card SSR-PB-HDR saw a decode defect as a slow ramp instead of low coverage for this reason). FIXED: a code- or phase-bias record counts only when it holds a bias and is fresh within the limit of the orbit and clock counts. Unit-tested on hand-built records and on a decoded empty IGS-SSR phase-bias block; results in section 5.

> Results are appended TO THIS FILE, below the text.

---

## 1. Evidence

The 60 s smoke sessions of 2026-11-03 on BKG (SSRC00BKG1) and GFZ (SSRC00GFZ1), and the 30-minute CHC
session (SSRC00CHC1), with the engine of that night (stream content from raw captures decoded message by
message; the source note gives the method):

| session | coverage line at the last epoch | what the stream sent |
| --- | --- | --- |
| smoke_bkg1 | pbias 18/18 | phase-bias messages every 5 s whose satellite blocks carry no bias |
| smoke_gfz1 | pbias 16/18 | the same |
| chc1 | pbias 17/18 | Galileo phase biases for 24 satellites; E21 listed with no bias, and counted |

The decoder pushes one record per satellite block, so an empty block becomes a record with an empty
signal map. The count also had no age gate for the biases, while orbit and clock were gated on
freshness; card SSR-PB-HDR section 5 records that a gate would have exposed its decode defect.

## 2. Fix

`coverageFor` counts a code- or phase-bias record only when `holdsFreshBias` holds: the record's signal
map is not empty and its receipt stamp is within `max_age_s` of the epoch, the gate and the limit of the
orbit and clock counts (60 s, the default the engine's coverage line passes). The decoder is unchanged:
an empty block still replaces the satellite's previous record, which is the stream's statement that it
currently sends no bias for that satellite.

What the count does not mirror: the default apply path reads biases without an age (`getCodeBias` and
`getPhaseBias` without a time; the aged overloads are used only with
`processing.enable_ssr_bias_max_age` on, which the live configurations do not set). The coverage line now reports the biases that are fresh, which is the diagnostic it exists
for; a stale bias that the apply path would still use shows as missing coverage. `censusSummary`, the
start-up census of the whole store, still counts records by presence on purpose (it localises a problem
to decode or to application).

## 3. Unit tests

`tests/test_ssr_bias_coverage.cpp`, suite `SsrBiasCoverageTest`: a fresh record with a bias counts; a
record with no bias does not (code and phase); a record counts inside the age limit and not one second
after it; the counts are per satellite; a decoded IGS-SSR phase-bias message (4076, IM 26) whose
satellite block carries no bias reaches the store as an empty record and is not counted.
