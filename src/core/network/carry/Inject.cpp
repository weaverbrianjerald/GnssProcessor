/**
 * @file Inject.cpp
 * @brief Event harness: producer-side fault injector (see Inject.hpp).
 */
#include "network/carry/Inject.hpp"

#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "common/GnssIdentity.hpp"
#include "math/Coordinates.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>

namespace GnssProcessor {

    namespace {

        /// Half-epoch guard on the tow match, matching @c rhs_attrib_tow's 0.25 s.
        constexpr double kTowGuard = 0.25;

        /// @brief True when the event's window has opened at @p tow.
        bool started(const NetworkInjectEvent& ev, double tow) {
            return tow >= ev.tow - kTowGuard;
        }

        /// @brief True when a windowed event (@c outage) is still inside its window.
        bool inWindow(const NetworkInjectEvent& ev, double tow) {
            return started(ev, tow) && tow < ev.tow + ev.duration_s - kTowGuard;
        }

        /// @brief True while a held kind (@c sat_health, @c station_step) is in force:
        ///        from its tow, for @c duration_s, or to the run end when that is 0.
        bool held(const NetworkInjectEvent& ev, double tow) {
            return started(ev, tow)
                   && (ev.duration_s <= 0.0 || tow < ev.tow + ev.duration_s - kTowGuard);
        }

        /// @brief True at and after the closing edge of an event with a positive duration.
        bool ended(const NetworkInjectEvent& ev, double tow) {
            return ev.duration_s > 0.0 && tow >= ev.tow + ev.duration_s - kTowGuard;
        }

        /// @brief True when an outage acts after its window (a re-acquisition shift or flag).
        bool actsOnReturn(const NetworkInjectEvent& ev) {
            return ev.delta_cycles != 0.0 || ev.flag_lli;
        }

        /// @brief The outage's signal scope: the listed phase codes, or empty for "" / "all".
        std::set<std::string> signalList(const std::string& s) {
            std::set<std::string> out;
            if (s.empty() || s == "all") return out;
            std::string tok;
            for (const char c : s + ";") {
                if (c == ';' || c == ',') {
                    if (!tok.empty()) out.insert(tok);
                    tok.clear();
                } else if (!std::isspace(static_cast<unsigned char>(c))) {
                    tok.push_back(c);
                }
            }
            return out;
        }

        /// @brief The scope as written back in a trace row ("L1C+L2W", or "all").
        std::string scopeText(const std::set<std::string>& phases) {
            std::string out;
            for (const std::string& p : phases) out += (out.empty() ? "" : "+") + p;
            return out.empty() ? std::string("all") : out;
        }

        /**
         * @brief True when @p code is a channel of one of the listed phase signals.
         *
         * A receiver that loses lock on a signal loses its carrier and its code together,
         * so the scope of "L1C" is every observable of band 1 with tracking attribute C:
         * C1C, L1C, D1C and S1C.
         */
        bool inSignalScope(const std::string& code, const std::set<std::string>& phases) {
            if (code.size() < 3) return false;
            for (const std::string& p : phases)
                if (p.size() >= 3 && code[1] == p[1] && code[2] == p[2]) return true;
            return false;
        }

        /// @brief Mark one observable absent in one channel (the maps have no erase).
        template <class T>
        void dropSlot(ObsValueMap<T>& m, const std::string& code) {
            if (m.count(code)) m.at(code) = ObsAbsent<T>::value();
        }

        /// @brief Drop the observables of @p o in the signal scope; returns how many.
        long long dropSignals(RinexObservation& o, const std::set<std::string>& phases) {
            std::vector<std::string> codes;
            for (const auto& kv : o.measurements) codes.push_back(kv.first);
            long long n = 0;
            for (const std::string& code : codes) {
                if (!inSignalScope(code, phases)) continue;
                dropSlot(o.measurements, code);
                dropSlot(o.variances, code);
                dropSlot(o.lli_flags, code);
                ++n;
            }
            return n;
        }

        /// @brief True when the record still holds a code or a phase observable.
        bool hasCodeOrPhase(const RinexObservation& o) {
            for (const auto& kv : o.measurements) {
                const ObsType t = obsTypeOf(kv.first);
                if (t == ObsType::Code || t == ObsType::Phase) return true;
            }
            return false;
        }

