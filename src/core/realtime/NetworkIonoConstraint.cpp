#include "realtime/NetworkIonoConstraint.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <Eigen/Dense>
#include <fmt/format.h>

namespace GnssProcessor {
namespace {

constexpr int kMinPairSatellites = 3;     ///< Satellites the median of a receiver pair needs.
constexpr int kMinPlaneReceivers = 4;     ///< Receivers a plane needs (three unknowns plus one check).
constexpr double kHalfPi = 1.5707963267948966;   ///< Variance factor of a median of n values: pi / (2 n).

double median(std::vector<double> v) {
    const std::size_t n = v.size();
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2), v.end());
    const double hi = v[n / 2];
    if (n % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2));
    return 0.5 * (lo + hi);
}

/// The link of a receiver and satellite in an epoch, or nullptr.
const NetworkIonoLink* linkOf(const NetworkIonoEpoch& links, const std::string& station, const std::string& sat) {
    const auto r = links.find(station);
    if (r == links.end()) return nullptr;
    const auto s = r->second.find(sat);
    return s == r->second.end() ? nullptr : &s->second;
}

/// Corrections of one constellation from two receivers at the site.
void pairCorrections(const IonoFileSettings& st, const NetworkIonoEpoch& links, const std::vector<std::size_t>& idx,
                     const std::vector<IonoFileUserSat>& sats, std::vector<IonoFileCorrection>& out) {
    const std::string& a = st.stations[0];
    const std::string& b = st.stations[1];
    std::vector<std::size_t> cand;
    std::vector<double> d;
    for (const std::size_t i : idx) {
        const NetworkIonoLink* la = linkOf(links, a, sats[i].sat);
        const NetworkIonoLink* lb = linkOf(links, b, sats[i].sat);
        if (!la || !lb) {
            out[i].cause = IonoFileCause::Missing;
        } else if (la->elevation_deg < st.min_elev_deg || lb->elevation_deg < st.min_elev_deg) {
            out[i].cause = IonoFileCause::LowElevation;
        } else if (!la->integer || !lb->integer) {
            out[i].cause = IonoFileCause::NotInteger;
        } else {
            out[i].elevation_net_deg = 0.5 * (la->elevation_deg + lb->elevation_deg);
            cand.push_back(i);
            d.push_back(la->iono_m - lb->iono_m);
        }
    }
    if (static_cast<int>(cand.size()) < kMinPairSatellites) {
        for (const std::size_t i : cand) out[i].cause = IonoFileCause::TooFew;
        return;
    }
    const double m = median(d);
    for (std::size_t k = 0; k < cand.size(); ++k) {
        IonoFileCorrection& c = out[cand[k]];
        if (std::abs(d[k] - m) > st.test_k * c.sigma_m) {
            c.cause = IonoFileCause::Disagree;
            continue;
        }
        const NetworkIonoLink* la = linkOf(links, a, c.sat);
        const NetworkIonoLink* lb = linkOf(links, b, c.sat);
        c.valid = true;
        c.value_m = 0.5 * (la->iono_m + lb->iono_m + m);
    }
}

/// Corrections of one constellation from one receiver.
void singleCorrections(const IonoFileSettings& st, const NetworkIonoEpoch& links, const std::vector<std::size_t>& idx,
                       const std::vector<IonoFileUserSat>& sats, std::vector<IonoFileCorrection>& out) {
    for (const std::size_t i : idx) {
        const NetworkIonoLink* l = linkOf(links, st.stations[0], sats[i].sat);
        if (!l) {
            out[i].cause = IonoFileCause::Missing;
        } else if (l->elevation_deg < st.min_elev_deg) {
            out[i].cause = IonoFileCause::LowElevation;
        } else if (!l->integer) {
            out[i].cause = IonoFileCause::NotInteger;
        } else {
            out[i].valid = true;
            out[i].value_m = l->iono_m;
            out[i].elevation_net_deg = l->elevation_deg;
        }
    }
}

