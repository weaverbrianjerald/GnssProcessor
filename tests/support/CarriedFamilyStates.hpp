#pragma once
/**
 * @file CarriedFamilyStates.hpp
 * @brief One network station's carried families put into a named state, and the steps the
 *        engine applies to them, for the response tests of the carried families.
 *
 * After each solve the network producer's harvest stashes every station's carried posteriors in
 * the run's entity tables, and the next epoch's station build folds them as priors.  A response
 * test needs a station whose carried state is known exactly before it applies an event.  The
 * builder here writes that state the way the harvest writes it: the same table cells
 * (net::put / net::putArc), the same stamping rules (localCarryStampBirth,
 * localCarryStampRelevel, the first-variance rule of each family), and, for the in-block route,
 * one harvest block shared by every column of the station, each entry holding its own row, with
 * the wet delay as the last row.  The families:
 *
 *   family                        cell                                          key
 *   leveling carry (ambiguities)  net::ArcState::amb                            (station, satellite, phase signal)
 *   receiver clock and its drift  net::StationSignalState::rx_clk, rx_clk_rate  "" (the primary clock)
 *   inter-system bias             net::StationSignalState::rx_clk               the system name ("Galileo")
 *   receiver phase bias           net::StationSignalState::rpb                  "<system>_RPB_<signal>"
 *   zenith wet delay              net::StationState::zwd                        the station
 *
 * The step helpers call the engine's own entry points:
 *   - a known chain frame step of a receiver (eq34ShiftCarry with the receiver's step keyed as
 *     the chain election keys it): the receiver phase bias moves by the step and every
 *     ambiguity of that receiver, constellation and band by minus the step;
 *   - a known chain frame step of a satellite (eq34ShiftCarry): its ambiguities at every
 *     station move by minus the step and every regression term on its phase bias by the step;
 *   - the wet delay's event response (zwdEventRespond): erase, or inflate and keep;
 *   - the float vehicle's slip and re-entry drop of one ambiguity (ambFloatDropFloored);
 *   - the receiver phase bias's harvest step test (rxPbStepExceeds) read against the carried
 *     entry, as the in-block harvest reads it.
 *
 * Not covered, because the engine has no entry point for them outside its full post-solve
 * harvest (networkHarvestRxClock): the receiver clock's jump response (the gate's test and the
 * erase of the clock and its drift are written inline in the harvest loop) and any step of an
 * inter-system bias (it has a gap prune and no step response).  The prediction both edges of the
 * clock carry share is the public kernel rxClockPredict, which a test can call on the state
 * written here.
 *
 * Every function takes the entity tables (and the carried state where the engine's entry point
 * takes it) by reference; nothing here owns state.
 */

#include "network/state/RunContext.hpp"   // first, as in the neighbouring carry tests: NetworkCarryState
#include "network/state/Entities.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/ar/AmbFloat.hpp"
#include "network/carry/RxPhaseBiasCarry.hpp"
#include "network/carry/ZwdCarry.hpp"
#include "network/carry/ZwdEdge.hpp"
#include "network/solve/ChainDatum.hpp"
#include "common/GnssIdentity.hpp"
#include "filter/StateKeySchema.hpp"

#include <gtest/gtest.h>
#include <Eigen/Dense>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace gnss_test {

/// One carried ambiguity of the leveling carry.
struct CarriedArc {
    std::string sat;         ///< Satellite ("G05").
    std::string signal;      ///< Phase signal ("L1C").
    double      value_abs;   ///< Absolute ambiguity (m).
    double      var;         ///< Conditional variance (m^2).
};

/// The primary receiver clock with its carried drift.
struct CarriedClock {
    double    value_abs;          ///< Absolute clock (m).
    double    var;                ///< Conditional variance (m^2).
    double    rate{0.0};          ///< Drift (m/s).
    double    rate_var{-1.0};     ///< Drift variance (m^2/s^2); negative: no rate yet.
    long long n_incr{0};          ///< Increments folded into the drift; 0: no rate yet.
};