        /**
         * @brief The re-acquisition after an outage: shift and flag the returned phase.
         *
         * Adds @p delta cycles to every phase observable in the scope (every phase code of
         * the record when @p phases is empty) and, when @p lli, raises LLI 0x1 on it.
         *
         * @return Phase observables touched.
         */
        long long shiftReturn(RinexObservation& o, const std::set<std::string>& phases,
                              double delta, bool lli) {
            std::vector<std::string> codes;
            for (const auto& kv : o.measurements) codes.push_back(kv.first);
            long long n = 0;
            for (const std::string& code : codes) {
                if (obsTypeOf(code) != ObsType::Phase) continue;
                if (!phases.empty() && !inSignalScope(code, phases)) continue;
                if (delta != 0.0) o.measurements[code] += delta;
                if (lli) o.lli_flags[code] |= 0x1;
                ++n;
            }
            return n;
        }

        /**
         * @brief Move one station's held coordinate by @p d (ECEF, metres).
         *
         * The three copies the epoch reads move together: the station's coordinate (the
         * geometry of the diagnostics and screens), the model state @c computeModels
         * evaluates the rows at, and the linearization points of the pinned position slots.
         */
        void moveHeldCoordinate(NetStation& sta, const Eigen::Vector3d& d) {
            sta.ecef += d;
            sta.pstate.position = sta.ecef;
            for (std::size_t a = 0; a < 3; ++a) {
                const int k = sta.params.position[a];
                if (k >= 0 && static_cast<std::size_t>(k) < sta.params.slots.size())
                    sta.params.slots[static_cast<std::size_t>(k)].x_lin +=
                            d(static_cast<Eigen::Index>(a));
            }
        }

        /// @brief The three copies of the held coordinate, as found (ECEF, then x_lin).
        std::array<double, 6> heldCoordinate(const NetStation& sta) {
            std::array<double, 6> x{ sta.ecef.x(), sta.ecef.y(), sta.ecef.z(), 0.0, 0.0, 0.0 };
            for (std::size_t a = 0; a < 3; ++a) {
                const int k = sta.params.position[a];
                x[3 + a] = (k >= 0 && static_cast<std::size_t>(k) < sta.params.slots.size())
                               ? sta.params.slots[static_cast<std::size_t>(k)].x_lin
                               : 0.0;
            }
            return x;
        }

        /// @brief Put back what @ref heldCoordinate read: the return is exact to the bit,
        ///        where adding the negated shift would round.
        void restoreHeldCoordinate(NetStation& sta, const std::array<double, 6>& x) {
            sta.ecef = Eigen::Vector3d(x[0], x[1], x[2]);
            sta.pstate.position = sta.ecef;
            for (std::size_t a = 0; a < 3; ++a) {
                const int k = sta.params.position[a];
                if (k >= 0 && static_cast<std::size_t>(k) < sta.params.slots.size())
                    sta.params.slots[static_cast<std::size_t>(k)].x_lin = x[3 + a];
            }
        }

        /**
         * @brief Write an unhealthy span [@p t0, @p t1) of @p sat into a health series.
         *
         * The series is a step function per satellite whose first step's status also holds
         * before its time.  The new steps are the old status OR the span, evaluated at every
         * old breakpoint, at both edges of the span and at one leading point (time 0) that
         * keeps the status before the first old step; consecutive equal statuses merge.
         */
        void forceUnhealthy(NetworkSbasis::SatHealth& h, const std::string& sat, double t0,
                            double t1) {
            const NetworkSbasis::SatHealth before = h;
            std::vector<double> ts{ 0.0, t0 };
            if (std::isfinite(t1)) ts.push_back(t1);
            const auto it = before.sats.find(sat);
            if (it != before.sats.end())
                for (const NetworkSbasis::SatHealthStep& x : it->second) ts.push_back(x.t_gps_s);
            std::sort(ts.begin(), ts.end());
            ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
            std::vector<NetworkSbasis::SatHealthStep> steps;
            for (const double t : ts) {
                const bool bad = (t >= t0 && t < t1) || before.unhealthyAt(sat, t);
                if (!steps.empty() && steps.back().unhealthy == bad) continue;
                steps.push_back(NetworkSbasis::SatHealthStep{ t, bad });
            }
            h.sats[sat] = std::move(steps);
        }