/// Corrections of one constellation from a plane over the receivers, evaluated at the user.
void planeCorrections(const IonoFileSettings& st, const NetworkIonoEpoch& links, const std::vector<std::size_t>& idx,
                      const std::vector<IonoFileUserSat>& sats, const IonoFileOffsets& offsets,
                      std::vector<IonoFileCorrection>& out) {
    std::vector<std::string> rx;   // receivers with a position
    for (const auto& s : st.stations)
        if (offsets.count(s)) rx.push_back(s);
    const auto above = [&](const NetworkIonoLink* l) { return l && l->elevation_deg >= st.min_elev_deg; };
    const auto usable = [&](const NetworkIonoLink* l) { return above(l) && l->integer; };
    // Reference: usable (above the cut, integer) at the most receivers, then the highest elevation at the user.
    int best_count = -1;
    std::size_t ref = idx.front();
    for (const std::size_t i : idx) {
        int n = 0;
        for (const auto& r : rx)
            if (usable(linkOf(links, r, sats[i].sat))) ++n;
        if (n > best_count || (n == best_count && sats[i].elevation_deg > sats[ref].elevation_deg)) {
            best_count = n;
            ref = i;
        }
    }
    if (best_count < kMinPlaneReceivers) {
        for (const std::size_t i : idx) out[i].cause = IonoFileCause::TooFew;
        return;
    }
    bool any = false;
    double ref_el = 0.0;
    int ref_n = 0;
    for (const auto& r : rx) {
        const NetworkIonoLink* lr = linkOf(links, r, sats[ref].sat);
        if (usable(lr)) {
            ref_el += lr->elevation_deg;
            ++ref_n;
        }
    }
    for (const std::size_t i : idx) {
        if (i == ref) continue;
        int present = 0, high = 0;
        double el_sum = 0.0;
        Eigen::MatrixXd A(static_cast<Eigen::Index>(rx.size()), 3);
        Eigen::VectorXd y(static_cast<Eigen::Index>(rx.size()));
        Eigen::Index n = 0;
        for (const auto& r : rx) {
            const NetworkIonoLink* ls = linkOf(links, r, sats[i].sat);
            const NetworkIonoLink* lr = linkOf(links, r, sats[ref].sat);
            if (ls) ++present;
            if (above(ls) && above(lr)) ++high;
            if (!usable(ls) || !usable(lr)) continue;
            const auto& en = offsets.at(r);
            A(n, 0) = 1.0;
            A(n, 1) = en[0];
            A(n, 2) = en[1];
            y(n) = ls->iono_m - lr->iono_m;
            el_sum += ls->elevation_deg;
            ++n;
        }
        if (n < kMinPlaneReceivers) {
            out[i].cause = present < kMinPlaneReceivers ? IonoFileCause::Missing
                         : high < kMinPlaneReceivers    ? IonoFileCause::LowElevation
                                                        : IonoFileCause::NotInteger;
            continue;
        }
        const Eigen::Vector3d coef = A.topRows(n).colPivHouseholderQr().solve(y.head(n));
        out[i].valid = true;
        out[i].value_m = coef(0);
        out[i].elevation_net_deg = el_sum / static_cast<double>(n);
        any = true;
    }
    if (any) {
        out[ref].valid = true;
        out[ref].value_m = 0.0;
        out[ref].elevation_net_deg = ref_el / static_cast<double>(ref_n);
    } else {
        out[ref].cause = IonoFileCause::TooFew;
    }
}

}  // namespace

const char* ionoFileCauseName(IonoFileCause cause) {
    switch (cause) {
    case IonoFileCause::Missing:      return "missing";
    case IonoFileCause::NotInteger:   return "not integer";
    case IonoFileCause::Disagree:     return "disagree";
    case IonoFileCause::TooFew:       return "too few";
    case IonoFileCause::LowElevation: return "low elevation";
    case IonoFileCause::Innovation:   return "innovation";
    case IonoFileCause::Pivot:        return "pivot";
    case IonoFileCause::Count:        break;
    }
    return "?";
}

bool parseIonoFileCombine(const std::string& name, IonoFileCombine& combine) {
    if (name == "pair") combine = IonoFileCombine::Pair;
    else if (name == "plane") combine = IonoFileCombine::Plane;
    else if (name == "single") combine = IonoFileCombine::Single;
    else return false;
    return true;
}