/// One inter-system bias, carried as a clock-family column keyed by its system name.
struct CarriedIsb {
    GnssProcessor::System sys;   ///< The constellation whose bias this is.
    double value_abs;            ///< Absolute bias (m).
    double var;                  ///< Conditional variance (m^2).
};

/// One receiver phase bias of the chain datum.
struct CarriedRxPhaseBias {
    GnssProcessor::System sys;   ///< Constellation.
    std::string signal;          ///< Phase signal ("L1C").
    double value_abs;            ///< Absolute bias (m).
    double var;                  ///< Conditional variance (m^2).
};

/// The station's wet delay.
struct CarriedZwd {
    double value_abs;   ///< Absolute zenith wet delay (m).
    double var;         ///< Conditional variance (m^2).
};

/// One station's carried families as one harvest would leave them.
struct CarriedFamilyState {
    int       station{0};      ///< Station roster index.
    double    stamp_s{0.0};    ///< Continuous-GPS second of the harvest (each entry's last_seen_s).
    long long epoch{1};        ///< The harvest's loop-epoch count (the birth stamp of a new entry).
    /// One harvest block shared by every column (the in-block route); false leaves every entry
    /// without a block row, as on the diagonal route.
    bool      in_block{true};
    double    cross_m2{0.0};   ///< Every off-diagonal of the harvest block (m^2).
    /// The border regression written into every entry (empty: none).
    std::vector<GnssProcessor::CoupledShared> coupling;
    /// Open the wet delay's first-sighting record when the station had no entry, as the harvest
    /// does while the float vehicle's event response is armed.
    bool      zwd_edge{false};
    std::vector<CarriedArc>         arcs;   ///< Leveling carry.
    std::optional<CarriedClock>     clock;  ///< Receiver clock and drift.
    std::vector<CarriedIsb>         isbs;   ///< Inter-system biases.
    std::vector<CarriedRxPhaseBias> rpbs;   ///< Receiver phase biases.
    std::optional<CarriedZwd>       zwd;    ///< Wet delay (the last block row).
};

/// What a chain frame step moved through the carry.
struct CarryShiftResult {
    long long n_values{0};   ///< Carried values moved.
    long long n_terms{0};    ///< Regression terms moved.
};

/// The receiver phase-bias key of a constellation signal ("GPS_RPB_L1C").
inline std::string rxPhaseBiasKey(GnssProcessor::System sys, const std::string& signal) {
    return GnssProcessor::StateKeys::makeReceiverPhaseBiasKey(GnssProcessor::systemName(sys), signal);
}

/// The clock-family key of an inter-system bias (the system name).
inline std::string isbKey(GnssProcessor::System sys) { return GnssProcessor::systemName(sys); }

/// The arc key of the leveling carry.
inline GnssProcessor::net::ArcTuple arcTupleOf(int station, const std::string& sat,
                                               const std::string& signal) {
    return std::make_tuple(station, sat, signal);
}

// ---- Readers --------------------------------------------------------------------------------

/// The carried ambiguity of an arc, or null.
inline const GnssProcessor::LocalCarry* carriedArc(const GnssProcessor::net::NetworkEntities& ent,
                                                   int station, const std::string& sat,
                                                   const std::string& signal) {
    return GnssProcessor::net::getArc(ent.arcs, arcTupleOf(station, sat, signal),
                                      &GnssProcessor::net::ArcState::amb);
}

/// A clock-family entry ("" the primary clock, a system name an inter-system bias), or null.
inline const GnssProcessor::LocalCarry* carriedClockFamily(
    const GnssProcessor::net::NetworkEntities& ent, int station, const std::string& kind) {
    return GnssProcessor::net::get(ent.stations, GnssProcessor::StationId{station},
                                   &GnssProcessor::net::StationState::signals, kind,
                                   &GnssProcessor::net::StationSignalState::rx_clk);
}

/// The primary clock's drift state, or null.
inline const GnssProcessor::net::RxClkRate* carriedClockRate(const GnssProcessor::net::NetworkEntities& ent,
                                                        int station) {
    return GnssProcessor::net::get(ent.stations, GnssProcessor::StationId{station},
                                   &GnssProcessor::net::StationState::signals, std::string(),
                                   &GnssProcessor::net::StationSignalState::rx_clk_rate);
}

