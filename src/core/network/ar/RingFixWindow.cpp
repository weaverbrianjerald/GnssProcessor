/**
 * @file RingFixWindow.cpp
 * @brief The windowed residual monitor of held ring-fix links (network.ring_fix.window_monitor; measure only).  The
 *        statistics and their order are documented in network/ar/RingFixWindow.hpp.
 */
#include "network/ar/RingFixWindow.hpp"

#include "network/ar/RingFixDecide.hpp"         // RingFixDecideReadings, RingFixDecideAudit
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"          // NetStation
#include "network/state/RunContext.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>

namespace GnssProcessor {

const char* ringFixWindowFormName(RingFixWindowForm f) {
    switch (f) {
        case RingFixWindowForm::O2b:           return "o2b";
        case RingFixWindowForm::O1:            return "o1";
        case RingFixWindowForm::O2a:           return "o2a";
        case RingFixWindowForm::ScaledStation: return "scaled_station";
        case RingFixWindowForm::ScaledNetwork: return "scaled_network";
    }
    return "?";
}

std::vector<std::string> ringFixWindowRxClasses(const std::vector<std::string>& receiver_types) {
    std::map<std::string, int> count;
    for (const std::string& t : receiver_types) ++count[t];
    std::vector<std::string> out;
    out.reserve(receiver_types.size());
    for (const std::string& t : receiver_types) {
        if (t.empty()) { out.emplace_back("unknown"); continue; }
        if (count[t] >= kRingFixWindowMinOthers) { out.push_back(t); continue; }
        out.push_back(t.substr(0, t.find(' ')));   // the manufacturer: the type's first word
    }
    return out;
}

double ringFixWindowResolution(double r_m) {
    return std::strtod(fmt::format("{:.5f}", r_m).c_str(), nullptr);
}

double ringFixWindowReference(double lambda1, double lambda2) {
    return 0.5 * kRingFixWindowFootprintShare * (lambda1 * lambda2 / (lambda1 + lambda2));
}

namespace {

/// The median of @p v (reordered); the mean of the two middle values for an even count.
double medianOf(std::vector<double>& v) {
    const std::size_t n = v.size();
    const std::size_t m = n / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
    const double hi = v[m];
    if (n % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m));
    return (lo + hi) / 2.0;
}

/// "<station>|<system>|<sig1>/<sig2>": the station class.
std::string stationKey(const RingFixLinkId& id) {
    return std::to_string(id.station) + "|" + id.sat.substr(0, 1) + "|" + id.sig1 + "/" + id.sig2;
}

/// "<receiver class>|<system>|<sig1>/<sig2>": the network class.
std::string networkKey(const RingFixWindowMonitor& W, const RingFixLinkId& id) {
    const std::string rx = (id.station >= 0 && static_cast<std::size_t>(id.station) < W.rx_class.size())
                               ? W.rx_class[static_cast<std::size_t>(id.station)]
                               : std::string("unknown");
    return rx + "|" + id.sat.substr(0, 1) + "|" + id.sig1 + "/" + id.sig2;
}

bool isScaled(RingFixWindowForm f) {
    return f == RingFixWindowForm::ScaledStation || f == RingFixWindowForm::ScaledNetwork;
}

/// The share of the neighbours (other held links) whose mean residual over their rows since @p t0 reaches the bound,
/// among those with enough overlap; -1 with none.
void shareOf(const RingFixWindowMonitor& W, const std::vector<RingFixLinkId>& neighbours, double t0, double dt_s,
             double& share, int& n) {
    share = -1.0;
    n = 0;
    if (t0 < 0.0) return;
    int k = 0;
    for (const RingFixLinkId& y : neighbours) {
        const auto it = W.links.find(y);
        if (it == W.links.end() || it->second.hist.empty()) continue;
        const auto& h = it->second.hist;
        const auto j = std::lower_bound(h.begin(), h.end(), t0 - 1e-6,
                                        [](const std::pair<double, double>& e, double t) { return e.first < t; });
        const std::size_t m = static_cast<std::size_t>(h.end() - j);
        if (m == 0 || static_cast<double>(m) * dt_s < kRingFixWindowShareOverlapS) continue;
        const double before = (j == h.begin()) ? 0.0 : (j - 1)->second;
        const double mean = (h.back().second - before) / static_cast<double>(m);
        ++n;
        if (std::abs(mean) >= kRingFixWindowShareM) ++k;
    }
    if (n > 0) share = static_cast<double>(k) / static_cast<double>(n);
}

/// The statistic of a form for an edge.
double formValue(const RingFixWindowLink& L, RingFixWindowForm f) {
    switch (f) {
        case RingFixWindowForm::O2b:           return std::max(L.o2b_plus, L.o2b_minus);
        case RingFixWindowForm::O1: {
            double sum = 0.0;
            for (const auto& e : L.o1) sum += e.second;
            return L.o1.empty() ? 0.0 : sum / static_cast<double>(L.o1.size());
        }
        case RingFixWindowForm::O2a:           return static_cast<double>(L.o2a_run * (L.o2a_sign < 0 ? -1 : 1));
        case RingFixWindowForm::ScaledStation: return std::max(L.s_plus[0], L.s_minus[0]);
        case RingFixWindowForm::ScaledNetwork: return std::max(L.s_plus[1], L.s_minus[1]);
    }
    return 0.0;
}

/// A closing link, kept until its shares are formed.
struct Closing {
    RingFixLinkId id;
    RingFixWindowLink L;
    const char* cause{""};
};

}  // namespace

