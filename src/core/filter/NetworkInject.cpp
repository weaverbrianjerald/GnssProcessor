/**
 * @file NetworkInject.cpp
 * @brief Event harness: producer-side fault injector (see NetworkInject.hpp).
 */
#include "filter/NetworkInject.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"
#include "common/PppUtils.hpp"
#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>

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
                          const std::shared_ptr<spdlog::logger>& log) {
        const auto& ic = config_.getNetworkConfig().inject;
        auto& st = ctx.inject;
        st.armed = false;
        if (!ic.enable || ic.events.empty()) return;

        st.events    = ic.events;
        st.n_applied.assign(st.events.size(), 0);
        st.onset_logged.assign(st.events.size(), false);
        st.utils = std::make_shared<PppUtils>(log_manager);
        st.armed = true;

        std::string list;
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            list += fmt::format("{}#{} {}@{:.1f}", i ? "; " : "", i, e.kind, e.tow);
            if (!e.station.empty()) list += " sta=" + e.station;
            if (!e.sat.empty())     list += " sat=" + e.sat;
            if (!e.signal.empty())  list += " sig=" + e.signal;
            if (e.kind == "slip")           list += fmt::format(" d={:.3f}cyc", e.delta_cycles);
            if (e.kind == "outage")         list += fmt::format(" dur={:.0f}s", e.duration_s);
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

        if (!st.armed) return se_raw;          // byte-identical path: no copy

        const double tow = se_raw.time.tow;

        // Partition this epoch's due events by whether they touch observations.
        std::vector<std::size_t> obs_due;
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            const bool due =
                (e.kind == "slip"            && started(e, tow))
             || (e.kind == "sat_clock_step"  && started(e, tow))
             || (e.kind == "rx_clock_step"   && started(e, tow))
             || (e.kind == "outage"          && inWindow(e, tow));
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
                auto sit = scratch.by_station.find(e.station);
                if (sit != scratch.by_station.end()) {
                    auto& v = sit->second;
                    const std::size_t before = v.size();
                    v.erase(std::remove_if(v.begin(), v.end(),
                                           [&](const RinexObservation& o) {
                                               return o.satellite == e.sat;
                                           }),
                            v.end());
                    touched = static_cast<long long>(before - v.size());
                }
                if (touched && onset)
                    row(e, "injected", e.duration_s,
                        fmt::format("outage_begin;dropped={};dur_s={:.0f}", touched,
                                    e.duration_s));
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

        // --- outage release row (the re-coupling edge, invariant #7) ----------------
        for (std::size_t i = 0; i < st.events.size(); ++i) {
            const auto& e = st.events[i];
            if (e.kind != "outage" || !st.onset_logged[i]) continue;
            const double t_end = e.tow + e.duration_s;
            if (tow >= t_end - kTowGuard && tow < t_end + 30.0 - kTowGuard)
                row(e, "released", e.duration_s,
                    fmt::format("outage_end;epochs_dropped={}", st.n_applied[i]));
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
            }
        }
        return *out;
    }

    int networkInjectRunEnd(NetworkRunContext& ctx,
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

} // namespace GnssProcessor