/// A receiver phase bias, or null.
inline const GnssProcessor::LocalCarry* carriedRxPhaseBias(
    const GnssProcessor::net::NetworkEntities& ent, int station, GnssProcessor::System sys,
    const std::string& signal) {
    return GnssProcessor::net::get(ent.stations, GnssProcessor::StationId{station},
                                   &GnssProcessor::net::StationState::signals,
                                   rxPhaseBiasKey(sys, signal),
                                   &GnssProcessor::net::StationSignalState::rpb);
}

/// The wet delay, or null.
inline const GnssProcessor::LocalCarry* carriedZwd(const GnssProcessor::net::NetworkEntities& ent,
                                                   int station) {
    return GnssProcessor::net::get(ent.stations, GnssProcessor::StationId{station},
                                   &GnssProcessor::net::StationState::zwd);
}

// ---- The builder ----------------------------------------------------------------------------

/**
 * @brief Writes one station's carried families into the entity tables as one harvest at
 *        @c s.stamp_s would leave them.
 *
 * Block rows follow the harvest's segment order: the ambiguities, the receiver clock then the
 * inter-system biases, the receiver phase biases, the wet delay last.  The block's diagonal is
 * each column's variance and every off-diagonal is @c s.cross_m2; a block that is not positive
 * definite fails the test.  An entry already present is re-stashed by the harvest's rules: its
 * birth stamps and first variance are kept, its value, variance, stamp, regression and block row
 * replaced.
 *
 * @param ent The entity tables (written).
 * @param s   The state.
 * @return The station's harvest block, or null without the in-block route or without columns.
 */
