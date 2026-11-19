#pragma once
/**
 * @file SsrStoreFixture.hpp
 * @brief Named states of the SSR correction store for user-engine tests, and the steps that move
 *        one quantity of a state at a time.
 *
 * A reaction test of the real-time engine or a test of a fixing function needs the correction
 * store in a known state (satellites with orbit and clock corrections, per-signal code and phase
 * biases with their integer flags, the discontinuity counters, the generation identifier, the
 * hold generation, the record ages) and then needs exactly one of those quantities to change.
 * Writing those pushes by hand in every test hides which quantity a case moves.
 *
 * The fixture is a plain value, @ref gnss_test::SsrStoreState, that describes the whole store
 * content, plus free functions:
 *   - builders that return a named state (@ref emptySsrState, @ref networkSsrState,
 *     @ref floatSsrState, @ref heldSsrState, @ref decodedSsrState);
 *   - steps that change one quantity of a state (a discontinuity counter, a flag, the generation,
 *     the hold generation, a bias value, a satellite's record age, a satellite's presence, the
 *     epoch);
 *   - writers that put a state into a store through its public push interface
 *     (@ref writeSsrState, @ref writeSsrSatellite).
 *
 * The store has no per-satellite removal, so @ref writeSsrState clears the store and pushes the
 * whole state again.  @c SsrCorrectionStore::clear also resets the store's diagnostics (the
 * attribute-mismatch and orbit-echo counters, the producer status, the declared level steps);
 * its configuration (band fallback, skew budget, age coefficient, declared roster) survives.  A
 * test that must keep the diagnostics across a step uses @ref writeSsrSatellite, which overwrites
 * one satellite's records without clearing.
 *
 * Every value is deterministic: the bias and clock values follow from the satellite's PRN and the
 * signal's position in its list, and every time stamp is derived from the state's epoch.
 */

#include "common/GnssIdentity.hpp"
#include "common/GnssTypes.hpp"
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rtcm/RtcmTypes.hpp"

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnss_test {

// ---------------------------------------------------------------------------
// The state description
// ---------------------------------------------------------------------------

/// One code signal of a satellite: the bias and its network discontinuity counter.
struct SsrCodeSignalSpec {
    double             bias_m{0.0};      ///< Code bias (m) as the store returns it.
    /// Network code-bias discontinuity counter; std::nullopt = untracked, as on every decoded
    /// RTCM or IGS-SSR stream (the wire has no such field).
    std::optional<int> discontinuity{0};
};

/// One phase signal of a satellite: the bias, its integer flags and its counters.
struct SsrPhaseSignalSpec {
    double        bias_m{0.0};             ///< Phase bias (m) as the store returns it.
    bool          signal_integer{true};    ///< The signal integer flag.
    int           wide_lane_integer{1};    ///< The wide-lane integer flag (2 bits on the wire).
    int           discontinuity{0};        ///< Phase-bias discontinuity counter.
    std::uint32_t hold_generation{0};      ///< Producer hold generation; 0 = not held.
};

/// Which records of a satellite a step addresses.
enum class SsrRecordKind { OrbitClock, CodeBias, PhaseBias, All };

/// Everything the store holds for one satellite.
struct SsrSatSpec {
    std::string     sat;                                       ///< Satellite id, e.g. "G05".
    bool            has_orbit{true};                           ///< Push an orbit record.
    bool            has_clock{true};                           ///< Push a clock record.
    Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};        ///< Orbit RAC delta (m).
    double          c0_m{0.0};                                 ///< Clock C0 (m).
    int             iode{-1};                                  ///< Broadcast IODE of the orbit; -1 = unknown.
    /// True for a decoded clock that applies only with an orbit of the same IOD-SSR; false for a
    /// network clock, which applies on its own.
    bool            clock_needs_orbit{false};
    int             clock_discontinuity{0};                    ///< Network clock discontinuity counter.
    std::map<std::string, SsrCodeSignalSpec>  code;            ///< Code signal -> bias; empty = no code-bias record.
    std::map<std::string, SsrPhaseSignalSpec> phase;           ///< Phase signal -> entry; empty = no phase-bias record.
    double          orbit_clock_age_s{0.0};                    ///< Age of the orbit and clock records at the state's epoch (s).
    double          code_age_s{0.0};                           ///< Age of the code-bias record (s).
    double          phase_age_s{0.0};                          ///< Age of the phase-bias record (s).
};

