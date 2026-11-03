/**
 * @file PhaseFrameHold.cpp
 * @brief The emitted phase-visible clock in a per-constellation held frame (see PhaseFrameHold.hpp).
 */

#include "network/emit/PhaseFrameHold.hpp"

#include "common/GnssIdentity.hpp"   // systemOfSat, letterOfSat, SignalId
#include "network/diag/RunTraces.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace GnssProcessor {

namespace {

/// The modelled pairs of a constellation, in the order they are tried.
const std::vector<std::pair<const char*, const char*>>& modelledPairs(System sys) {
    static const std::vector<std::pair<const char*, const char*>> gps{{"L1W", "L2W"}, {"L1C", "L2W"}};
    static const std::vector<std::pair<const char*, const char*>> gal{{"L1C", "L5Q"}, {"L1X", "L5X"}};
    static const std::vector<std::pair<const char*, const char*>> bds{{"L2I", "L6I"}};
    static const std::vector<std::pair<const char*, const char*>> none{};
    switch (sys) {
        case System::GPS:     return gps;
        case System::Galileo: return gal;
        case System::BeiDou:  return bds;
        default:              return none;
    }
}

/// The median of a non-empty vector (the mean of the two middle values for an even count).
double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// The |step| ladder bin: below 0.5, 1, 2, 5, 10 mm and above.
std::size_t stepBin(double step_m) {
    const double a = std::abs(step_m);
    return a < 0.0005 ? 0 : a < 0.001 ? 1 : a < 0.002 ? 2 : a < 0.005 ? 3 : a < 0.010 ? 4 : 5;
}

}  // namespace

std::optional<PhaseFrameSample> phaseFrameSample(const NetworkSatSolution& s, double u_m,
                                                 double t_s) {
    if (s.sat.empty() || !s.has_clock || !std::isfinite(s.clock_c0_m)) return std::nullopt;
    const System sys = systemOfSat(s.sat);
    for (const auto& [sig1, sig2] : modelledPairs(sys)) {
        const auto i1 = s.phase_bias.find(sig1);
        const auto i2 = s.phase_bias.find(sig2);
        if (i1 == s.phase_bias.end() || i2 == s.phase_bias.end()) continue;
        const auto id1 = SignalId::fromObsCode(sys, sig1);
        const auto id2 = SignalId::fromObsCode(sys, sig2);
        if (!id1 || !id2) continue;
        const double f1 = id1->frequencyHz(0), f2 = id2->frequencyHz(0);
        const double a  = f1 * f1 / (f1 * f1 - f2 * f2);
        const double p_if = a * i1->second.bias_m - (a - 1.0) * i2->second.bias_m;
        if (!std::isfinite(p_if)) continue;
        PhaseFrameSample out;
        out.pair = std::string(sig1) + "+" + sig2;
        out.gen1 = i1->second.hold_generation;
        out.gen2 = i2->second.hold_generation;
        out.v_m  = s.clock_c0_m - p_if + u_m;
        out.t_s  = t_s;
        return out;
    }
    return std::nullopt;
}

std::map<char, PhaseFrameStep> phaseFrameHoldEpoch(const std::vector<PhaseFrameEntry>& entries,
                                                   double t_s,
                                                   std::map<std::string, PhaseFrameSample>& prev,
                                                   std::map<char, double>& frame_g,
                                                   const PhaseFrameConfig& cfg) {
    std::map<char, PhaseFrameStep> out;
    std::map<char, std::vector<double>> innov;
    for (const PhaseFrameEntry& e : entries) {
        if (e.sat.empty()) continue;
        const char c = letterOfSat(e.sat);
        PhaseFrameStep& st = out[c];
        if (e.forced) {
            ++st.forced;
            continue;
        }
        if (!e.now) continue;
        const auto it = prev.find(e.sat);
        if (it == prev.end()) continue;
        const PhaseFrameSample& p = it->second;
        if (p.pair != e.now->pair || p.gen1 != e.now->gen1 || p.gen2 != e.now->gen2) continue;
        if (!(t_s - p.t_s <= cfg.max_gap_s)) continue;
        innov[c].push_back(e.now->v_m - p.v_m);
    }
    for (auto& [c, st] : out) {
        const auto iit = innov.find(c);
        st.voters = (iit == innov.end()) ? 0 : static_cast<int>(iit->second.size());
        if (st.voters >= cfg.min_voters) {
            st.moved  = true;
            st.step_m = medianOf(iit->second);
            const auto [git, inserted] = frame_g.emplace(c, 0.0);
            st.started = inserted;
            git->second += st.step_m;
            st.g_peak_m = git->second;
            if (std::abs(git->second) > cfg.leash_m) {
                git->second   = 0.0;
                st.reanchored = true;
            }
        }
        const auto git = frame_g.find(c);
        st.g_m = (git == frame_g.end()) ? 0.0 : git->second;
        if (!st.moved) st.g_peak_m = st.g_m;
    }
    prev.clear();
    for (const PhaseFrameEntry& e : entries)
        if (!e.sat.empty() && e.now) prev[e.sat] = *e.now;
    return out;
}