inline std::shared_ptr<GnssProcessor::PlHarvestBlock> stashCarriedFamilies(
    GnssProcessor::net::NetworkEntities& ent, const CarriedFamilyState& s) {
    namespace net = GnssProcessor::net;
    using GnssProcessor::LocalCarry;
    using GnssProcessor::StationId;

    std::vector<double> diag;
    for (const CarriedArc& a : s.arcs) diag.push_back(a.var);
    if (s.clock) diag.push_back(s.clock->var);
    for (const CarriedIsb& b : s.isbs) diag.push_back(b.var);
    for (const CarriedRxPhaseBias& p : s.rpbs) diag.push_back(p.var);
    if (s.zwd) diag.push_back(s.zwd->var);

    std::shared_ptr<GnssProcessor::PlHarvestBlock> blk;
    if (s.in_block && !diag.empty()) {
        const Eigen::Index n = static_cast<Eigen::Index>(diag.size());
        blk = std::make_shared<GnssProcessor::PlHarvestBlock>();
        blk->C = Eigen::MatrixXd::Constant(n, n, s.cross_m2);
        for (Eigen::Index k = 0; k < n; ++k) blk->C(k, k) = diag[static_cast<std::size_t>(k)];
        blk->stamp_s = s.stamp_s;
        EXPECT_EQ(Eigen::LLT<Eigen::MatrixXd>(blk->C).info(), Eigen::Success)
            << "the carried state's harvest block is not positive definite";
    }
    int row = 0;
    const auto place = [&](LocalCarry& e) {
        if (blk) {
            e.blk     = blk;
            e.blk_idx = row;
        }
        ++row;
    };

    // The leveling carry: the first-stash-wins birth, the first variance kept when positive,
    // the birth prior (no floor), the re-level clock on creation, the drop memory kept.
    for (const CarriedArc& a : s.arcs) {
        const auto key = arcTupleOf(s.station, a.sat, a.signal);
        const LocalCarry* prev = net::getArc(ent.arcs, key, &net::ArcState::amb);
        LocalCarry e{a.value_abs, a.var, s.stamp_s, s.coupling};
        e.var_first = (prev != nullptr && prev->var_first > 0.0) ? prev->var_first : a.var;
        e.var_birth = (prev != nullptr && prev->var_birth > 0.0) ? prev->var_birth : e.var_first;
        e.dropped_ep = prev != nullptr ? prev->dropped_ep : 0;
        e.drop_walk_ph = prev != nullptr ? prev->drop_walk_ph : 0;
        if (e.drop_walk_ph > 0 && prev != nullptr) e.drop_walk_ph = (e.drop_walk_ph == 1) ? 2 : 0;
        e.n_folds = prev != nullptr ? prev->n_folds : 0;
        GnssProcessor::localCarryStampBirth(e, prev, s.epoch, s.stamp_s);
        e.relevel_s = GnssProcessor::localCarryStampRelevel(prev != nullptr ? prev->relevel_s : 0.0,
                                                            prev == nullptr, false, s.stamp_s);
        place(e);
        net::putArc(ent, key, &net::ArcState::amb) = std::move(e);
    }

    // The clock family: the drift pair for the primary clock, a plain entry per bias.
    const auto stashClockFamily = [&](const std::string& kind, double value, double var) {
        const LocalCarry* prev = carriedClockFamily(ent, s.station, kind);
        LocalCarry e{value, var, s.stamp_s, s.coupling};
        e.var_first = prev != nullptr ? prev->var_first : var;
        GnssProcessor::localCarryStampBirth(e, prev, s.epoch, s.stamp_s);
        place(e);
        net::put(ent.stations, StationId{s.station}, &net::StationState::signals, kind,
                 &net::StationSignalState::rx_clk) = std::move(e);
    };
    if (s.clock) {
        GnssProcessor::net::RxClkRate& rt =
            net::put(ent.stations, StationId{s.station}, &net::StationState::signals, std::string(),
                     &net::StationSignalState::rx_clk_rate);
        rt.rate     = s.clock->rate;
        rt.rate_var = s.clock->rate_var;
        rt.n_incr   = s.clock->n_incr;
        rt.prev_abs = s.clock->value_abs;
        rt.prev_s   = s.stamp_s;
        rt.prev_var = s.clock->var;
        stashClockFamily(std::string(), s.clock->value_abs, s.clock->var);
    }
    for (const CarriedIsb& b : s.isbs) stashClockFamily(isbKey(b.sys), b.value_abs, b.var);

    // The receiver phase bias: as the clock family.
    for (const CarriedRxPhaseBias& p : s.rpbs) {
        const std::string key = rxPhaseBiasKey(p.sys, p.signal);
        const LocalCarry* prev = carriedRxPhaseBias(ent, s.station, p.sys, p.signal);
        LocalCarry e{p.value_abs, p.var, s.stamp_s, s.coupling};
        e.var_first = prev != nullptr ? prev->var_first : p.var;
        GnssProcessor::localCarryStampBirth(e, prev, s.epoch, s.stamp_s);
        place(e);
        net::put(ent.stations, StationId{s.station}, &net::StationState::signals, key,
                 &net::StationSignalState::rpb) = std::move(e);
    }

    // The wet delay: a fresh entry each harvest (no birth stamps), the last block row.
    if (s.zwd) {
        if (s.zwd_edge && carriedZwd(ent, s.station) == nullptr)
            GnssProcessor::zwdEdgeCreate(GnssProcessor::zwdEdgeOf(ent.stations, s.station), s.stamp_s);
        LocalCarry e{s.zwd->value_abs, s.zwd->var, s.stamp_s, s.coupling};
        place(e);
        net::put(ent.stations, StationId{s.station}, &net::StationState::zwd) = std::move(e);
    }
    return blk;
}

// ---- Steps ------------------------------------------------------------------------------------

/**
 * @brief A known chain frame step of one receiver on one constellation band, through
 *        eq34ShiftCarry, keyed as the chain election keys it (the receiver's step under its
 *        constellation letter and band signal, and the same step under its phase-bias key).
 * @param carry  The run's carried state.
 * @param ent    The entity tables (written: the receiver phase bias and the arcs of the band).
 * @param station Station roster index.
 * @param sys    Constellation.
 * @param signal Band signal ("L1C").
 * @param step_m The step (m); zero or non-finite moves nothing.
 * @return What moved.
 */