        /// @brief True on the first epoch at or after the event's tow.
        bool isOnsetEpoch(const NetworkInjectEvent& ev, double tow, bool already) {
            return !already && started(ev, tow);
        }

        /**
         * @brief Add @p step_m metres of non-dispersive range to one observation.
         *
         * Code observables (@c C*) take the metres directly; phase observables
         * (@c L*) take @c step_m/lambda cycles, because RINEX phase is in cycles
         * at this site and @c PppModel::computeModels derives both
         * @c PppObservation::observed (metres, via the wavelength) and
         * @c raw_observed (cycles, read by MW, GF and IF-TD) from this one entry.
         * A phase code whose wavelength does not resolve is left unchanged and
         * not counted, so the count includes only observables that moved.
         *
         * Shared by @c sat_clock_step (all stations, one satellite) and
         * @c rx_clock_step (one station, all satellites): the two kinds differ
         * only in which index they sweep, which is the duality between a
         * satellite-clock jump and a receiver-clock jump.
         *
         * @return Observables actually moved.
         */
        long long addRangeStep(RinexObservation& o, double step_m, const PppUtils* utils) {
            long long n = 0;
            // Snapshot the codes first: operator[] may reallocate a privately-grown
            // layout, which would invalidate an iterator over `measurements`.
            std::vector<std::string> codes;
            codes.reserve(o.measurements.size());
            for (const auto& kv : o.measurements) codes.push_back(kv.first);
            for (const auto& code : codes) {
                const ObsType type = obsTypeOf(code);
                if (type == ObsType::Code) {
                    o.measurements[code] += step_m;
                    ++n;
                } else if (type == ObsType::Phase) {
                    const double lam = utils
                            ? utils->getWavelength(o.system, code, o.satellite) : 0.0;
                    if (lam > 0.0) {
                        o.measurements[code] += step_m / lam;
                        ++n;
                    }
                }
            }
            return n;
        }

    } // namespace