/// The whole content of a correction store at one epoch.
struct SsrStoreState {
    GnssProcessor::GnssTime           epoch{2300, 259200.0};   ///< Epoch every record age is counted from.
    int                               iod_ssr{1};              ///< IOD-SSR of every record and header.
    std::uint32_t                     generation_id{0};        ///< Producer generation id; 0 = none stamped.
    std::map<std::string, SsrSatSpec> sats;                    ///< Satellites in the store.
    std::map<std::string, SsrSatSpec> withdrawn;               ///< Satellites taken out, kept for their return.
};

// ---------------------------------------------------------------------------
// Time and deterministic values
// ---------------------------------------------------------------------------

/// @brief @p t moved by @p dt_s seconds, across week boundaries in either direction.
inline GnssProcessor::GnssTime ssrShift(GnssProcessor::GnssTime t, double dt_s) {
    constexpr double kWeek = static_cast<double>(GnssProcessor::TimeUtils::SECONDS_IN_WEEK);
    t.tow += dt_s;
    while (t.tow >= kWeek) { t.tow -= kWeek; ++t.week; }
    while (t.tow < 0.0)    { t.tow += kWeek; --t.week; }
    return t;
}

/// @brief The PRN of a satellite id, 0 when the id does not parse.
inline int ssrPrn(const std::string& sat) {
    const auto id = GnssProcessor::SatId::parse(sat);
    return id ? id->prn : 0;
}

/// @brief The default code and phase signals of a satellite's constellation: GPS C1C/C2W and
///        L1C/L2W, Galileo C1C/C5Q and L1C/L5Q, BeiDou C2I/C6I and L2I/L6I; any other
///        constellation takes the GPS pair.
inline std::pair<std::vector<std::string>, std::vector<std::string>>
ssrDefaultSignals(const std::string& sat) {
    using Names = std::vector<std::string>;
    const auto id = GnssProcessor::SatId::parse(sat);
    const auto sys = id ? id->sys : GnssProcessor::System::Unknown;
    if (sys == GnssProcessor::System::Galileo) return {Names{"C1C", "C5Q"}, Names{"L1C", "L5Q"}};
    if (sys == GnssProcessor::System::BeiDou)  return {Names{"C2I", "C6I"}, Names{"L2I", "L6I"}};
    return {Names{"C1C", "C2W"}, Names{"L1C", "L2W"}};
}

/// @brief Synthetic code bias of a satellite's @p index-th code signal (m).
inline double ssrSyntheticCodeBias(int prn, int index) { return 0.25 + 0.01 * prn + 0.5 * index; }
/// @brief Synthetic phase bias of a satellite's @p index-th phase signal (m).
inline double ssrSyntheticPhaseBias(int prn, int index) { return 0.03 + 0.001 * prn + 0.02 * index; }
/// @brief Synthetic clock C0 of a satellite (m).
inline double ssrSyntheticClock(int prn) { return 1.0 + 0.1 * prn; }
/// @brief Synthetic orbit RAC delta of a satellite (m).
inline Eigen::Vector3d ssrSyntheticOrbit(int prn) {
    return Eigen::Vector3d(0.01 * prn, 0.02 * prn, -0.005 * prn);
}

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

/// @brief One satellite as the network producer emits it: orbit and network clock, the default
///        code signals with tracked counters at 0, the default phase signals with both integer
///        flags set and every counter at 0, all records fresh.
inline SsrSatSpec networkSsrSatellite(const std::string& sat) {
    SsrSatSpec s;
    s.sat       = sat;
    const int prn = ssrPrn(sat);
    s.delta_rac = ssrSyntheticOrbit(prn);
    s.c0_m      = ssrSyntheticClock(prn);
    const auto signals = ssrDefaultSignals(sat);
    for (std::size_t i = 0; i < signals.first.size(); ++i)
        s.code[signals.first[i]] = SsrCodeSignalSpec{ssrSyntheticCodeBias(prn, static_cast<int>(i)), 0};
    for (std::size_t i = 0; i < signals.second.size(); ++i) {
        SsrPhaseSignalSpec p;
        p.bias_m = ssrSyntheticPhaseBias(prn, static_cast<int>(i));
        s.phase[signals.second[i]] = p;
    }
    return s;
}

/// @brief A state with no satellite and no generation id.
inline SsrStoreState emptySsrState(const GnssProcessor::GnssTime& epoch = GnssProcessor::GnssTime{2300, 259200.0}) {
    SsrStoreState st;
    st.epoch = epoch;
    return st;
}