double IonoFileSettings::sigma(double elevation_deg, char constellation) const {
    const auto own = sigma_by_system.find(constellation);
    const std::vector<double>& table = own == sigma_by_system.end() ? sigma_m : own->second;
    if (sigma_elev_deg.empty() || table.size() != sigma_elev_deg.size()) return -1.0;
    std::size_t k = 0;
    while (k + 1 < sigma_elev_deg.size() && elevation_deg >= sigma_elev_deg[k + 1]) ++k;
    return table[k];
}

IonoFileOffsets ionoFileOffsets(const std::array<double, 3>& user_ecef,
                                const std::map<std::string, std::array<double, 3>>& receivers) {
    const double lon = std::atan2(user_ecef[1], user_ecef[0]);
    const double lat = std::atan2(user_ecef[2], std::hypot(user_ecef[0], user_ecef[1]));
    const Eigen::Vector3d east(-std::sin(lon), std::cos(lon), 0.0);
    const Eigen::Vector3d north(-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat));
    IonoFileOffsets out;
    for (const auto& [name, p] : receivers) {
        const Eigen::Vector3d d(p[0] - user_ecef[0], p[1] - user_ecef[1], p[2] - user_ecef[2]);
        out[name] = { d.dot(east) / 1000.0, d.dot(north) / 1000.0 };
    }
    return out;
}

std::vector<IonoFileCorrection> ionoFileCorrections(const IonoFileSettings& settings, const NetworkIonoEpoch* links,
                                                    const std::vector<IonoFileUserSat>& sats,
                                                    const IonoFileOffsets& offsets) {
    std::vector<IonoFileCorrection> out(sats.size());
    std::map<char, std::vector<std::size_t>> by_cons;
    for (std::size_t i = 0; i < sats.size(); ++i) {
        out[i].sat = sats[i].sat;
        out[i].sigma_m = settings.sigma(sats[i].elevation_deg, sats[i].sat.empty() ? char(0) : sats[i].sat[0]);
        if (!links || sats[i].sat.empty() || out[i].sigma_m < 0.0) {
            out[i].cause = IonoFileCause::Missing;
            continue;
        }
        by_cons[sats[i].sat[0]].push_back(i);
    }
    if (!links) return out;
    for (const auto& [cons, idx] : by_cons) {
        switch (settings.combine) {
        case IonoFileCombine::Pair:   pairCorrections(settings, *links, idx, sats, out); break;
        case IonoFileCombine::Single: singleCorrections(settings, *links, idx, sats, out); break;
        case IonoFileCombine::Plane:  planeCorrections(settings, *links, idx, sats, offsets, out); break;
        }
    }
    return out;
}

