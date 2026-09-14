# RULE F2 — the LLI shadow — a lane that did not see the flagged epoch is not silent

**STATUS**

- **id:** RULE F2
- **topic:** slip
- **status:** closed
- **summary:** the LLI shadow — a lane that did not see the flagged epoch is not silent
- **dates in the text:** —
- **knobs / posture:** `network.cycle_slip.common_band_scope`
- **hashes named in the text** (commit / exe / byte-cert): `420634f`
- **run dirs named in the text:** `runs/evleg_m_s1_fix`, `runs/evleg_m_s1_`, `runs/evleg_r_s1_fix`
- **source:** `docs/archive/SPEC_event_harness.md` — 14.11, 14.11.1, 14.11.2, 14.11.3, 14.11.4

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 14.11 RULE F2 — the LLI shadow: a lane that did not see the flagged epoch is not silent

### 14.11.1 The mechanism, from the detector's own decision lines

`MwSlipDetector::detect`'s uncombined branch, on any epoch where a satellite
carries a receiver loss-of-lock bit, schedules the ambiguity resets and
`continue`s — **deliberately preserving the MW arc** (the MW combination is a
function of raw observations alone, so a short lock-loss does not invalidate the
EWMA).  So the arc count runs straight through the flagged epoch and the first
sample after it reads MATURE while the lane saw nothing at the epoch the receiver
declared broken — and a band-common slip does not move the wide lane in any case.

Storm-bed census: all five spurious `keep` verdicts in 2 994 sit exactly one epoch
after a receiver-flagged slip on the same arc, and in every one the exonerating
lane CONTAINED the flagged band (S37/G20 (L1C+L5Q) n 87 → 99 across the LLI, thr
9.0 WL; S25/E36 (L1C+L5Q) n 164 → 173).

Reproduced on the S1 leg, from the new `MWDEC` / `MWSKIP` lines
(`runs/evleg_m_s1_fix`, S21000SIM / G08):

```
  -1 L1C+L2W     dev  +0.5668 m (+0.658 WL)  thr 3.4777 (4.035 WL)  n= 9  flag=0  shadow=0
  +0 ALL PAIRS WITHHELD (LLI on L5Q)          <- no sample evaluated; n does NOT advance
  +1 L1C+L2W     dev -24.5936 m (-28.534 WL)  thr 3.5786 (4.152 WL)  n=10  flag=1  shadow=0
  +1 L1C+L5Q     dev  +1.6025 m ( +2.133 WL)  thr 3.1228 (4.156 WL)  n=10  flag=0  shadow=1
  +1 L2W+L5Q     dev +181.82 m (+31.023 WL)   thr 2.0700 (0.353 WL)  n=10  flag=1  shadow=1
  +2 L1C+L5Q     dev  +1.1801 m ( +1.571 WL)  thr 4.8701 (6.481 WL)  n=11  flag=0  shadow=0
```