/// @brief The network producer's content: every satellite of @p sats as @ref networkSsrSatellite
///        builds it, with the given generation id (0 = none stamped).
inline SsrStoreState networkSsrState(const std::vector<std::string>& sats,
                                     std::uint32_t generation_id = 0,
                                     const GnssProcessor::GnssTime& epoch = GnssProcessor::GnssTime{2300, 259200.0}) {
    SsrStoreState st = emptySsrState(epoch);
    st.generation_id = generation_id;
    for (const auto& sat : sats) st.sats[sat] = networkSsrSatellite(sat);
    return st;
}

/// @brief The network content with both integer flags cleared on every phase signal (a float
///        product: biases present, no integer property declared).
inline SsrStoreState floatSsrState(const std::vector<std::string>& sats,
                                   std::uint32_t generation_id = 0,
                                   const GnssProcessor::GnssTime& epoch = GnssProcessor::GnssTime{2300, 259200.0}) {
    SsrStoreState st = networkSsrState(sats, generation_id, epoch);
    for (auto& [sat, s] : st.sats)
        for (auto& [sig, p] : s.phase) { p.signal_integer = false; p.wide_lane_integer = 0; }
    return st;
}

/// @brief The network content under a phase-bias hold: every phase signal carries the hold
///        generation @p hold_generation (nonzero = held), and the store the generation id.
inline SsrStoreState heldSsrState(const std::vector<std::string>& sats,
                                  std::uint32_t generation_id, std::uint32_t hold_generation,
                                  const GnssProcessor::GnssTime& epoch = GnssProcessor::GnssTime{2300, 259200.0}) {
    SsrStoreState st = networkSsrState(sats, generation_id, epoch);
    for (auto& [sat, s] : st.sats)
        for (auto& [sig, p] : s.phase) p.hold_generation = hold_generation;
    return st;
}

/// @brief A decoded RTCM or IGS-SSR stream: clocks that apply only with their orbit, orbits on
///        broadcast IODE @p iode, code biases without discontinuity counters, phase biases with
///        the signal integer flag and no wide-lane flag, no generation id and no hold.
inline SsrStoreState decodedSsrState(const std::vector<std::string>& sats, int iode = 50,
                                     const GnssProcessor::GnssTime& epoch = GnssProcessor::GnssTime{2300, 259200.0}) {
    SsrStoreState st = networkSsrState(sats, 0, epoch);
    for (auto& [sat, s] : st.sats) {
        s.iode              = iode;
        s.clock_needs_orbit = true;
        for (auto& [sig, c] : s.code) c.discontinuity = std::nullopt;
        for (auto& [sig, p] : s.phase) p.wide_lane_integer = 0;
    }
    return st;
}

// ---------------------------------------------------------------------------
// Lookups used by the steps.  An unknown name is a mistake in the test, reported as a
// non-fatal failure; the step then does nothing.
// ---------------------------------------------------------------------------

/// @brief The satellite's spec in @p st, or nullptr (with a reported failure) when it is not in the
///        state.  A withdrawn satellite is not found.
inline SsrSatSpec* ssrFindSat(SsrStoreState& st, const std::string& sat) {
    const auto it = st.sats.find(sat);
    if (it == st.sats.end()) {
        ADD_FAILURE() << "SsrStoreFixture: satellite " << sat << " is not in the state";
        return nullptr;
    }
    return &it->second;
}

/// @brief The phase signal of a satellite, or nullptr (with a reported failure).
inline SsrPhaseSignalSpec* ssrFindPhase(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrSatSpec* s = ssrFindSat(st, sat);
    if (!s) return nullptr;
    const auto it = s->phase.find(sig);
    if (it == s->phase.end()) {
        ADD_FAILURE() << "SsrStoreFixture: " << sat << " has no phase signal " << sig;
        return nullptr;
    }
    return &it->second;
}

/// @brief The code signal of a satellite, or nullptr (with a reported failure).
inline SsrCodeSignalSpec* ssrFindCode(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrSatSpec* s = ssrFindSat(st, sat);
    if (!s) return nullptr;
    const auto it = s->code.find(sig);
    if (it == s->code.end()) {
        ADD_FAILURE() << "SsrStoreFixture: " << sat << " has no code signal " << sig;
        return nullptr;
    }
    return &it->second;
}

// ---------------------------------------------------------------------------
// Steps: each changes one quantity of the state and returns the new value (or -1 / false when the
// name was unknown).  The store changes only when the state is written again.
// ---------------------------------------------------------------------------