IonoFileDecision NetworkIonoConstraint::decide(const std::vector<IonoFileCorrection>& corr,
                                               const std::map<std::string, IonoFileUserState>& state,
                                               const std::map<std::string, double>& elevation) {
    IonoFileDecision dec;
    const double k = settings_.test_k;

    // The pivot of each constellation (its slant state is in no row) and its hand-overs.
    std::map<char, std::string> pivot_now;
    for (const auto& [sat, st] : state)
        if (st.pivot && !sat.empty()) pivot_now[sat[0]] = sat;
    for (const auto& [cons, sat] : pivot_now) {
        const auto it = pivot_.find(cons);
        if (it != pivot_.end() && it->second != sat) {
            ++counters_[cons].pivot_handovers;
            dec.events.push_back(fmt::format("pivot {} -> {}", it->second, sat));
        }
        pivot_[cons] = sat;
    }
    const auto isPivot = [&](const std::string& sat) {
        const auto it = state.find(sat);
        return it != state.end() && it->second.pivot;
    };

    // The innovation test, per constellation, over the non-pivot satellites with a correction and a state.
    std::map<char, std::vector<std::size_t>> cand;
    for (std::size_t i = 0; i < corr.size(); ++i)
        if (corr[i].valid && state.count(corr[i].sat) && !isPivot(corr[i].sat)) cand[corr[i].sat[0]].push_back(i);
    std::vector<bool> innov_ok(corr.size(), true);
    std::vector<double> innov(corr.size(), 0.0), bound(corr.size(), 0.0);
    for (const auto& [cons, idx] : cand) {
        std::vector<double> u;
        for (const std::size_t i : idx) u.push_back(corr[i].value_m - state.at(corr[i].sat).x_m);
        if (idx.size() >= 3) {
            // The centre is the median of u over the held satellites when two or more are held (a satellite whose
            // integers are not held rests on code and can sit decimetres off), otherwise over all; its own variance,
            // about pi / (2 n) times the typical variance of one value, enters every bound.
            std::vector<double> var(idx.size()), u_c, var_c;
            for (std::size_t j = 0; j < idx.size(); ++j) {
                const std::size_t i = idx[j];
                var[j] = corr[i].sigma_m * corr[i].sigma_m + std::max(0.0, state.at(corr[i].sat).var_m2);
                if (state.at(corr[i].sat).held) {
                    u_c.push_back(u[j]);
                    var_c.push_back(var[j]);
                }
            }
            if (u_c.size() < 2) {
                u_c = u;
                var_c = var;
            }
            const double m = median(u_c);
            const double var_centre = kHalfPi / static_cast<double>(u_c.size()) * median(var_c);
            for (std::size_t j = 0; j < idx.size(); ++j) {
                const std::size_t i = idx[j];
                innov[i] = u[j] - m;
                bound[i] = k * std::sqrt(var[j] + var_centre);
                innov_ok[i] = std::abs(innov[i]) <= bound[i];
            }
        } else if (idx.size() == 2) {
            double var = 0.0;
            for (const std::size_t i : idx)
                var += corr[i].sigma_m * corr[i].sigma_m + std::max(0.0, state.at(corr[i].sat).var_m2);
            const bool ok = std::abs(u[0] - u[1]) <= k * std::sqrt(var);
            for (std::size_t j = 0; j < 2; ++j) {
                innov[idx[j]] = u[j] - u[1 - j];
                bound[idx[j]] = k * std::sqrt(var);
                innov_ok[idx[j]] = ok;
            }
        }
    }

    // Each offered satellite's state.
    std::set<std::string> offered;
    for (std::size_t i = 0; i < corr.size(); ++i) {
        const IonoFileCorrection& c = corr[i];
        if (c.sat.empty()) continue;
        offered.insert(c.sat);
        IonoFileCounters& cnt = counters_[c.sat[0]];
        const bool has_state = state.count(c.sat) > 0;
        const bool pivot = isPivot(c.sat);
        const bool passing = c.valid && has_state && !pivot && innov_ok[i];
        const IonoFileCause cause = pivot ? IonoFileCause::Pivot
                                  : !c.valid ? c.cause
                                  : !has_state ? IonoFileCause::Missing
                                               : IonoFileCause::Innovation;
        SatUse& use = use_[c.sat];
        if (pivot && !use.active) {   // the pivot (never active): a later return is validated
            use.released = true;
            use.streak = 0;
            continue;
        }
        if (use.active) {
            if (passing) continue;
            use.active = false;
            use.released = true;
            use.streak = 0;
            ++cnt.releases[static_cast<std::size_t>(cause)];
            if (cause == IonoFileCause::Innovation) {
                dec.refloat.push_back(c.sat);
                ++cnt.refloats;
                dec.events.push_back(fmt::format("release {} innovation {:+.4f} m (bound {:.4f} m), refloat", c.sat,
                                                 innov[i], bound[i]));
            } else {
                dec.events.push_back(fmt::format("release {} {}", c.sat, ionoFileCauseName(cause)));
            }
        } else if (passing) {
            if (!use.released) {
                use.active = true;
                ++cnt.first_uses;
            } else if (++use.streak >= settings_.return_ep) {
                use.active = true;
                use.streak = 0;
                ++cnt.returns_validated;
                dec.events.push_back(fmt::format("return {}", c.sat));
            }
        } else {
            if (use.released && use.streak > 0) {
                ++cnt.returns_failed;
                dec.events.push_back(fmt::format("return failed {} {}", c.sat, ionoFileCauseName(cause)));
            }
            use.streak = 0;
        }
    }
    // Active satellites the user no longer offers.
    for (auto& [sat, use] : use_) {
        if (!use.active || offered.count(sat)) continue;
        use.active = false;
        use.released = true;
        use.streak = 0;
        ++counters_[sat[0]].releases[static_cast<std::size_t>(IonoFileCause::Missing)];
        dec.events.push_back(fmt::format("release {} missing (not offered)", sat));
    }

    // The per-satellite results (the trace reads them), and the applied corrections beyond 2 and 3 sigma.
    for (std::size_t i = 0; i < corr.size(); ++i) {
        if (corr[i].sat.empty()) continue;
        IonoFileSatResult r;
        r.sat = corr[i].sat;
        r.tested = corr[i].valid && state.count(corr[i].sat) > 0 && !isPivot(corr[i].sat);
        r.innov_m = innov[i];
        r.bound_m = bound[i];
        r.active = use_[corr[i].sat].active;
        dec.sats.push_back(r);
        if (r.active && r.tested && bound[i] > 0.0) {
            const double z = std::abs(innov[i]) * k / bound[i];   // innovation over the test's standard deviation
            IonoFileCounters& cnt = counters_[corr[i].sat[0]];
            auto& by = beyond_by_sat_[corr[i].sat];
            if (z > 2.0) { ++cnt.beyond2; ++by[0]; }
            if (z > 3.0) { ++cnt.beyond3; ++by[1]; }
        }
    }

    // Rows: per constellation, the active satellites against the pivot (when active) or the highest one.
    std::map<char, std::vector<std::string>> active;
    for (const auto& c : corr)
        if (!c.sat.empty() && use_[c.sat].active) active[c.sat[0]].push_back(c.sat);
    for (auto& [cons, sats] : active) {
        if (sats.size() < 2) continue;
        auto elev = [&](const std::string& s) {
            const auto it = elevation.find(s);
            return it == elevation.end() ? 0.0 : it->second;
        };
        // The reference: the active satellite with the highest elevation (the pivot is never active).
        const std::string ref = *std::max_element(
            sats.begin(), sats.end(), [&](const std::string& a, const std::string& b) { return elev(a) < elev(b); });
        IonoFileRows rows;
        rows.constellation = cons;
        rows.reference = ref;
        for (const auto& s : sats)
            if (s != ref) rows.sats.push_back(s);
        IonoFileCounters& cnt = counters_[cons];
        ++cnt.epochs_with_rows;
        cnt.rows += static_cast<long>(rows.sats.size());
        dec.rows.push_back(std::move(rows));
    }
    return dec;
}