The answer to "does the detector evaluate the LLI epoch's sample or withhold it":
**it withholds it, for the whole satellite, and keeps the arc** — so the next
epoch's sample is compared against the pre-LLI mean and reads the COMBINED jump.
Line 4 is the damage: +2.13 WL against a 4.16 WL gate, silent and "mature", and
it exonerated L1C and L5Q.  The S3 leg's +1 shows the other half — `L1C+L2W dev
+0.466 WL, shadow=0` next to `L1C+L5Q +31.133 WL flag=1` and `L2W+L5Q +31.023 WL
flag=1` — the lane carrying no flagged band, which is why the correct late
narrowing survives F2.

### 14.11.2 The rule

Gated by `network.cycle_slip.common_band_scope`; no new knob, OFF stays
byte-identical.  **A MW pair is NOT silent — it is refused exactly as an immature
pair is — at any epoch where one of its BANDS carried an LLI at the previous epoch
on that (station, satellite).**  Exported by the detector itself:
`EpochPairStatus::lli_shadow`, from a one-epoch per-satellite band memory
(`prev_lli_bands_`, rotated at the top of `detect`), so no run-context fallback is
needed and the (station) scope is the per-station detector instance.  Band-scoped
on purpose: a lane made only of unflagged bands is untouched by the receiver's
report and may still exonerate.  Basis token `mw(L1C+L5Q)lli_shadow(n=99)`,
counter `cb_lli_shadow`, run-end clause extended.

| leg | with F2 |
|---|---|
| **S1 ON** | +1 `all reason=no_exoneration; basis=mw(L1C+L5Q)lli_shadow(n=10);…` — **F2 alone closes the storm shape**; levels, ZWD, other arcs and T4 all return to the OFF baseline |
| **S1 FIX** | same verdict; the +0 IF-TD catches remain |
| **S2** | unchanged (no LLI on that leg) — the two mechanisms are complementary, each closing its own shape |
| **L2** | unchanged `keep implicated=L2W`, level residual 0.008–0.013 cyc |
| **S3** | unchanged `keep implicated=L5Q` at +1 — the 853-of-858 correct late narrowing is not costed |

### 14.11.3 Detector readout

Two DEBUG lines in `MwSlipDetector` (silent at the shipped level; an emission
cannot change a decision, and products are byte-identical):

- `MWDEC sta=… <sat>_<sig1>_<sig2> t=… dev=… thr=… n=… lam_wl=… flag=… lli=… lli_shadow=…`
  at the flag decision, for **every evaluated pair** — the per-sample record was
  previously readable only through a verdict, and a verdict exists only on epochs
  where some pair FIRED, so the silent lane either side of a slip was unreadable;
- `MWSKIP sta=… <sat> t=… lli_bands=[…] — EVERY pair's sample WITHHELD this epoch`
  in the LLI branch, so a missing MWDEC line reads as a withheld sample rather
  than as missing data.

`setStationLabel` because all 60 stations share the one `mw_slip_detector` logger
and its records are keyed only by `<sat>_<sig1>_<sig2>` — without the label the
log cannot be read per station at all.  Arm with
`logging.enable_mw_slip_detector: true` + `logging.levels.mw_slip_detector:
"debug"` (the §13.4 recipe) and read the rotated `mw_slip_detector.N.log` files
too.  ⚠ The sink is 4 × 5 MB and a 60-station leg writes ~15 MB per 25 epochs, so
a 70-epoch leg rotates its own event out: use a **25-epoch leg with `--at-ep 10`**
when the log is the instrument (`runs/evleg_m_s1_*`, `m_s3_*`).

### 14.11.4 Gates

| gate | result |
|---|---|
| **G1** both knobs at their defaults, vs main's own Release exe (`420634f`) | **PASS** on the L2 control leg and on the quiet 240-epoch null: products, solution, events, ZWD, DD-fractional, rx-states and the consumer trace all byte-identical |
| `all` vs `first`, quiet 70-epoch null | products / solution / ZWD / DD-fractional **byte-identical**; `network_events.csv` differs only by the new rows and `pair=` tokens |
| `all` vs `first`, L2 single-band control | same four artifacts **byte-identical** |
| `all` vs `first`, S1 / S3 | differ, as they must — the new pairs catch the L5 slip at +0 |
| unit suite | 1892 cases / 259 suites, 1875 passed, 17 fixture-gated skips, 0 failures (+22 over main's 1870) |

Run-end clause from a FIX leg (`runs/evleg_r_s1_fix`):

```
Network (common-band scope): verdicts 480, narrowed 3 (bands kept 6), all-band 477
  (no exoneration 471 / inconsistent 6 / anchor co-locate 0), IF-TD vetoes 0,
  immature pairs 720, LLI shadows 1, weak silences 3; split-referenced satellites
  mean 3.4 / max 5 of 52.3 anchored [knob on].
Network (IF-TD pairs [all]): 5 pair kind(s), eval 202014 / flag 3 / refloat 3 total —
  L1C_L2W eval 39813 / flag 1 / refloat 1, L1C_L5Q eval 67338 / flag 1 / refloat 1,
  L1C_L7Q eval 27525 / flag 0 / refloat 0, L2W_L5Q eval 39813 / flag 1 / refloat 1,
  L5Q_L7Q eval 27525 / flag 0 / refloat 0.
```

Both knobs stay default (`first` / OFF).  Owed before any promotion: the storm-bed
arm, and the §14.10.4 LLI-epoch verdict named above.