bool ringFixWindowScale(const std::deque<std::tuple<double, int, double>>& buf, const std::map<int, int>& members,
                        int own, double& scale, int& n) {
    n = static_cast<int>(members.size()) - (members.count(own) != 0 ? 1 : 0);
    if (n < kRingFixWindowMinOthers) return false;
    std::vector<double> v;
    v.reserve(buf.size());
    for (const auto& e : buf)
        if (std::get<1>(e) != own) v.push_back(std::get<2>(e));
    const double med = medianOf(v);
    for (double& x : v) x = std::abs(x - med);
    const double mad = medianOf(v);
    if (!(mad > 0.0)) return false;
    scale = kRingFixWindowMadToSigma * mad;
    return true;
}

std::vector<RingFixWindowEdge> ringFixWindowStep(RingFixWindowMonitor& W, const std::vector<RingFixWindowObs>& held,
                                                 double now_s, double dt_s, std::vector<RingFixWindowRow>* rows) {
    std::map<RingFixLinkId, const RingFixWindowObs*> now;
    for (const RingFixWindowObs& o : held) now[o.id] = &o;

    // ---- 1. Closes: a watched link not held now, or held in a new hold ----
    std::vector<Closing> closing;
    for (auto it = W.links.begin(); it != W.links.end();) {
        const auto o = now.find(it->first);
        const bool gone = o == now.end();
        const bool renewed = !gone && o->second->hold_s != it->second.hold_s;
        if (!gone && !renewed) { ++it; continue; }
        bool any = false;
        for (bool b : it->second.open) any = any || b;
        if (any) closing.push_back(Closing{it->first, it->second, gone ? "end" : "new_hold"});
        for (std::size_t f = 0; f < kRingFixWindowFormCount; ++f) {
            if (!it->second.open[f]) continue;
            ++W.closed[f];
            const auto form = static_cast<RingFixWindowForm>(f);
            if (isScaled(form)) {
                const int c = form == RingFixWindowForm::ScaledStation ? 0 : 1;
                const std::string key = c == 0 ? stationKey(it->first) : networkKey(W, it->first);
                if (--W.open_in_class[c][key] <= 0) W.open_in_class[c].erase(key);
            }
        }
        if (gone) {
            it = W.links.erase(it);
        } else {
            const int index = it->second.index;
            it->second = RingFixWindowLink{};
            it->second.index = index;
            ++it;
        }
    }

    // ---- 2. This epoch's rows: new holds, the class buffers, the histories ----
    std::vector<std::string> skey(held.size()), nkey(held.size());
    for (std::size_t i = 0; i < held.size(); ++i) {
        const RingFixWindowObs& o = held[i];
        auto ix = W.index.find(o.id);
        if (ix == W.index.end()) ix = W.index.emplace(o.id, static_cast<int>(W.index.size())).first;
        RingFixWindowLink& L = W.links[o.id];
        if (L.index < 0) {
            L.index = ix->second;
            L.hold_s = o.hold_s;
        }
        L.hold_s = o.hold_s;
        L.n1 = o.n1;
        L.n2 = o.n2;
        skey[i] = stationKey(o.id);
        nkey[i] = networkKey(W, o.id);
        ++W.link_epochs;
        if (!o.have_r) continue;
        ++W.with_residual;
        if (L.first_s < 0.0) L.first_s = now_s;
        L.hist.emplace_back(now_s, (L.hist.empty() ? 0.0 : L.hist.back().second) + o.r);
        for (int c = 0; c < 2; ++c) {
            const std::string& key = c == 0 ? skey[i] : nkey[i];
            W.buf[c][key].emplace_back(now_s, L.index, o.r);
            ++W.members[c][key][L.index];
        }
    }
    for (int c = 0; c < 2; ++c) {
        for (auto& [key, dq] : W.buf[c]) {
            auto& mem = W.members[c][key];
            while (!dq.empty() && std::get<0>(dq.front()) <= now_s - kRingFixWindowScaleS) {
                const int idx = std::get<1>(dq.front());
                if (--mem[idx] <= 0) mem.erase(idx);
                dq.pop_front();
            }
        }
    }

    // ---- 3. The statistics and the trips ----
    std::vector<RingFixWindowEdge> opens;
    std::vector<std::pair<std::size_t, RingFixWindowForm>> open_of;   // (held index, form) of each open
    if (rows) rows->assign(held.size(), RingFixWindowRow{});
    const std::map<std::string, int> in_episode[2] = {W.open_in_class[0], W.open_in_class[1]};
    for (std::size_t i = 0; i < held.size(); ++i) {
        const RingFixWindowObs& o = held[i];
        RingFixWindowLink& L = W.links.at(o.id);
        RingFixWindowRow row;
        row.id = o.id;
        row.have_r = o.have_r;
        row.r = o.r;
        bool trip[kRingFixWindowFormCount]{};
        if (o.have_r) {
            const double d = ringFixWindowReference(o.lambda1, o.lambda2);
            for (int c = 0; c < 2; ++c) {
                const std::string& key = c == 0 ? skey[i] : nkey[i];
                double s = 0.0;
                int n = 0;
                bool formed = false;
                if (in_episode[c].count(key) != 0 && L.scale[c] > 0.0) {
                    s = L.scale[c];
                    n = L.scale_n[c];
                    formed = true;
                } else if (ringFixWindowScale(W.buf[c][key], W.members[c][key], L.index, s, n)) {
                    L.scale[c] = s;
                    L.scale_n[c] = n;
                    formed = true;
                }
                row.scale_n[c] = n;
                if (!formed) { ++W.unscaled[c]; continue; }
                ++W.formed[c];
                const double z = o.r / s;
                const double dz = d / s;
                L.s_plus[c] = std::max(0.0, L.s_plus[c] + z - dz);
                L.s_minus[c] = std::max(0.0, L.s_minus[c] - z - dz);
                row.formed[c] = true;
                row.scale[c] = s;
                const double h = c == 0 ? kRingFixWindowHStation : kRingFixWindowHNetwork;
                if (L.s_plus[c] > h || L.s_minus[c] > h)
                    trip[static_cast<std::size_t>(c == 0 ? RingFixWindowForm::ScaledStation
                                                         : RingFixWindowForm::ScaledNetwork)] = true;
            }
            L.o2b_plus = std::max(0.0, L.o2b_plus + o.r - kRingFixWindowO2bKM);
            L.o2b_minus = std::max(0.0, L.o2b_minus - o.r - kRingFixWindowO2bKM);
            if (L.o2b_plus >= kRingFixWindowO2bHM || L.o2b_minus >= kRingFixWindowO2bHM)
                trip[static_cast<std::size_t>(RingFixWindowForm::O2b)] = true;
            L.o1.emplace_back(now_s, o.r);
            while (!L.o1.empty() && L.o1.front().first <= now_s - kRingFixWindowO1S) L.o1.pop_front();
            const double need = dt_s > 0.0 ? std::ceil(kRingFixWindowO1Cover * kRingFixWindowO1S / dt_s)
                                           : std::numeric_limits<double>::infinity();
            row.o1_valid = (now_s - L.first_s >= kRingFixWindowO1S - dt_s) && static_cast<double>(L.o1.size()) >= need;
            row.o1_mean = formValue(L, RingFixWindowForm::O1);
            if (row.o1_valid && std::abs(row.o1_mean) >= kRingFixWindowO1MeanM)
                trip[static_cast<std::size_t>(RingFixWindowForm::O1)] = true;
            const int v = std::abs(o.r) >= kRingFixWindowO2aM ? (o.r > 0.0 ? 1 : -1) : 0;
            if (v != 0 && v == L.o2a_sign) ++L.o2a_run;
            else if (v != 0) L.o2a_run = 1;
            else L.o2a_run = 0;
            L.o2a_sign = v;
            if (L.o2a_run >= kRingFixWindowO2aEpochs) trip[static_cast<std::size_t>(RingFixWindowForm::O2a)] = true;
        } else {
            for (int c = 0; c < 2; ++c) {
                row.scale[c] = L.scale[c];
                row.scale_n[c] = L.scale_n[c];
            }
            row.o1_mean = formValue(L, RingFixWindowForm::O1);
        }
        for (int c = 0; c < 2; ++c) {
            row.s_plus[c] = L.s_plus[c];
            row.s_minus[c] = L.s_minus[c];
        }
        row.o2b_plus = L.o2b_plus;
        row.o2b_minus = L.o2b_minus;
        row.o2a_run = L.o2a_run * (L.o2a_sign < 0 ? -1 : 1);
        for (std::size_t f = 0; f < kRingFixWindowFormCount; ++f) {
            if (!trip[f] || L.open[f]) continue;
            const auto form = static_cast<RingFixWindowForm>(f);
            L.open[f] = true;
            L.open_s[f] = now_s;
            ++W.opened[f];
            RingFixWindowEdge e;
            e.open = true;
            e.form = form;
            e.id = o.id;
            e.n1 = o.n1;
            e.n2 = o.n2;
            e.value = formValue(L, form);
            if (isScaled(form)) {
                const int c = form == RingFixWindowForm::ScaledStation ? 0 : 1;
                e.scale = L.scale[c];
                e.scale_n = L.scale_n[c];
                ++W.open_in_class[c][c == 0 ? skey[i] : nkey[i]];
            }
            e.age_s = now_s - o.hold_s;
            opens.push_back(e);
            open_of.emplace_back(i, form);
        }
        if (rows) (*rows)[i] = row;
    }

    // ---- 4. The shares: the station's and the satellite's other held links ----
    std::map<int, std::vector<RingFixLinkId>> by_station;
    std::map<std::string, std::vector<RingFixLinkId>> by_sat;
    for (const RingFixWindowObs& o : held) {
        by_station[o.id.station].push_back(o.id);
        by_sat[o.id.sat].push_back(o.id);
    }
    const auto neighbours = [&](const RingFixLinkId& x, bool station) {
        std::vector<RingFixLinkId> out;
        const std::vector<RingFixLinkId>& all = station ? by_station[x.station] : by_sat[x.sat];
        for (const RingFixLinkId& y : all) {
            if (station ? (y < x || x < y) : (y.station != x.station)) out.push_back(y);
        }
        return out;
    };
    if (rows) {
        for (std::size_t i = 0; i < held.size(); ++i) {
            RingFixWindowRow& row = (*rows)[i];
            const double t0 = W.links.at(held[i].id).first_s;
            shareOf(W, neighbours(held[i].id, true), t0, dt_s, row.share_sta, row.n_sta);
            shareOf(W, neighbours(held[i].id, false), t0, dt_s, row.share_sat, row.n_sat);
        }
    }
    for (std::size_t k = 0; k < opens.size(); ++k) {
        const std::size_t i = open_of[k].first;
        const double t0 = W.links.at(held[i].id).first_s;
        shareOf(W, neighbours(held[i].id, true), t0, dt_s, opens[k].share_sta, opens[k].n_sta);
        shareOf(W, neighbours(held[i].id, false), t0, dt_s, opens[k].share_sat, opens[k].n_sat);
    }

    // ---- 5. The edges: the closes (with their shares over the current held links), then the opens ----
    std::vector<RingFixWindowEdge> edges;
    for (const Closing& cl : closing) {
        double share_sta = -1.0, share_sat = -1.0;
        int n_sta = 0, n_sat = 0;
        shareOf(W, neighbours(cl.id, true), cl.L.first_s, dt_s, share_sta, n_sta);
        shareOf(W, neighbours(cl.id, false), cl.L.first_s, dt_s, share_sat, n_sat);
        for (std::size_t f = 0; f < kRingFixWindowFormCount; ++f) {
            if (!cl.L.open[f]) continue;
            const auto form = static_cast<RingFixWindowForm>(f);
            RingFixWindowEdge e;
            e.open = false;
            e.form = form;
            e.id = cl.id;
            e.n1 = cl.L.n1;
            e.n2 = cl.L.n2;
            e.value = formValue(cl.L, form);
            if (isScaled(form)) {
                const int c = form == RingFixWindowForm::ScaledStation ? 0 : 1;
                e.scale = cl.L.scale[c];
                e.scale_n = cl.L.scale_n[c];
            }
            e.share_sta = share_sta;
            e.n_sta = n_sta;
            e.share_sat = share_sat;
            e.n_sat = n_sat;
            e.age_s = now_s - cl.L.open_s[f];
            e.cause = cl.cause;
            edges.push_back(e);
        }
    }
    edges.insert(edges.end(), opens.begin(), opens.end());
    return edges;
}