std::string NetworkIonoConstraint::runEndLine() const {
    static const char* kCombine[] = { "pair", "plane", "single" };
    std::string stations;
    for (const auto& s : settings_.stations) stations += (stations.empty() ? "" : " ") + s;
    std::string out = fmt::format("Network iono file ({}, {})", kCombine[static_cast<int>(settings_.combine)],
                                  stations);
    if (counters_.empty()) return out + ": no satellite offered (inert)";
    for (const auto& [cons, c] : counters_) {
        std::string rel;
        for (std::size_t i = 0; i < c.releases.size(); ++i)
            rel += fmt::format("{}{} {}", rel.empty() ? "" : ", ", ionoFileCauseName(static_cast<IonoFileCause>(i)),
                               c.releases[i]);
        std::string by;
        for (const auto& [sat, n] : beyond_by_sat_)
            if (!sat.empty() && sat[0] == cons && n[0] > 0)
                by += fmt::format("{}{} {}/{}", by.empty() ? "" : ", ", sat, n[0], n[1]);
        out += fmt::format("; {}: epochs with rows {}, rows {}, first uses {}, releases ({}), refloats {}, returns "
                           "validated {}, failed {}, pivot hand-overs {}, applied beyond 2 / 3 sigma {} / {}{}",
                           cons, c.epochs_with_rows, c.rows, c.first_uses, rel, c.refloats, c.returns_validated,
                           c.returns_failed, c.pivot_handovers, c.beyond2, c.beyond3,
                           by.empty() ? "" : " (" + by + ")");
    }
    return out;
}

}  // namespace GnssProcessor
