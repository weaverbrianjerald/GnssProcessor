/**
 * @file FloatPhaseScale.cpp
 * @brief The receiver's own phase noise factor and its application to float satellites' phase rows
 *        (see include/realtime/FloatPhaseScale.hpp).
 */

#include "realtime/FloatPhaseScale.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

int applyPhaseVarianceScale(Eigen::VectorXd& R_v, const std::vector<std::string>& satellite_ids,
                            const std::vector<std::string>& signal_types,
                            const std::map<std::string, double>& scale) {
    if (scale.empty()) return 0;
    const std::size_t n = std::min({ static_cast<std::size_t>(R_v.size()), satellite_ids.size(),
                                     signal_types.size() });
    int scaled = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (signal_types[i].empty() || signal_types[i][0] != 'L') continue;
        const auto it = scale.find(satellite_ids[i]);
        if (it == scale.end()) continue;
        R_v(static_cast<Eigen::Index>(i)) *= it->second;
        ++scaled;
    }
    return scaled;
}

std::pair<std::string, std::string> splitRowSatellites(const std::string& id) {
    const auto dash = id.find('-');
    if (dash == std::string::npos) return { id, std::string{} };
    return { id.substr(0, dash), id.substr(dash + 1) };
}

double FloatPhaseScale::factor(char constellation) const {
    const auto it = k2_.find(constellation);
    return it == k2_.end() ? 1.0 : it->second;
}

std::size_t FloatPhaseScale::windowSize(char constellation) const {
    const auto it = window_.find(constellation);
    return it == window_.end() ? 0 : it->second.size();
}

void FloatPhaseScale::observeEpoch(double tow, const std::vector<FloatPhaseScaleRow>& rows,
                                   const std::map<std::string, double>& applied) {
    ++c_.epochs_seen;
    auto applied_of = [&applied](const std::string& sat) {
        const auto it = applied.find(sat);
        return it == applied.end() ? 1.0 : it->second;
    };
    struct Sums { double num{ 0.0 }, den{ 0.0 }; int rows{ 0 }, tail{ 0 }, phase{ 0 }; };
    std::map<char, Sums> sums;
    std::set<std::string> tail_now;
    for (const FloatPhaseScaleRow& r : rows) {
        if (r.signal.empty() || r.signal[0] != 'L' || r.sat_i.empty()) continue;
        if (!(r.r_applied > 0.0) || !(r.sr >= 0.0) || !std::isfinite(r.v)) continue;
        const char c = r.sat_i[0];
        Sums& s = sums[c];
        ++s.phase;
        const double f_i = applied_of(r.sat_i);
        const double f_p = r.sat_p.empty() ? f_i : applied_of(r.sat_p);
        if (f_i != f_p) {
            ++c_.rows_mixed;
            continue;
        }
        const double red = std::min(1.0, r.sr / r.r_applied);
        if (red < s_.min_redundancy || r.sr <= 0.0) continue;
        // The w-test under the applied model is N(0, k^2 / f) when the receiver's white factor is k^2.
        const double w = r.v / std::sqrt(r.sr);
        const double bar = s_.tail_z * std::sqrt(factor(c) / f_i);
        if (std::abs(w) > bar) {
            ++s.tail;
            ++c_.tail_rows;
            tail_now.insert(r.sat_i);
            continue;
        }
        s.num += f_i * r.v * r.v / r.r_applied;
        s.den += red;
        ++s.rows;
        ++c_.rows_used;
    }
    for (const auto& [c, s] : sums) {
        if (s.rows < 3 || s.den < 0.5) continue;
        if (s.phase > 0 && static_cast<double>(s.tail) > s_.freeze_share * static_cast<double>(s.phase)) {
            ++c_.values_frozen;
            continue;
        }
        auto& win = window_[c];
        win.emplace_back(tow, s.num / s.den);
        ++c_.values_pushed;
        while (!win.empty() && tow - win.front().first > s_.window_s) win.pop_front();
        if (static_cast<int>(win.size()) >= s_.min_epochs) {
            std::vector<double> v;
            v.reserve(win.size());
            for (const auto& e : win) v.push_back(e.second);
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            k2_[c] = v[v.size() / 2];
        }
    }
    for (const auto& sat : tail_now)
        if (!tail_sats_.count(sat)) ++c_.tail_enter;
    for (const auto& sat : tail_sats_)
        if (!tail_now.count(sat)) ++c_.tail_leave;
    tail_sats_ = std::move(tail_now);
}

std::map<std::string, double> FloatPhaseScale::scaleMap(const std::vector<std::string>& sats,
                                                        const std::set<std::string>& held) {
    std::map<std::string, double> map;
    std::set<std::string> observed(sats.begin(), sats.end());
    for (const auto& sat : observed) {
        if (sat.empty() || held.count(sat) || tail_sats_.count(sat) || !formed(sat[0])) continue;
        map[sat] = factor(sat[0]);
    }
    last_edges_.clear();
    for (const auto& [sat, f] : map) {
        if (!last_map_.count(sat)) {
            ++c_.to_bulk;
            last_edges_.emplace_back(sat, true);
        }
    }
    for (const auto& [sat, f] : last_map_) {
        if (!map.count(sat) && observed.count(sat)) {
            ++c_.to_present;
            last_edges_.emplace_back(sat, false);
        }
    }
    c_.scaled_sat_epochs += static_cast<long long>(map.size());
    last_map_ = map;
    return map;
}

std::string FloatPhaseScale::runEndLine() const {
    std::string per;
    for (const auto& [c, win] : window_) {
        if (!per.empty()) per += ", ";
        const auto it = k2_.find(c);
        per += it == k2_.end() ? fmt::format("{} not formed ({} values)", c, win.size())
                               : fmt::format("{} k={:.3f} ({} values)", c, std::sqrt(it->second), win.size());
    }
    return fmt::format("Float phase scale: {}; {} epochs, values pushed {} and frozen {}, phase rows used {} and "
                       "mixed {}; tail rows {} (satellites in {}, out {}); satellites to the receiver's factor {}, "
                       "back to the configured weight {}, scaled satellite-epochs {}",
                       per.empty() ? std::string{ "no constellation" } : per, c_.epochs_seen, c_.values_pushed,
                       c_.values_frozen, c_.rows_used, c_.rows_mixed, c_.tail_rows, c_.tail_enter, c_.tail_leave,
                       c_.to_bulk, c_.to_present, c_.scaled_sat_epochs);
}

} // namespace GnssProcessor