/// @brief Increment one phase signal's discontinuity counter.
inline int bumpPhaseDiscontinuity(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    return p ? ++p->discontinuity : -1;
}

/// @brief Increment one code signal's discontinuity counter; an untracked counter starts at 0, so
///        its first bump reads 1.
inline int bumpCodeDiscontinuity(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrCodeSignalSpec* c = ssrFindCode(st, sat, sig);
    if (!c) return -1;
    c->discontinuity = c->discontinuity.value_or(0) + 1;
    return *c->discontinuity;
}

/// @brief Increment a satellite's clock discontinuity counter.
inline int bumpClockDiscontinuity(SsrStoreState& st, const std::string& sat) {
    SsrSatSpec* s = ssrFindSat(st, sat);
    return s ? ++s->clock_discontinuity : -1;
}

/// @brief Invert one phase signal's signal integer flag; returns the new flag.
inline bool flipSignalInteger(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    if (!p) return false;
    p->signal_integer = !p->signal_integer;
    return p->signal_integer;
}

/// @brief Set one phase signal's wide-lane integer flag; returns the new flag.
inline int setWideLaneInteger(SsrStoreState& st, const std::string& sat, const std::string& sig, int flag) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    if (!p) return -1;
    p->wide_lane_integer = flag;
    return flag;
}

/// @brief Toggle one phase signal's wide-lane integer flag between 0 and 1; returns the new flag.
inline int flipWideLaneInteger(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    if (!p) return -1;
    p->wide_lane_integer = (p->wide_lane_integer == 0) ? 1 : 0;
    return p->wide_lane_integer;
}

/// @brief Set the producer generation id (0 = none stamped); returns it.
inline std::uint32_t setGeneration(SsrStoreState& st, std::uint32_t id) {
    st.generation_id = id;
    return id;
}

/// @brief Move the producer generation id to the next value (a producer restart); returns it.
inline std::uint32_t bumpGeneration(SsrStoreState& st) { return ++st.generation_id; }

/// @brief Set one phase signal's hold generation (0 = not held); returns it.
inline std::uint32_t setHoldGeneration(SsrStoreState& st, const std::string& sat, const std::string& sig,
                                       std::uint32_t gen) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    if (!p) return 0;
    p->hold_generation = gen;
    return gen;
}

/// @brief Move one phase signal's hold generation to the next value; returns it.
inline std::uint32_t bumpHoldGeneration(SsrStoreState& st, const std::string& sat, const std::string& sig) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    return p ? ++p->hold_generation : 0;
}

/// @brief Add @p delta_m to one phase signal's bias (a value step, counters untouched); returns
///        the new bias.
inline double stepPhaseBias(SsrStoreState& st, const std::string& sat, const std::string& sig, double delta_m) {
    SsrPhaseSignalSpec* p = ssrFindPhase(st, sat, sig);
    if (!p) return 0.0;
    p->bias_m += delta_m;
    return p->bias_m;
}

/// @brief Set the age, at the state's epoch, of a satellite's records of the given kind.
inline bool ageSatellite(SsrStoreState& st, const std::string& sat, double age_s,
                         SsrRecordKind kind = SsrRecordKind::All) {
    SsrSatSpec* s = ssrFindSat(st, sat);
    if (!s) return false;
    if (kind == SsrRecordKind::OrbitClock || kind == SsrRecordKind::All) s->orbit_clock_age_s = age_s;
    if (kind == SsrRecordKind::CodeBias   || kind == SsrRecordKind::All) s->code_age_s        = age_s;
    if (kind == SsrRecordKind::PhaseBias  || kind == SsrRecordKind::All) s->phase_age_s       = age_s;
    return true;
}

/// @brief Take a satellite out of the state, keeping its spec (counters, flags, ages) for its
///        return.
inline bool withdrawSatellite(SsrStoreState& st, const std::string& sat) {
    const auto it = st.sats.find(sat);
    if (it == st.sats.end()) {
        ADD_FAILURE() << "SsrStoreFixture: satellite " << sat << " is not in the state";
        return false;
    }
    st.withdrawn[sat] = std::move(it->second);
    st.sats.erase(it);
    return true;
}

/// @brief Bring a withdrawn satellite back with the spec it had when it left.
inline bool restoreSatellite(SsrStoreState& st, const std::string& sat) {
    const auto it = st.withdrawn.find(sat);
    if (it == st.withdrawn.end()) {
        ADD_FAILURE() << "SsrStoreFixture: satellite " << sat << " was not withdrawn";
        return false;
    }
    st.sats[sat] = std::move(it->second);
    st.withdrawn.erase(it);
    return true;
}

