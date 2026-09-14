# QPR blocked — blocked on root cause, leave-alone (falsified or decided), and the queue's maintenance rule

**STATUS**

- **id:** QPR blocked
- **topic:** queues
- **status:** open
- **summary:** blocked on root cause, leave-alone (falsified or decided), and the queue's maintenance rule
- **dates in the text:** 2026-08-14
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/QUEUE_principled_replacements.md` — (see body)

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: QUEUE_principled_replacements.md -->
## Blocked on root cause — do not build the "correct" fix yet

- ~~**⚠ NEW 2026-08-14 — the clock re-realization churn itself (item 3).**~~ **✅ RESOLVED same day —
  root NAMED and A/B-confirmed. Moved to item 1(d).** The four nulls and three falsifications below
  are all *correct as measured* and all *on the wrong side of the edge*: every one probes the
  design-matrix side of the gauge, while the defect is on the CARRY side. The live lead recorded
  below — "threshold crossings of a slowly-evolving quantity … shifted by exactly one epoch" — was
  the right instinct and resolved structurally, not statistically: `NetworkPhaseDatum.cpp:339` sets
  `target = 0.0` and overrides it only `if (is_handover && handover)`, so at T+1 the target reverts
  and the continuous pin is a **one-epoch deferral by construction**. The decisive test deleted the
  entire within-epoch QC tier and the re-realization SURVIVED (terminal `clock_std_m` 15.28 → 14.63,
  against 2.15 on the pdp-OFF twin) — so the tier is the *impulse channel*, not the source. Full
  evidence: `SPEC_pivot_visibility_gauge_defect.md` §5.07.

- **⚠ ORIGINAL (superseded) — the clock re-realization churn itself (item 3).** FOUR independent
  interventions on the phase-bias gauge left it untouched on the 24-sta arm: datum election on the
  prioritized path, `phase_datum_handover_continuous`, the pin bridge, and the
  `phase_datum_min_stations` coverage gate (that last byte-identical at 1/2/3 — the gate never binds
  at this density). Every arm holds at **7–8 episodes and Σ|dev| ≈ 160 m**. Three candidate roots
  tested and falsified: anchor handovers (compensated, no effect), column-set changes (0/15
  correlate), and the CLOCK datum station's own visibility (1/6 — the mechanism that DOES explain the
  legacy PB defect). Four nulls plus three falsifications is strong evidence this is **not a gauge
  problem at all**. Live lead: the episodes behave like threshold crossings of a slowly-evolving
  quantity — arming the handover knob shifted every one of the seven by exactly one epoch.

- **Trio-runaway (settled triple-freq divergence):** the INC-1 FREEZE is a bandaid that measured as
  ~35 % contributor, NOT root (runaway survives 1720 → 1132 cyc). Mechanism card first (suspects: tree
  re-hosts refolding invariant carry; chord-carry × slant-iono feedback). FLOAT stays the track.
- ✅ **D5 born-chord gating — HOLD SUBSTANTIALLY LIFTED 2026-08-14.** The hold below was "replacing D5
  means re-deciding the representation — a science program, not a patch swap", and the reason it
  stayed a science program is that the representation error had never been quantified. It now is:
  datum-free per-LOS interaction **rms 0.764 m**, truth-graded, with a measured consumer chain into
  both T3 and the DD integers (`SPEC_t3_null_derivation.md` §7.8–§7.9). See item 8. ⚠ Still hold D5
  itself until 8(a) says whether the error survives the directive posture.
- **D5 born-chord gating (original):** the lane spectrometer verdict is that chord churn IS the iono
  representation. Replacing D5 means re-deciding the representation (slant vs SH) — a science
  program, not a patch swap. Keep D5 until then.

## Leave alone — falsified or decided; do not re-litigate

- ZOH clock dynamics (C1: the rate state measured net-harmful; ZOH is MMSE here).
- MW slip thresholds empirical (standing user decision).
- Lane layer OFF (measured at both 63 and 305 stations, settled windows).
- Era-2 adaptive iono q (falsified).
- Datum-choice-at-emit as a consumer fix — ensemble PB, clock s2z, broadcast-anchored (falsified;
  see item 3).
- Reset-over-repair and withhold-as-backstop doctrines — principled; the gap is item 7's release
  policy, not the doctrines.

## Maintenance

Strike items as their cards land; this doc indexes, the SPECs carry the detail. When item 1a/1c land,
update `docs/SPEC_pivot_visibility_gauge_defect.md` §5.1 and the memory card
`project_pivot_visibility_gauge_defect` in the same commit. Item numbers are STABLE — express a
re-ranking in the BUILD ORDER block at the top rather than renumbering, so cross-references from
other docs keep resolving.