std::set<char> networkPhaseFrameHold(NetworkRunContext& ctx, NetworkRunTraces& traces,
                                     const SyncedEpoch& se, const std::set<char>& marked_consts,
                                     bool global_bump, const std::set<std::string>& retro_sats,
                                     long long n_emitted, NetworkSolutionEpoch& sol) {
    if (!ctx.knobs.phase_frame_hold || sol.sats.empty()) return {};
    net::NetworkEntities& entities = ctx.entities;   // written: constellations sats
    auto& c = ctx.counters;
    const double t_s = sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;

    // A warm resume's first armed epoch after emitted products: the frame restarts cold while
    // a receiver may hold products of the old one, so every constellation present is marked.
    const bool resume_seam = (c.phf_epochs == 0 && n_emitted > 0);
    ++c.phf_epochs;

    const std::map<std::string, double> track =
        ctx.knobs.apriori_track_on
            ? net::satMap(ctx.entities.sats, &net::SatelliteState::apriori_track_m)
            : std::map<std::string, double>{};
    std::vector<PhaseFrameEntry> entries;
    entries.reserve(sol.sats.size());
    for (const NetworkSatSolution& s : sol.sats) {
        if (s.sat.empty() || s.phase_bias.empty()) continue;
        PhaseFrameEntry e;
        e.sat = s.sat;
        const auto tit = track.find(s.sat);
        e.now = phaseFrameSample(s, tit != track.end() ? tit->second : 0.0, t_s);
        if (!e.now && s.has_clock) ++c.phf_no_pair;
        e.forced = global_bump || marked_consts.count(letterOfSat(s.sat)) != 0
                   || retro_sats.count(s.sat) != 0;
        entries.push_back(std::move(e));
    }

    std::map<std::string, PhaseFrameSample> prev =
        net::satMap(ctx.entities.sats, &net::SatelliteState::phase_frame_prev);
    std::map<char, double> frame_g;
    net::forEach(ctx.entities.constellations, &net::ConstellationState::phase_frame_g_m,
                 [&](System sys, double g) { frame_g.emplace(rinexLetter(sys), g); });
    PhaseFrameConfig cfg;
    cfg.leash_m = ctx.knobs.phase_frame_leash_m;
    const std::map<char, PhaseFrameStep> steps = phaseFrameHoldEpoch(entries, t_s, prev, frame_g, cfg);
    net::assignSat(entities, &net::SatelliteState::phase_frame_prev, prev);
    for (const auto& [cc, g] : frame_g)
        net::put(entities.constellations, systemFromLetter(cc), &net::ConstellationState::phase_frame_g_m) = g;

    std::set<char> marked;
    for (const auto& [cc, st] : steps) {
        const std::string cons(1, cc);
        const char* event = "";
        if (st.moved) {
            ++c.phf_voted[cc];
            c.phf_voters[cc] += st.voters;
            ++c.phf_step_hist[cc][stepBin(st.step_m)];
            c.phf_drained_m[cc] += std::abs(st.step_m);
        } else if (st.forced > 0 && st.voters == 0) {
            ++c.phf_marked[cc];
            event = "marked";
        } else {
            ++c.phf_paused[cc];
            event = "paused";
        }
        double& gmax = c.phf_max_abs_g_m[cc];
        gmax = std::max(gmax, std::abs(st.g_peak_m));
        if (st.reanchored) {
            ++c.phf_reanchors[cc];
            marked.insert(cc);
            event = "reanchor";
            traces.emitEvent(se.time, "PHASE_FRAME", "reanchor", cons, "", std::abs(st.step_m),
                             ctx.knobs.phase_frame_leash_m, "datum_mark",
                             fmt::format("voters={}; step={:.4f} m; the frame left the leash and "
                                         "returned to the solve frame", st.voters, st.step_m));
        }
        if (st.started) {
            if (!st.reanchored) event = "start";
            traces.emitEvent(se.time, "PHASE_FRAME", "frame_start", cons, "", st.step_m, std::nullopt,
                             "hold", fmt::format("voters={}", st.voters));
        }
        if (resume_seam) {
            marked.insert(cc);
            ++c.phf_resume_seams;
            traces.emitEvent(se.time, "PHASE_FRAME", "resume_seam", cons, "", std::nullopt,
                             std::nullopt, "datum_mark",
                             "the frame restarts cold after emitted products");
        }
        if (traces.phase_frame_trace)
            traces.phase_frame_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                                     << cc << ',' << st.voters << ',' << st.forced << ','
                                     << fmt::format("{:.6f}", st.step_m) << ','
                                     << fmt::format("{:.6f}", st.g_m) << ',' << event << '\n';
    }

    // Every phase bias of every satellite of a started constellation, in its frame.
    for (NetworkSatSolution& s : sol.sats) {
        if (s.sat.empty() || s.phase_bias.empty()) continue;
        const auto git = frame_g.find(letterOfSat(s.sat));
        if (git == frame_g.end() || git->second == 0.0) continue;
        for (auto& [signal, npb] : s.phase_bias) {
            (void)signal;
            npb.bias_m += git->second;
            ++c.phf_biases_shifted;
        }
    }
    return marked;
}