/// @brief Move the state's epoch by @p dt_s; every record keeps its age, so it is re-stamped at the
///        new epoch (the next epoch of a stream that keeps refreshing).
inline GnssProcessor::GnssTime advanceSsrEpoch(SsrStoreState& st, double dt_s) {
    st.epoch = ssrShift(st.epoch, dt_s);
    return st.epoch;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

/// @brief Push one satellite's records into @p store without clearing it.  Each record is stamped
///        (receipt time and SSR epoch) at the state's epoch minus that record's age, with a header
///        of the same time and the state's IOD-SSR; the header the store keeps is the last one
///        pushed (orbit, clock, code bias, phase bias in that order).
inline void writeSsrSatellite(GnssProcessor::SsrCorrectionStore& store, const SsrStoreState& st,
                              const SsrSatSpec& s) {
    using namespace GnssProcessor;
    auto header = [&](const GnssTime& t) {
        SsrHeader h;
        h.epoch_time = t;
        h.iod_ssr    = st.iod_ssr;
        const auto id = SatId::parse(s.sat);
        h.system     = id ? id->sys : System::Unknown;
        return h;
    };
    const GnssTime t_oc = ssrShift(st.epoch, -s.orbit_clock_age_s);
    if (s.has_orbit) {
        SsrOrbitCorrection o;
        o.sat           = s.sat;
        o.iode          = s.iode;
        o.delta_rac     = s.delta_rac;
        o.received_time = t_oc;
        o.epoch_time    = t_oc;
        o.iod_ssr       = st.iod_ssr;
        store.pushOrbit(o, header(t_oc));
    }
    if (s.has_clock) {
        SsrClockCorrection c;
        c.sat                    = s.sat;
        c.c0                     = s.c0_m;
        c.received_time          = t_oc;
        c.epoch_time             = t_oc;
        c.iod_ssr                = st.iod_ssr;
        c.needs_orbit_correction = s.clock_needs_orbit;
        c.disc_counter           = s.clock_discontinuity;
        store.pushClock(c, header(t_oc));
    }
    if (!s.code.empty()) {
        const GnssTime t = ssrShift(st.epoch, -s.code_age_s);
        SsrCodeBias cb;
        cb.sat           = s.sat;
        cb.received_time = t;
        cb.iod_ssr       = st.iod_ssr;
        for (const auto& [sig, c] : s.code) {
            cb.bias_per_signal[sig] = c.bias_m;
            if (c.discontinuity) cb.disc_per_signal[sig] = *c.discontinuity;
        }
        store.pushCodeBias(cb, header(t));
    }
    if (!s.phase.empty()) {
        const GnssTime t = ssrShift(st.epoch, -s.phase_age_s);
        SsrPhaseBias pb;
        pb.sat           = s.sat;
        pb.received_time = t;
        pb.iod_ssr       = st.iod_ssr;
        for (const auto& [sig, p] : s.phase) {
            SsrPhaseBiasEntry e;
            e.bias_m                = p.bias_m;
            e.signal_integer        = p.signal_integer;
            e.wide_lane_integer     = p.wide_lane_integer;
            e.discontinuity_counter = p.discontinuity;
            e.hold_generation       = p.hold_generation;
            pb.bias_per_signal[sig] = e;
        }
        store.pushPhaseBias(pb, header(t));
    }
}

/// @brief Push the named satellite of @p st into @p store without clearing it; reports a failure
///        when the satellite is not in the state.
inline bool writeSsrSatellite(GnssProcessor::SsrCorrectionStore& store, const SsrStoreState& st,
                              const std::string& sat) {
    const auto it = st.sats.find(sat);
    if (it == st.sats.end()) {
        ADD_FAILURE() << "SsrStoreFixture: satellite " << sat << " is not in the state";
        return false;
    }
    writeSsrSatellite(store, st, it->second);
    return true;
}

/// @brief Make @p store hold exactly @p st: clear it, push every satellite of the state (withdrawn
///        ones are left out) and stamp the generation id.
inline void writeSsrState(GnssProcessor::SsrCorrectionStore& store, const SsrStoreState& st) {
    store.clear();
    for (const auto& [sat, s] : st.sats) writeSsrSatellite(store, st, s);
    store.setGenerationId(st.generation_id);
}

}  // namespace gnss_test
