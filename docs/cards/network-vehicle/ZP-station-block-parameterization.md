# ZP — the station block's parameterization at ignition — pre-registered

**STATUS**

- **id:** ZP
- **topic:** network-vehicle
- **status:** open
- **summary:** the station block's parameterization at ignition — pre-registered
- **dates in the text:** —
- **knobs / posture:** `network.trace_station_block`, `network.zwd_prior_sigma_m`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_event_harness.md` — 24.10

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.10 Card ZP — the station block's parameterization at ignition (pre-registered; the phase-deweight arm above is its first result)

**Instrument (build, trace-only): `network.trace_station_block`** — per station per epoch: the local block's rank and condition number, its
smallest eigenpairs with the local states that span them (which of clock / ZWD / DCB / iono / ambiguity columns), the ZWD column's effective
information Σ w·h² in the reduced system against its raw Σ w·m_wet², the reconstructed local correction's norm, and the count of zero-redundancy
rows by kind — written for the first 20 epochs and every 60th thereafter.  Bars: none; it is the eyes of the card.

**Mechanism (build, default OFF, byte-identical, never fingerprinted): `network.zwd_prior_sigma_m`** — one pseudo-observation per station per
epoch on the tropo column toward the a-priori ZWD (the SH-tie row's shape).  Arms (cold 2 h from the golden, the traces on): σ 0.02 m (held),
σ 0.2 m (physical), control.  Reads: §24.8's (i)–(v), the clock LEVEL vs CODE, the cold consumer's 3D at 1 h / 2 h, and the station-block
trace.  Frozen reading: with the ZWD held, the code residual rms and the wire-vs-CODE within-arc rms FALL ⇒ the sink was a symptom and the
prior is the fix (a posture change; the level instruments re-graded); they RISE ⇒ the sink was protecting the wire and the inconsistency is
then measured directly as the change in the clock LEVEL vs CODE between the arms.

**The design question this leaves for the author** (not a worker's call): a station block whose code rows must resolve clock, ZWD, a γ-scaled
receiver bias and N tied ionospheres every epoch, with the phase contributing nothing absolute, is under-determined by construction; the
candidates are (a) a per-epoch ZWD prior (above), (b) an ionosphere-free code combination for the station states (the standalone PPP's
posture, which converges), (c) a receiver DCB datum per station instead of a free γ-scaled column, (d) an absolute phase time-link alongside
the DD carry (the leveling inject that `carry_invariant` retired).  Each is a posture change of the field vehicle; the goldens, the harness
seeds and every level-channel number change with it.


