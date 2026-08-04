#!/usr/bin/env python3
"""Generate the RealTimeEngine seam TUs by byte-line surgery on the frozen source.

    python scripts/rt_seamcut.py            # write the .cpp files + the manifest
    python scripts/rt_seamcut.py --survey   # just print each block's bounds

Rebuild of the network decomposition's seamcut.py mechanic: extract -> dedent by
a measured depth -> assert min-indent -> prepend a mechanically-derived alias
preamble -> write, with ONE canonical seam table generating both the .cpp bodies
and the verifier manifest, so the two can never disagree about what was moved.

Line ranges are INCLUSIVE, 1-based, into runs/rtseam_goldens/pre/src/
RealTimeEngine.cpp.frozen -- captured before any edit and never regenerated.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FROZEN = ROOT / "runs" / "rtseam_goldens" / "pre" / "src" / "RealTimeEngine.cpp.frozen"
MANIFEST = ROOT / "docs" / "rt_seam_manifest.json"

# Alias preamble fragments, keyed by the driver-local name the body reads.
ALIAS = {
    "log":            'auto log = log_manager_.getLogger("rt_engine");',
    "epoch_prefix":   "const std::string& epoch_prefix = ctx.epoch_prefix;",
    "current_time":   "const GnssTime& current_time = ctx.current_time;",
    "epoch_obs":      "const std::vector<RinexObservation>& epoch_obs = ctx.epoch_obs;",
    "modeled_obs":    "std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;",
    "modeled_obs_c":  "const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;",
    "drop_reasons":   "const std::map<std::string, DropReason>& drop_reasons = ctx.drop_reasons;",
    "dt":             "const double dt = ctx.dt;",
    "dt_rw":          "double& dt = ctx.dt;",
    "gap_detected":   "const bool gap_detected = ctx.gap_detected;",
    "gap_detected_rw": "bool& gap_detected = ctx.gap_detected;",
    "meas_update":    "EpochMeasurement& meas_update = ctx.meas_update;",
    "update_applied": "bool& update_applied = ctx.update_applied;",
    # --- ingest seams: re-derive the singletons/accessors the driver held ---
    # Each is a getInstance() or a parser accessor, so re-deriving is identical
    # to threading the original reference through a parameter.
    "ssr_store":      "auto& ssr_store = SsrCorrectionStore::getInstance();",
    "hdr":            "const SsrHeader& hdr = parser.getLastSsrHeader();",
    "eph_store":      "auto& eph_store = LiveEphemerisStore::getInstance();",
}

# ---------------------------------------------------------------------------
# THE canonical seam table.
#   file    -- destination TU (basename under src/core/realtime/)
#   ret     -- "void" or "bool"
#   lines   -- [first, last] inclusive in the frozen source (the BODY only:
#              a bare `{ ... }` scope contributes its interior, never its braces)
#   aliases -- ALIAS keys, emitted in order as the preamble
#   subs    -- {frozen_text: relocated_text}; normally EMPTY.  A non-empty map is
#              a DECLARED non-verbatim edit and must be justified in `why`.
#   doc     -- Doxygen brief for the definition site
# ---------------------------------------------------------------------------
SEAMS = [
    # --- RealTimeEngineAdmit.cpp -------------------------------------------
    dict(file="RealTimeEngineAdmit.cpp", func="admitBacklogAndSync", ret="bool",
         lines=[835, 915], aliases=["log", "epoch_prefix", "current_time", "epoch_obs"],
         trailing_return=True,
         doc="Real-time obs/wall-clock lag: backlog warn, obs<->SSR hold, skip-to-latest."),

    dict(file="RealTimeEngineAdmit.cpp", func="gateMaxDuration", ret="bool",
         lines=[924, 935], aliases=["log", "epoch_prefix", "current_time"],
         trailing_return=True,
         doc="processing.max_duration_s data-time cutoff; latches max_duration_reached_."),

    dict(file="RealTimeEngineAdmit.cpp", func="stepTimeAndDetectGap", ret="bool",
         lines=[940, 978], aliases=["log", "epoch_prefix", "current_time",
                                    "dt_rw", "gap_detected_rw"],
         trailing_return=True,
         subs={"const GnssTime aiding_t_prev = prev_epoch_time_;":
               "ctx.aiding_t_prev = prev_epoch_time_;",
               "const bool     aiding_first  = first_epoch_;":
               "ctx.aiding_first  = first_epoch_;"},
         why="The two aiding captures were DECLARATIONS of driver locals; they "
             "become writes to the context. The `double dt;` / `bool gap_detected = "
             "false;` declarations are replaced by the rw aliases instead (they sit "
             "outside the extracted range).",
         doc="Time step / gap detection + the aiding-predict capture. False on a "
             "non-advancing TOW (duplicate epoch)."),

    dict(file="RealTimeEngineAdmit.cpp", func="gateCorrectionsReady", ret="bool",
         lines=[1019, 1120], aliases=["log", "epoch_prefix", "current_time", "modeled_obs_c"],
         trailing_return=True,
         doc="Corrections-ready startup gate: defer ALL estimation until every "
             "modeled above-mask satellite is broadcast+SSR."),

    # --- RealTimeEngineSsrDiag.cpp -----------------------------------------
    dict(file="RealTimeEngineSsrDiag.cpp", func="logSsrCoverage", ret="void",
         lines=[1130, 1144], aliases=["log", "epoch_prefix", "current_time", "modeled_obs_c"],
         doc="One-line per-epoch SSR coverage summary (are corrections flowing AND applying?)."),

    dict(file="RealTimeEngineSsrDiag.cpp", func="traceSsrConsumer", ret="void",
         lines=[1156, 1225], aliases=["current_time", "modeled_obs_c"],
         doc="L10 consumer-side SSR-application trace (network.trace_ssr_consumer)."),

    # --- RealTimeEnginePrepare.cpp -----------------------------------------
    dict(file="RealTimeEnginePrepare.cpp", func="dropBroadcastGrade", ret="void",
         lines=[1235, 1244], aliases=["log", "epoch_prefix", "modeled_obs"],
         doc="Drop broadcast-grade satellites from the precise SSR PPP-RTK solution."),

    dict(file="RealTimeEnginePrepare.cpp", func="seedClockFromWls", ret="void",
         lines=[1249, 1271], aliases=["log", "epoch_prefix", "current_time",
                                      "modeled_obs_c", "gap_detected"],
         doc="WLS receiver-clock seed. Self-gated on hasClock() -- mutually "
             "exclusive with regaugeBssd()."),

    dict(file="RealTimeEnginePrepare.cpp", func="regaugeBssd", ret="void",
         lines=[1300, 1333], aliases=["log", "epoch_prefix", "modeled_obs"],
         doc="BSSD code/phase re-gauge: remove the bulk receiver clock. Self-gated "
             "on !hasClock() -- mutually exclusive with seedClockFromWls()."),

    # --- RealTimeEngineSlipQc.cpp ------------------------------------------
    dict(file="RealTimeEngineSlipQc.cpp", func="detectAndRepairSlips", ret="void",
         lines=[1371, 1631], aliases=["log", "epoch_prefix", "current_time",
                                      "modeled_obs_c", "dt"],
         doc="Cycle-slip detection / repair (MW + TurboEdit + soft-LLI bridge), "
             "run pre-estimation."),

    # --- RealTimeEngineSsrBreaks.cpp ---------------------------------------
    dict(file="RealTimeEngineSsrBreaks.cpp", func="reactToGenerationChange", ret="void",
         lines=[1648, 1670], aliases=["log", "epoch_prefix"],
         doc="RTA-3: solution-generation id change -> GLOBAL datum break."),

    dict(file="RealTimeEngineSsrBreaks.cpp", func="logProducerStatus", ret="void",
         lines=[1684, 1699], aliases=["log", "epoch_prefix"],
         doc="RTA-10: producer epoch-status heartbeat -> one-shot transition log."),

    dict(file="RealTimeEngineSsrBreaks.cpp", func="resetOnPhaseBiasDisc", ret="void",
         lines=[1708, 1751], aliases=["log", "epoch_prefix", "modeled_obs_c"],
         doc="SSR phase-bias discontinuity / freshness flip -> ambiguity reset."),

    dict(file="RealTimeEngineSsrBreaks.cpp", func="reinflateOnCodeBiasDisc", ret="void",
         lines=[1766, 1868], aliases=["log", "epoch_prefix", "modeled_obs_c"],
         doc="SSR code-bias datum re-reference -> receiver code-bias reinflation."),

    dict(file="RealTimeEngineSsrBreaks.cpp", func="reinflateOnClockDisc", ret="void",
         lines=[1889, 1943], aliases=["log", "epoch_prefix", "modeled_obs_c"],
         doc="SSR clock datum re-reference -> receiver-clock note + scoped ISB "
             "re-inflation (A2)."),

    # --- RealTimeEngineEstimate.cpp ----------------------------------------
    dict(file="RealTimeEngineEstimate.cpp", func="runMeasurementUpdate", ret="void",
         lines=[1961, 2045], aliases=["epoch_prefix", "modeled_obs_c",
                                      "meas_update", "update_applied"],
         doc="KF measurement update with the outlier / pivot-handover rebuild loop."),

    dict(file="RealTimeEngineEstimate.cpp", func="applyZwdConstraint", ret="void",
         lines=[2063, 2076], aliases=[],
         doc="Soft a-priori ZWD constraint (one pseudo-observation row)."),

    dict(file="RealTimeEngineEstimate.cpp", func="applyNetworkIonoPrior", ret="void",
         lines=[2100, 2163], aliases=["current_time", "modeled_obs_c", "dt"],
         doc="Network slant-ionosphere prior (ionosphere-weighted / BLUP)."),

    dict(file="RealTimeEngineEstimate.cpp", func="resolveAmbiguities", ret="void",
         lines=[2169, 2193], aliases=["log", "epoch_prefix", "modeled_obs_c",
                                      "update_applied"],
         doc="Integer ambiguity resolution (MLAMBDA)."),

    dict(file="RealTimeEngineEstimate.cpp", func="writeEpochTraces", ret="void",
         lines=[2196, 2243], aliases=["log", "epoch_prefix", "current_time",
                                      "epoch_obs", "modeled_obs_c", "drop_reasons",
                                      "meas_update", "update_applied"],
         doc="Posterior state extract, per-epoch summary line and the trace-logger sweep."),

    # =====================================================================
    # Secondary cuts: initialize() and feedRtcmFrame().
    # Same mechanic, different origin function -- hence the explicit `origin`
    # and `params` (these seams have no per-epoch context to thread).
    # =====================================================================

    # --- RealTimeEngineInit.cpp (from initialize(), frozen 94-378) ---------
    dict(file="RealTimeEngineInit.cpp", func="armSsrStoreOptions", ret="void",
         params="", origin="RealTimeEngine::initialize", lines=[98, 130],
         aliases=["log"],
         doc="RTB-1(c) leap-second override + the two opt-in process-wide SSR "
             "store policies (RTA-13 code-bias band fallback, Card 7 orbit/clock "
             "skew budget).  Must run before any stream thread decodes a frame."),

    dict(file="RealTimeEngineInit.cpp", func="initProducts", ret="bool",
         params="", origin="RealTimeEngine::initialize", lines=[132, 181],
         aliases=["log"], tail="return true;",
         doc="ProductParser / RinexParser construction, receiver-antenna seeding "
             "and the product load + cache.  False if the cache load fails."),

    dict(file="RealTimeEngineInit.cpp", func="initStateManagerAndApriori", ret="void",
         params="", origin="RealTimeEngine::initialize", lines=[183, 211],
         aliases=["log"],
         doc="StateManager construction and the coarse a-priori position seed."),

    dict(file="RealTimeEngineInit.cpp", func="initProcessingComponents", ret="void",
         params="", origin="RealTimeEngine::initialize", lines=[213, 305],
         aliases=["log"],
         doc="PppModel / MeasurementModel / KalmanFilter / aiding / WLS / slip "
             "detectors, plus the three 'enabled but not wired into RT' warnings."),

    dict(file="RealTimeEngineInit.cpp", func="initAmbiguityResolver", ret="void",
         params="", origin="RealTimeEngine::initialize", lines=[307, 331],
         aliases=["log"],
         doc="Integer ambiguity resolution (MLAMBDA) setup; same configuration "
             "surface as the post-processing path."),

    dict(file="RealTimeEngineInit.cpp", func="seedInitialState", ret="void",
         params="", origin="RealTimeEngine::initialize", lines=[333, 347],
         aliases=["log"],
         doc="PppModel::initializeState plus the Earth-centred a-priori fallback "
             "a live stream needs (no RINEX APPROX POSITION)."),

    dict(file="RealTimeEngineInit.cpp", func="initTraceLoggers", ret="void",
         params="", origin="RealTimeEngine::initialize", lines=[349, 373],
         aliases=["log"],
         doc="TraceLogger, the L10 SSR-consumer trace and the slip-detector traces."),

    # --- RealTimeEngineIngest.cpp (from feedRtcmFrame(), frozen 437-687) ---
    # The observation-buffering loop (456-501) and the ephemeris push (544-548)
    # deliberately STAY in the driver TU: both call anonymous-namespace helpers
    # (isSystemActive, refreshGlonassSlots) that are file-local to
    # RealTimeEngine.cpp by design.  Moving them would mean hoisting those into
    # a new internal header for 51 lines of gain -- not worth the coupling.
    dict(file="RealTimeEngineIngest.cpp", func="ingestSsrRecords", ret="std::size_t",
         params="RtcmParser& parser, SsrAcceptPolicy accept",
         origin="RealTimeEngine::feedRtcmFrame", lines=[518, 542],
         aliases=["ssr_store", "hdr"], tail="return n_ssr_dropped;",
         doc="Push decoded SSR records into the shared store under the per-stream "
             "RTA-8(b) accept policy.  Returns the count dropped by that policy."),

    dict(file="RealTimeEngineIngest.cpp", func="logRtcmIngestCounters", ret="void",
         params="RtcmParser& parser, std::size_t n_ssr_dropped",
         origin="RealTimeEngine::feedRtcmFrame", lines=[550, 582],
         aliases=["ssr_store", "hdr", "eph_store"],
         doc="Per-frame SSR / ephemeris ingest counters, and the obs<->SSR "
             "drain trigger that fires when the SSR horizon advances."),

    dict(file="RealTimeEngineIngest.cpp", func="seedAprioriFromStationCoords", ret="void",
         params="RtcmParser& parser",
         origin="RealTimeEngine::feedRtcmFrame", lines=[583, 686],
         aliases=[],
         doc="RTCM 1005/1006 reference-station ARP -> filter a-priori seed, with "
             "the RTA-12(3) blunder gate."),
]

HEADERS = {
    "RealTimeEngineAdmit.cpp": (
        "Admission gates for RealTimeEngine::processEpoch.",
        "The only seams that can end an epoch early.  Each returns false to mean\n"
        " * \"processEpoch returns false\".  Every early exit in these bodies was\n"
        " * ALREADY a `return false` in the pre-cut driver, so the relocation is\n"
        " * verbatim and the whole control-flow transformation is the appended\n"
        " * `return true` — see scripts/rt_seam_verify.py, which proves that\n"
        " * mechanically against the frozen pre-cut source.",
        ['#include "math/TimeUtils.hpp"', '#include "common/PppUtils.hpp"',
         '#include "products/SsrCorrectionStore.hpp"', "#include <algorithm>", "#include <chrono>"]),
    "RealTimeEngineSsrDiag.cpp": (
        "Consumer-side SSR observability seams for RealTimeEngine::processEpoch.",
        "Pure diagnostics: neither seam mutates estimator state.  Both run BEFORE\n"
        " * the broadcast-grade drop so satellites the precise solution discards\n"
        " * still appear in the census and the trace.",
        ['#include "math/TimeUtils.hpp"', '#include "products/SsrCorrectionStore.hpp"',
         "#include <limits>", "#include <fmt/format.h>"]),
    "RealTimeEnginePrepare.cpp": (
        "Measurement-set conditioning seams for RealTimeEngine::processEpoch.",
        "Everything that reshapes the epoch's observation set or the filter's\n"
        " * starting point between computeModels and the KF predict.\n"
        " *\n"
        " * seedClockFromWls and regaugeBssd are MUTUALLY EXCLUSIVE by construction\n"
        " * (hasClock() vs !hasClock()): under BSSD the filter carries no receiver\n"
        " * clock state to seed, which is exactly why the re-gauge exists.  Both\n"
        " * self-gate, so the driver calls both unconditionally.",
        ['#include "math/TimeUtils.hpp"', '#include "math/WlsEstimator.hpp"',
         "#include <algorithm>", "#include <cmath>"]),
    "RealTimeEngineSlipQc.cpp": (
        "Cycle-slip detection and repair for RealTimeEngine::processEpoch.",
        "The largest single seam.  Roughly two thirds of it is reachable only with\n"
        " * estimation.cycle_slip.turbo_edit armed (the TurboEdit joint MW+GF repair\n"
        " * and the soft-LLI gap bridge); the remainder is the MW mandate path that\n"
        " * every real-time run takes.",
        ['#include "math/TimeUtils.hpp"', '#include "common/PppUtils.hpp"',
         "#include <map>", "#include <set>"]),
    "RealTimeEngineSsrBreaks.cpp": (
        "SSR datum-break reactions for RealTimeEngine::processEpoch.",
        "Five handlers for \"the network producer moved its datum under us\".\n"
        " *\n"
        " * ORDER IS LOAD-BEARING: reactToGenerationChange applies the UNION of the\n"
        " * finer-grained reactions globally (a producer restart re-realizes every\n"
        " * bias and level at once, with disc counters re-seeded to 0), so it must\n"
        " * run before the per-signal handlers that would otherwise see 0->0 = \"no\n"
        " * change\".  All five are no-ops without SSR content, which is what keeps\n"
        " * live RTCM streams byte-identical.",
        ['#include "math/TimeUtils.hpp"', '#include "common/PppUtils.hpp"',
         '#include "measurement/StateInitializer.hpp"',
         '#include "products/SsrCorrectionStore.hpp"', "#include <set>", "#include <vector>"]),
    "RealTimeEngineEstimate.cpp": (
        "Estimation tail for RealTimeEngine::processEpoch.",
        "Measurement update, the two pseudo-observation constraints, ambiguity\n"
        " * resolution and the trace sweep.\n"
        " *\n"
        " * The constraints sit AFTER the update and BEFORE AR on purpose: both\n"
        " * shrink the covariance the resolver reads (applyPseudoObservation updates\n"
        " * state AND covariance, which is what lets height recover — a value-only\n"
        " * clamp would not).",
        ['#include "app/EpochUpdateSupport.hpp"', '#include "math/TimeUtils.hpp"',
         '#include "products/SsrCorrectionStore.hpp"', "#include <cmath>", "#include <set>"]),
    "RealTimeEngineInit.cpp": (
        "Construction-phase seams for RealTimeEngine::initialize().",
        "Runs exactly once, before any frame is decoded.  Ordering is a genuine\n"
        " * dependency chain, not a stylistic sequence: the product parsers feed the\n"
        " * StateManager's a-priori, which the processing components bind to, which\n"
        " * the resolver and the state seed then read.  seedInitialState in\n"
        " * particular MUST follow initProcessingComponents — PppModel::initializeState\n"
        " * would otherwise clobber the a-priori with ppp_state_'s zero position.",
        ['#include "aiding/ImuAidingSource.hpp"', '#include "math/TimeUtils.hpp"',
         '#include "products/ProductCache.hpp"',
         '#include "products/SsrCorrectionStore.hpp"',
         '#include "common/Constants.hpp"', "#include <filesystem>"]),
    "RealTimeEngineIngest.cpp": (
        "RTCM ingest seams for RealTimeEngine::feedRtcmFrame().",
        "The SSR push, its counters and the reference-station a-priori seed.\n"
        " *\n"
        " * The observation-buffering loop and the broadcast-ephemeris push stay in\n"
        " * the driver TU on purpose: both call anonymous-namespace helpers\n"
        " * (isSystemActive, refreshGlonassSlots) that are deliberately file-local\n"
        " * to RealTimeEngine.cpp.",
        ['#include "math/TimeUtils.hpp"', '#include "math/Coordinates.hpp"',
         '#include "products/SsrCorrectionStore.hpp"',
         '#include "products/LiveEphemerisStore.hpp"', "#include <cmath>"]),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--survey", action="store_true")
    args = ap.parse_args()

    src = FROZEN.read_text(errors="replace").splitlines()
    by_file, manifest = {}, []

    for s in SEAMS:
        a, b = s["lines"]
        block = src[a - 1:b]

        # Assert the measured dedent: every non-blank line must carry it, or the
        # range boundary is wrong (the trap that silently eats a closing brace).
        indents = [len(l) - len(l.lstrip()) for l in block if l.strip()]
        dedent = min(indents) if indents else 4
        if dedent < 4:
            sys.exit(f"{s['func']}: min indent {dedent} < 4 at frozen {a}-{b} "
                     f"-- range crosses a scope boundary")

        if args.survey:
            print(f"{s['func']:26s} {a:5d}-{b:<5d} dedent={dedent} "
                  f"lines={len(block)}")
            print(f"    first: {block[0].strip()[:88]}")
            print(f"    last : {block[-1].strip()[:88]}")
            continue

        body = [l[dedent:] if l.strip() else "" for l in block]
        for frm, to in s.get("subs", {}).items():
            body = [l.replace(frm, to) for l in body]

        pre = [ALIAS[k] for k in s["aliases"]]
        params = s.get("params", "RtEpochContext& ctx")
        origin = s.get("origin", "RealTimeEngine::processEpoch")
        doc = ["/**", f" * @brief {s['doc']}", " *",
               f" * Relocated verbatim from the pre-cut {origin}",
               f" * (frozen lines {a}-{b}); see scripts/rt_seam_verify.py.", " */"]
        lines = doc + [f"{s['ret']} RealTimeEngine::{s['func']}({params}) {{"]
        if pre:
            lines += ["    " + p for p in pre] + [""]
        lines += ["    " + l if l else "" for l in body]
        # `tail` is the declared trailing statement -- `return true` for a bool
        # gate whose body only ever returns false, or a value return for a seam
        # that hands a count back to the driver.
        tail = s.get("tail") or ("return true;" if s.get("trailing_return") else None)
        if tail:
            lines += ["", "    " + tail]
        lines += ["}", ""]

        by_file.setdefault(s["file"], []).append("\n".join(lines))
        manifest.append(dict(file=f"src/core/realtime/{s['file']}",
                             func=f"RealTimeEngine::{s['func']}",
                             head=[a, b], dedent=dedent,
                             skip=len(pre), subs=s.get("subs", {}),
                             tail=tail))

    if args.survey:
        return

    for fname, bodies in by_file.items():
        brief, detail, incs = HEADERS[fname]
        out = ["/**", f" * @file {fname}", f" * @brief {brief}", " *",
               " * " + detail, " */", "",
               '#include "realtime/RealTimeEngine.hpp"'] + incs + [
               "", "namespace GnssProcessor {", ""] + bodies + [
               "} // namespace GnssProcessor", ""]
        (ROOT / "src" / "core" / "realtime" / fname).write_text("\n".join(out))
        print(f"wrote {fname}  ({len(bodies)} seams)")

    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(
        {"frozen": "runs/rtseam_goldens/pre/src/RealTimeEngine.cpp.frozen",
         "seams": manifest}, indent=2))
    print(f"wrote {MANIFEST.relative_to(ROOT)}  ({len(manifest)} seams)")


if __name__ == "__main__":
    main()