void networkRingFixWindowMonitor(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideReadings& readings,
                                 const RingFixDecideAudit& audit) {
    if (!run.ctx.carry.ring_fix || !run.ctx.carry.ring_fix->window_monitor) return;   // the gate
    // ---- From the run and the epoch ----
    NetworkRingFix& rf = *run.ctx.carry.ring_fix;   // written: the monitor's state (rf.window)
    NetworkRunTraces& traces = run.traces;          // written: network_ring_fix_window.csv, RINGFIX window events
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const double now_s = ep.clock.pl_now_s;
    const double step_s = ep.clock.dt;
    const std::vector<RingFixDecideObs>& links = readings.links;
    const std::map<RingFixLinkId, bool>& held_before = audit.held_before;
    // ---- End of the bindings ----
    if (step_s > 0.0 && (rf.window.dt_s <= 0.0 || step_s < rf.window.dt_s)) rf.window.dt_s = step_s;
    const double dt_s = rf.window.dt_s;   // 0 before the second epoch: O1 is not valid and no share is formed
    std::vector<RingFixWindowObs> held;
    for (const RingFixDecideObs& o : links) {
        const auto hb = held_before.find(o.id);
        if (hb == held_before.end() || !hb->second) continue;
        const RingFixLink& L = rf.links.at(o.id);
        RingFixWindowObs w;
        w.id = o.id;
        w.n1 = L.n1;
        w.n2 = L.n2;
        w.hold_s = L.decided_s;
        w.lambda1 = o.lambda1;
        w.lambda2 = o.lambda2;
        w.have_r = o.have_resid && std::isfinite(o.resid_if_m);
        w.r = w.have_r ? ringFixWindowResolution(o.resid_if_m) : 0.0;
        held.push_back(w);
    }
    std::vector<RingFixWindowRow> rows;
    const std::vector<RingFixWindowEdge> edges =
        ringFixWindowStep(rf.window, held, now_s, dt_s, traces.ring_fix_window_trace.is_open() ? &rows : nullptr);
    const auto name = [&](int i) {
        return (i >= 0 && static_cast<std::size_t>(i) < stations.size()) ? stations[static_cast<std::size_t>(i)].name
                                                                           : std::string("#") + std::to_string(i);
    };
    const auto cell = [](double v, int prec) { return fmt::format("{:.{}f}", v, prec); };
    if (traces.ring_fix_window_trace.is_open()) {
        for (const RingFixWindowRow& r : rows) {
            traces.ring_fix_window_trace
                << t.week << ',' << fmt::format("{:.3f}", t.tow) << ',' << name(r.id.station) << ',' << r.id.sat << ','
                << r.id.sig1 << ',' << r.id.sig2 << ',' << (r.have_r ? cell(r.r, 5) : std::string()) << ','
                << (r.formed[0] ? cell(r.scale[0] * 1000.0, 4) : std::string()) << ',' << r.scale_n[0] << ','
                << cell(r.s_plus[0], 3) << ',' << cell(r.s_minus[0], 3) << ','
                << (r.formed[1] ? cell(r.scale[1] * 1000.0, 4) : std::string()) << ',' << r.scale_n[1] << ','
                << cell(r.s_plus[1], 3) << ',' << cell(r.s_minus[1], 3) << ',' << cell(r.o2b_plus, 5) << ','
                << cell(r.o2b_minus, 5) << ',' << (r.o1_valid ? cell(r.o1_mean, 5) : std::string()) << ','
                << r.o2a_run << ',' << (r.share_sta >= 0.0 ? cell(r.share_sta, 3) : std::string()) << ',' << r.n_sta
                << ',' << (r.share_sat >= 0.0 ? cell(r.share_sat, 3) : std::string()) << ',' << r.n_sat << '\n';
        }
    }
    for (const RingFixWindowEdge& e : edges) {
        const double h = e.form == RingFixWindowForm::O2b             ? kRingFixWindowO2bHM
                       : e.form == RingFixWindowForm::O1              ? kRingFixWindowO1MeanM
                       : e.form == RingFixWindowForm::O2a             ? static_cast<double>(kRingFixWindowO2aEpochs)
                       : e.form == RingFixWindowForm::ScaledStation   ? kRingFixWindowHStation
                                                                      : kRingFixWindowHNetwork;
        traces.emitEvent(t, "RINGFIX", e.open ? "window_open" : "window_close", e.id.sat, name(e.id.station),
                         e.value, h, ringFixWindowFormName(e.form),
                         fmt::format("sig1={} sig2={} n1={} n2={} scale_mm={:.4f} n_scale={} share_sta={} n_sta={} "
                                     "share_sat={} n_sat={} {}={:.0f}{}{}",
                                     e.id.sig1, e.id.sig2, e.n1, e.n2, e.scale * 1000.0, e.scale_n,
                                     e.share_sta >= 0.0 ? fmt::format("{:.3f}", e.share_sta) : std::string("none"),
                                     e.n_sta,
                                     e.share_sat >= 0.0 ? fmt::format("{:.3f}", e.share_sat) : std::string("none"),
                                     e.n_sat, e.open ? "age_s" : "episode_s", e.age_s, e.open ? "" : " cause=",
                                     e.open ? "" : e.cause));
    }
}

}  // namespace GnssProcessor