// ---- Run-end report ----

int networkPhaseFrameHoldRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    if (!ctx.knobs.phase_frame_hold) return 0;
    const auto& c = ctx.counters;
    std::set<char> conses;
    for (const auto& kv : c.phf_voted) conses.insert(kv.first);
    for (const auto& kv : c.phf_paused) conses.insert(kv.first);
    for (const auto& kv : c.phf_marked) conses.insert(kv.first);
    const auto at = [](const auto& m, char k) {
        const auto it = m.find(k);
        return it == m.end() ? typename std::decay_t<decltype(m)>::mapped_type{} : it->second;
    };
    std::string per;
    for (char cc : conses) {
        const long long voted = at(c.phf_voted, cc);
        const std::array<long long, 6> h = at(c.phf_step_hist, cc);
        per += fmt::format(
            "; {}: voted {}, paused {}, datum-change epochs {}, voters per voted epoch {:.1f}, |step| "
            "<0.5 mm {}, 0.5-1 mm {}, 1-2 mm {}, 2-5 mm {}, 5-10 mm {}, >=10 mm {}, drained {:.4f} m, "
            "largest |G| {:.4f} m, re-anchors {}",
            cc, voted, at(c.phf_paused, cc), at(c.phf_marked, cc),
            voted > 0 ? static_cast<double>(at(c.phf_voters, cc)) / static_cast<double>(voted) : 0.0,
            h[0], h[1], h[2], h[3], h[4], h[5], at(c.phf_drained_m, cc), at(c.phf_max_abs_g_m, cc),
            at(c.phf_reanchors, cc));
    }
    log->info("Network (phase frame hold): {} emitted epoch(s){}; resume seams {}; phase-bias values "
              "shifted {}; satellite-epochs without a complete modelled pair {}.  0 voted is inert "
              "wiring.",
              c.phf_epochs, per, c.phf_resume_seams, c.phf_biases_shifted, c.phf_no_pair);
    return 0;
}

}  // namespace GnssProcessor