    void networkInjectArm(NetworkRunContext& ctx, const GnssConfig& config_,
                          LogManager& log_manager,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkSetupResult* setup) {
        const auto& ic = config_.getNetworkConfig().inject;
        auto& st = ctx.inject;
        st.armed = false;
        if (!ic.enable || ic.events.empty()) return;

        st.events    = ic.events;
        st.n_applied.assign(st.events.size(), 0);
        st.onset_logged.assign(st.events.size(), false);
        st.release_logged.assign(st.events.size(), false);
        st.n_window.assign(st.events.size(), 0);
        st.signal_scope.assign(st.events.size(), {});
        st.station_index.assign(st.events.size(), -1);
        st.station_shift.assign(st.events.size(), { 0.0, 0.0, 0.0 });
        st.station_orig.assign(st.events.size(), { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 });
        for (std::size_t i = 0; i < st.events.size(); ++i)
            if (st.events[i].kind == "outage")
                st.signal_scope[i] = signalList(st.events[i].signal);
        st.setup = setup;
        st.utils = std::make_shared<PppUtils>(log_manager);
        st.armed = true;

        // The readers of a broadcast health flip on this posture.  The "exclude" door ran at
        // setup on the broadcast as parsed, so this seam removes the flagged satellite's
        // observations itself; the "pivot" and coverage elections query the series, into
        // which each flip's span is written here.
        {
            std::string readers;
            const auto add = [&readers](const char* r) {
                readers += readers.empty() ? std::string(r) : std::string(",") + r;
            };
            if (config_.networkSatHealth() == 2) add("exclude");
            if (config_.networkSatHealth() == 1) add("pivot");
            if (config_.networkPivotElection() != 0) add("pivot_election");
            st.health_readers = readers.empty() ? std::string("none") : readers;
            st.health_exclude = config_.networkSatHealth() == 2;
        }
        for (const NetworkInjectEvent& e : st.events) {
            if (e.kind == "sat_health") {
                if (st.health_readers == "none") {
                    log->warn("Network (fault inject): the sat_health flip of {} at TOW {:.1f} is "
                              "UNREAD on this posture (network.sat_health off and "
                              "network.pivot_election off): it fires, and nothing acts on it.",
                              e.sat, e.tow);
                } else if (setup) {
                    // The series is on the continuous GPS scale; a leg never spans a week
                    // rollover, so the week is the processing window's.
                    const double week_s = std::floor(setup->window_t0_s / 604800.0) * 604800.0;
                    const double t0 = week_s + e.tow - kTowGuard;
                    const double t1 = e.duration_s > 0.0
                                          ? week_s + e.tow + e.duration_s - kTowGuard
                                          : std::numeric_limits<double>::infinity();
                    forceUnhealthy(setup->sat_health, e.sat, t0, t1);
                    ++st.n_health_spans;
                }
            } else if (e.kind == "station_step") {
                bool found = false;
                if (setup)
                    for (const NetStation& s : setup->stations) found = found || s.name == e.station;
                if (!found)
                    log->error("Network (fault inject): station_step names '{}', which is not a "
                               "roster station of this run; it cannot fire, and the run end "
                               "will fail it.", e.station);
            }
        }

        std::string list;
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            list += fmt::format("{}#{} {}@{:.1f}", i ? "; " : "", i, e.kind, e.tow);
            if (!e.station.empty()) list += " sta=" + e.station;
            if (!e.sat.empty())     list += " sat=" + e.sat;
            if (!e.signal.empty())  list += " sig=" + e.signal;
            if (e.kind == "slip")           list += fmt::format(" d={:.3f}cyc", e.delta_cycles);
            if (e.kind == "fixed_cycle_plant") list += fmt::format(" d={:.0f}cyc", e.delta_cycles);
            if (e.kind == "outage") {
                list += fmt::format(" dur={:.0f}s", e.duration_s);
                if (e.delta_cycles != 0.0) list += fmt::format(" return_d={:.3f}cyc", e.delta_cycles);
                if (e.flag_lli)            list += " return_lli";
            }
            if (e.kind == "sat_health")
                list += fmt::format(" dur={:.0f}s readers={}", e.duration_s, st.health_readers);
            if (e.kind == "station_step")
                list += fmt::format(" enu=({:.4f},{:.4f},{:.4f})m dur={:.0f}s", e.east_m,
                                    e.north_m, e.up_m, e.duration_s);
            if (e.kind == "anchor_handover")list += " mode=" + e.mode;
            if (e.kind == "sat_clock_step" || e.kind == "rx_clock_step")
                list += fmt::format(" step={:.3f}m", e.step_m);
            // Oracle ambiguity pin (simulation only): the scope defines the event, so
            // it is printed in the arming line; otherwise a leg whose scope differed
            // from the one the operator wrote would be graded as the intended arm.
            if (e.kind == "oracle_pin")
                list += fmt::format(" scope={} sigma={:.2e}m", e.scope, e.sigma_m);
        }
        log->warn("Network (fault inject): ARMED with {} scripted event(s) — this run's "
                  "input is DELIBERATELY CORRUPTED and its products are a harness "
                  "fixture, not a solution.  [{}]", st.events.size(), list);
    }

    const SyncedEpoch& networkInjectEpoch(NetworkRunContext& ctx,
                                          const std::shared_ptr<spdlog::logger>& log,
                                          NetworkRunTraces& traces,
                                          const SyncedEpoch& se_raw,
                                          SyncedEpoch& scratch) {
        auto& st = ctx.inject;

        // The staged directives live for one epoch; a directive that survived its
        // epoch would act again at every later election.  All three vectors are
        // cleared here, ahead of the armed guard, because `anchor_now_slip` is
        // staged by the production anchor-slip handover
        // (network.cycle_slip.anchor_slip_handover), which also runs on unarmed
        // runs where the injector returns early.  Clearing empty containers does
        // not affect the solve, so the unarmed path stays byte-identical.
        st.anchor_now.clear();
        st.arc_erase_now.clear();
        st.anchor_now_slip.clear();
        st.fixed_cycle_plant_now.clear();

        if (!st.armed) return se_raw;          // byte-identical path: no copy

        const double tow = se_raw.time.tow;

        // Partition this epoch's due events by whether they touch observations.  An outage
        // acts inside its window (the drop) and, with a re-acquisition shift or flag, after
        // it; a health flag touches observations only where "exclude" reads it.
        std::vector<std::size_t> obs_due;
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            const bool due =
                (e.kind == "slip"            && started(e, tow))
             || (e.kind == "sat_clock_step"  && started(e, tow))
             || (e.kind == "rx_clock_step"   && started(e, tow))
             || (e.kind == "outage"          && inWindow(e, tow))
             || (e.kind == "outage"          && actsOnReturn(e) && ended(e, tow)
                 && (e.delta_cycles != 0.0 || !st.release_logged[i]))
             || (e.kind == "sat_health"      && st.health_exclude && held(e, tow));
            if (due) obs_due.push_back(i);
        }

        const SyncedEpoch* out = &se_raw;
        if (!obs_due.empty()) {
            scratch = se_raw;                  // the only copy, taken only when something is due
            out = &scratch;
        }

        auto row = [&](const NetworkInjectEvent& e, const char* action,
                       std::optional<double> stat, const std::string& detail) {
            traces.emitEvent(se_raw.time, "INJECT", e.kind.c_str(), e.sat, e.station,
                             stat, std::nullopt, action, detail);
        };

        // --- observation-side kinds -------------------------------------------------
        for (std::size_t idx : obs_due) {
            const auto& e = st.events[idx];
            const bool onset = isOnsetEpoch(e, tow, st.onset_logged[idx]);
            long long touched = 0;

            if (e.kind == "outage") {
                // Scope: one satellite or every one ("all", a whole-station outage); every
                // signal of it, or the listed ones while its others track.
                const bool all_sats = e.sat == "all";
                const std::set<std::string>& phases = st.signal_scope[idx];
                const auto in_sat = [&](const RinexObservation& o) {
                    return all_sats || o.satellite == e.sat;
                };
                auto sit = scratch.by_station.find(e.station);
                if (inWindow(e, tow)) {
                    if (sit != scratch.by_station.end()) {
                        auto& v = sit->second;
                        const std::size_t before = v.size();
                        if (phases.empty()) {
                            v.erase(std::remove_if(v.begin(), v.end(), in_sat), v.end());
                            touched = static_cast<long long>(before - v.size());
                            if (all_sats) st.n_outage_station_recs += touched;
                        } else {
                            for (auto& o : v)
                                if (in_sat(o)) touched += dropSignals(o, phases);
                            st.n_outage_signal_obs += touched;
                            // A record left with no code or phase is a record the receiver
                            // did not write; it leaves like one.
                            v.erase(std::remove_if(v.begin(), v.end(),
                                                   [](const RinexObservation& o) {
                                                       return !hasCodeOrPhase(o);
                                                   }),
                                    v.end());
                        }
                    }
                    if (touched) ++st.n_window[idx];
                    if (touched && onset) {
                        // The pre-existing plain-outage row text is kept; a scoped or
                        // re-acquiring outage appends its scope.
                        std::string detail = fmt::format("outage_begin;dropped={};dur_s={:.0f}",
                                                         touched, e.duration_s);
                        if (all_sats || !phases.empty() || actsOnReturn(e))
                            detail += fmt::format(";sats={};signals={};return_d={:.3f};lli={}",
                                                  all_sats ? "all" : e.sat, scopeText(phases),
                                                  e.delta_cycles, e.flag_lli ? 1 : 0);
                        row(e, "injected", e.duration_s, detail);
                    }
                } else if (sit != scratch.by_station.end()) {
                    // After the window: the re-acquisition.  The shift persists (the new
                    // integer); the LLI flag is raised at the first epoch back only.
                    const bool first_back = !st.release_logged[idx];
                    for (auto& o : sit->second)
                        if (in_sat(o))
                            touched += shiftReturn(o, phases, e.delta_cycles,
                                                   e.flag_lli && first_back);
                    if (first_back) st.n_outage_return_obs += touched;
                }
            } else if (e.kind == "sat_health") {
                // "exclude" reads the flag: the satellite leaves every station's record,
                // the held-out consumer's included, as the setup door removes it.
                for (auto& [sta, vec] : scratch.by_station) {
                    const std::size_t before = vec.size();
                    vec.erase(std::remove_if(vec.begin(), vec.end(),
                                             [&](const RinexObservation& o) {
                                                 return o.satellite == e.sat;
                                             }),
                              vec.end());
                    touched += static_cast<long long>(before - vec.size());
                }
                st.n_health_obs_removed += touched;
                // The flag's own ledger (fired, epochs in force, rows) is kept below with
                // the other held kind, because under "pivot" no observation moves.
                continue;
            } else if (e.kind == "slip") {
                auto sit = scratch.by_station.find(e.station);
                if (sit != scratch.by_station.end())
                    for (auto& o : sit->second) {
                        if (o.satellite != e.sat) continue;
                        if (!o.measurements.count(e.signal)) continue;
                        // RINEX phase is in cycles here; computeModels multiplies by
                        // the wavelength downstream, so one write moves both the
                        // metre-valued design row and the cycle-valued MW/GF/IF-TD feed.
                        // A step, not a ramp: the same delta every epoch from tow on.
                        o.measurements[e.signal] += e.delta_cycles;
                        if (e.flag_lli && onset)
                            o.lli_flags[e.signal] |= 0x1;   // "lost lock since last epoch"
                        ++touched;
                    }
                if (touched && onset)
                    row(e, "injected", e.delta_cycles,
                        fmt::format("slip_onset;signal={};lli={};rows={}", e.signal,
                                    e.flag_lli ? 1 : 0, touched));
            } else if (e.kind == "sat_clock_step") {
                // y = L~ - (rho - c.dt^s + ...).  Adding step_m to every code
                // observable and step_m/lambda cycles to every phase observable of
                // this satellite is equivalent to adding step_m to the a-priori
                // satellite clock.
                for (auto& [sta, vec] : scratch.by_station)
                    for (auto& o : vec) {
                        if (o.satellite != e.sat) continue;
                        touched += addRangeStep(o, e.step_m, st.utils.get());
                    }
                if (touched && onset)
                    row(e, "injected", e.step_m,
                        fmt::format("clock_step_onset;rows={}", touched));
            } else {   // rx_clock_step, the exact dual of sat_clock_step
                // Same residual, other index: y = L~ - (rho + c.dt_r - c.dt^s + ...),
                // so adding step_m metres to every code observable and step_m/lambda
                // cycles to every phase observable of every satellite at one station
                // is equivalent to adding step_m to that station's receiver clock
                // c.dt_r.  A receiver clock jump is station-common and
                // non-dispersive, and it persists: the same delta is applied every
                // epoch from tow on, never as a one-epoch spike (a receiver that
                // steps its clock does not step it back).  Nothing here is
                // satellite- or band-scoped; the loader refuses a `sat` or a
                // `signal` for this kind.
                long long n_sats = 0;
                auto sit = scratch.by_station.find(e.station);
                if (sit != scratch.by_station.end())
                    for (auto& o : sit->second) {
                        const long long n = addRangeStep(o, e.step_m, st.utils.get());
                        if (n) ++n_sats;
                        touched += n;
                    }
                if (touched && onset)
                    row(e, "injected", e.step_m,
                        fmt::format("rx_clock_step_onset;sats={};rows={}", n_sats, touched));
            }

            if (touched) {
                ++st.n_applied[idx];
                if (onset) {
                    st.onset_logged[idx] = true;
                    ++st.fired_by_kind[e.kind];
                    log->warn("Network (fault inject): TOW {:.1f} event #{} {} FIRED "
                              "(sta='{}' sat='{}' sig='{}') — {} observable(s) touched.",
                              tow, idx, e.kind, e.station, e.sat, e.signal, touched);
                }
            }
        }

        // --- held kinds: a health flag and a coordinate step, onset and epochs in force --
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            if ((e.kind != "sat_health" && e.kind != "station_step") || !held(e, tow)) continue;
            if (e.kind == "station_step" && !st.onset_logged[i]) {
                // The step lands once, at the first epoch at or after its tow; the roster
                // station keeps it until the closing edge moves it back.
                if (!st.setup) continue;
                std::vector<NetStation>& roster = st.setup->stations;
                int k = -1;
                for (std::size_t j = 0; j < roster.size(); ++j)
                    if (roster[j].name == e.station) { k = static_cast<int>(j); break; }
                if (k < 0) continue;                    // never fires: the run end fails it
                NetStation& sta = roster[static_cast<std::size_t>(k)];
                const Coordinates::Llh llh = Coordinates::ecefToLlh(sta.ecef);
                const Eigen::Vector3d d = Coordinates::enuToEcefMatrix(llh.lat_rad, llh.lon_rad)
                                          * Eigen::Vector3d(e.east_m, e.north_m, e.up_m);
                st.station_orig[i] = heldCoordinate(sta);
                moveHeldCoordinate(sta, d);
                st.station_index[i] = k;
                st.station_shift[i] = { d.x(), d.y(), d.z() };
                ++st.n_station_steps;
                row(e, "injected", d.norm(),
                    fmt::format("station_step;e={:.4f};n={:.4f};u={:.4f};dx={:.4f};dy={:.4f};"
                                "dz={:.4f};dur_s={:.0f}", e.east_m, e.north_m, e.up_m, d.x(),
                                d.y(), d.z(), e.duration_s));
            } else if (e.kind == "sat_health" && !st.onset_logged[i]) {
                row(e, "injected", e.duration_s,
                    fmt::format("health_flip;unhealthy;readers={};dur_s={:.0f}",
                                st.health_readers, e.duration_s));
            }
            ++st.n_applied[i];
            if (!st.onset_logged[i]) {
                st.onset_logged[i] = true;
                ++st.fired_by_kind[e.kind];
                log->warn("Network (fault inject): TOW {:.1f} event #{} {} FIRED "
                          "(sta='{}' sat='{}').", tow, i, e.kind, e.station, e.sat);
            }
        }

        // --- closing edges (the re-coupling edge, invariant #7) ----------------------
        // The first epoch at or after the end of an outage window, a health flag or a
        // coordinate step: one "released" row each, and the coordinate moves back.
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            if (!st.onset_logged[i] || st.release_logged[i] || !ended(e, tow)) continue;
            if (e.kind == "outage") {
                std::string detail =
                        fmt::format("outage_end;epochs_dropped={}", st.n_window[i]);
                if (actsOnReturn(e))
                    detail += fmt::format(";return_d={:.3f};lli={}", e.delta_cycles,
                                          e.flag_lli ? 1 : 0);
                row(e, "released", e.duration_s, detail);
            } else if (e.kind == "sat_health") {
                row(e, "released", e.duration_s,
                    fmt::format("health_flip_end;epochs_flagged={}", st.n_applied[i]));
            } else if (e.kind == "station_step") {
                const int k = st.station_index[i];
                if (k >= 0 && st.setup
                    && static_cast<std::size_t>(k) < st.setup->stations.size()) {
                    restoreHeldCoordinate(st.setup->stations[static_cast<std::size_t>(k)],
                                          st.station_orig[i]);
                    ++st.n_station_returns;
                }
                row(e, "released", e.duration_s,
                    fmt::format("station_return;epochs_stepped={}", st.n_applied[i]));
            } else {
                continue;
            }
            st.release_logged[i] = true;
        }

        // --- decision-side kinds: stage a one-epoch directive ------------------------
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            if (!isOnsetEpoch(e, tow, st.onset_logged[i])) continue;
            if (e.kind == "anchor_handover") {
                InjectAnchorDirective d;
                d.sat    = e.sat;
                d.signal = e.signal.empty() ? "all" : e.signal;
                d.cold   = (e.mode == "cold");
                st.anchor_now.push_back(std::move(d));
                // The ledger entry is closed after the election, at readback.
            } else if (e.kind == "arc_erase") {
                InjectArcErase a;
                a.station  = e.station;
                a.sat      = e.sat;
                a.mode     = e.mode.empty() ? std::string("erase") : e.mode;
                a.ev_index = i;
                // Band scope: "" / "all" => every signal (the production scope).
                if (!e.signal.empty() && e.signal != "all") {
                    std::string tok;
                    for (char c : e.signal + ";")
                        if (c == ';' || c == ',') {
                            if (!tok.empty()) a.signals.insert(tok);
                            tok.clear();
                        } else if (!std::isspace(static_cast<unsigned char>(c))) {
                            tok.push_back(c);
                        }
                }
                st.arc_erase_now.push_back(std::move(a));
                // The ledger entry is closed by the consumer (the T4 block for
                // every erase mode, the PASS-A drain for free_epoch), so a
                // directive that no seam could act on still reaches the
                // never-fired hard error.
            } else if (e.kind == "fixed_cycle_plant") {
                // Re-staged at every epoch until the fixed-cycle seam closes the ledger
                // entry: it fires at the first epoch at which the ledger holds the arc.
                st.fixed_cycle_plant_now.push_back(InjectFixedCyclePlant{
                    e.station, e.sat, e.signal, std::llround(e.delta_cycles), i});
            }
        }
        return *out;
    }

    int networkInjectRunEnd(const NetworkRunContext& ctx,
                            const std::shared_ptr<spdlog::logger>& log) {
        auto& st = ctx.inject;
        if (!st.armed) return 0;

        long long n_fired = 0;
        std::vector<std::string> never;
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            if (st.n_applied[i] > 0) { ++n_fired; continue; }
            never.push_back(fmt::format(
                "#{} {} (sta='{}' sat='{}' sig='{}') armed at tow {:.1f} never fired",
                i, e.kind, e.station, e.sat, e.signal, e.tow));
        }
        std::string by_kind;
        for (const auto& [k, n] : st.fired_by_kind)
            by_kind += fmt::format("{}{}={}", by_kind.empty() ? "" : ", ", k, n);
        if (by_kind.empty()) by_kind = "none";

        std::string by_mode;
        for (const auto& [m, n] : st.arc_erase_mode_acted)
            by_mode += fmt::format("{}{}={}", by_mode.empty() ? "" : ", ", m, n);
        if (by_mode.empty()) by_mode = "none";
        log->info("Network (fault inject): {} event(s) armed, {} fired ({}), {} never "
                  "fired.  Injected T4 erases refloated {} carried chord(s).  Study-1 "
                  "path census (mode=chords acted): {}.  Attribution: "
                  "network_events.csv tier INJECT (network.trace_events).",
                  st.events.size(), n_fired, by_kind, never.size(),
                  st.n_arc_erase_acted, by_mode);
        // The scenario kinds' two edges (card SMX), printed only when one is scheduled.
        bool any_scenario = false;
        for (const auto& e : st.events)
            any_scenario = any_scenario || e.kind == "sat_health" || e.kind == "station_step"
                           || (e.kind == "outage"
                               && (e.sat == "all" || !e.signal.empty() || actsOnReturn(e)));
        if (any_scenario) {
            long long n_released = 0;
            for (std::size_t i = 0; i < st.events.size(); ++i)
                if (st.release_logged[i]) ++n_released;
            log->info("Network (fault inject): scenario edges: {} closing edge(s) released; "
                      "outages dropped {} signal observable(s) and {} whole-station record(s), "
                      "{} phase observable(s) shifted or flagged at the return; health flips "
                      "read by '{}' ({} span(s) written into the series, {} observation(s) "
                      "removed under exclude); {} coordinate step(s) applied, {} moved back.",
                      n_released, st.n_outage_signal_obs, st.n_outage_station_recs,
                      st.n_outage_return_obs, st.health_readers, st.n_health_spans,
                      st.n_health_obs_removed, st.n_station_steps, st.n_station_returns);
        }

        if (!never.empty()) {
            // Same rule as the simulator's scripted schedule: a leg whose event did
            // not fire would otherwise be graded as a null and reported as "the
            // event had no effect".
            for (const auto& s : never)
                log->error("Network (fault inject): FAIL — {}.  Check the station/sat "
                           "names against the roster, the signal against the RINEX "
                           "obs codes, and the tow against this leg's epoch window.",
                           s);
            return 1;
        }
        return 0;
    }

    // ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

    // Run-end report: the event harness (fault injection).
    int networkInjectRunEnd(const NetworkRunEndView& v) {
        const NetworkRunContext& ctx = v.ctx;
        const std::shared_ptr<spdlog::logger>& log = v.log;

        // Event harness run-end (network/carry/Inject.cpp): the clause, and the hard error
        // for a schedule that never fired.  Read before the zero-emit guard so an armed but
        // silent schedule is reported even on a run that also produced nothing.
        const int inject_rc = networkInjectRunEnd(ctx, log);
        return inject_rc;
    }

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