inline CarryShiftResult applyReceiverChainStep(GnssProcessor::NetworkCarryState& carry,
                                               GnssProcessor::net::NetworkEntities& ent,
                                               int station, GnssProcessor::System sys,
                                               const std::string& signal, double step_m) {
    const std::map<std::pair<std::string, std::string>, double> shift_sat;
    std::map<std::tuple<int, char, std::string>, double> shift_rxg;
    std::map<std::pair<int, std::string>, double> shift_rpb;
    shift_rxg[std::make_tuple(station, GnssProcessor::rinexLetter(sys), signal)] = step_m;
    shift_rpb[{station, rxPhaseBiasKey(sys, signal)}] = step_m;
    CarryShiftResult r;
    GnssProcessor::eq34ShiftCarry(carry, ent, shift_sat, shift_rxg, shift_rpb, r.n_values, r.n_terms);
    return r;
}

/**
 * @brief A known chain frame step of one satellite's phase bias, through eq34ShiftCarry.
 * @param carry  The run's carried state (its carried phase bias moves too).
 * @param ent    The entity tables (written: the satellite's arcs and the regression terms on it).
 * @param sat    Satellite ("G05").
 * @param signal Phase signal ("L1C").
 * @param step_m The step (m); zero or non-finite moves nothing.
 * @return What moved.
 */
inline CarryShiftResult applySatelliteChainStep(GnssProcessor::NetworkCarryState& carry,
                                                GnssProcessor::net::NetworkEntities& ent,
                                                const std::string& sat, const std::string& signal,
                                                double step_m) {
    std::map<std::pair<std::string, std::string>, double> shift_sat;
    shift_sat[{sat, signal}] = step_m;
    CarryShiftResult r;
    GnssProcessor::eq34ShiftCarry(carry, ent, shift_sat, {}, {}, r.n_values, r.n_terms);
    return r;
}

/**
 * @brief The wet delay's event response on one station (zwdEventRespond): erase when
 *        @p inflate_m is not positive, else keep the value and add @p inflate_m to its sigma in
 *        quadrature; either way the station's decoupling record opens.
 * @return True when the station carried a wet delay.
 */
inline bool applyWetDelayEvent(GnssProcessor::net::NetworkEntities& ent, int station,
                               GnssProcessor::ZwdEdgeCause cause, double now_s, double inflate_m,
                               GnssProcessor::ZwdCarryCounters& counters) {
    return GnssProcessor::zwdEventRespond(ent.stations, station, cause, now_s, inflate_m, counters);
}

/**
 * @brief The float vehicle's drop of one carried ambiguity at a slip or a re-entry
 *        (ambFloatDropFloored), with both floors already mapped to the line of sight.
 * @return The outcome; empty when the arc carries nothing.
 */
inline std::optional<GnssProcessor::AmbDropOutcome> applyArcSlipDrop(
    GnssProcessor::net::NetworkEntities& ent, int station, const std::string& sat,
    const std::string& signal, double birth_floor_m2, double drop_floor_m2, double now_s,
    long long now_ep) {
    GnssProcessor::LocalCarry* lc = GnssProcessor::net::getArc(
        ent.arcs, arcTupleOf(station, sat, signal), &GnssProcessor::net::ArcState::amb);
    if (lc == nullptr) return std::nullopt;
    return GnssProcessor::ambFloatDropFloored(*lc, birth_floor_m2, drop_floor_m2, now_s, now_ep);
}

/**
 * @brief Whether a harvested receiver phase bias at @p now_s would trip the in-block harvest's
 *        step test against the carried entry (rxPbStepExceeds over the entry's own gap).
 * @return False when the station carries no such entry.
 */
inline bool rxPhaseBiasStepTrips(const GnssProcessor::net::NetworkEntities& ent, int station,
                                 GnssProcessor::System sys, const std::string& signal,
                                 double value_now, double var_now, double now_s, double q_m2s,
                                 double max_gap_s) {
    const GnssProcessor::LocalCarry* prev = carriedRxPhaseBias(ent, station, sys, signal);
    if (prev == nullptr) return false;
    return GnssProcessor::rxPbStepExceeds(value_now, var_now, prev->value_abs, prev->var, q_m2s,
                                          now_s - prev->last_seen_s, max_gap_s);
}

}  // namespace gnss_test
