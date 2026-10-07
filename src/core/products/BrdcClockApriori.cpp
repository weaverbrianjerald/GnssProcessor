/**
 * @file BrdcClockApriori.cpp
 * @brief Card NBP's native broadcast-clock a priori: selector, offset table, store feed
 *        and wire stamp.  See @c BrdcClockApriori.hpp for the design.
 */

#include "products/BrdcClockApriori.hpp"

#include "common/Constants.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace GnssProcessor {

namespace {

constexpr double kBdtToGpsOffsetSec = 1356.0 * 604800.0 + 14.0;
/// The bed's selection tolerance (`scl_brdc_clk.py` pick: ttom <= t + 1e-6).
constexpr double kTtomTolS = 1e-6;

double continuousSeconds(const GnssTime& t) {
    return static_cast<double>(t.week) * 604800.0 + t.tow;
}

double toeGpsSeconds(const BroadcastEphemeris& e) {
    double c = continuousSeconds(e.kepler.toe);
    if (e.system == System::BeiDou) c += kBdtToGpsOffsetSec;
    return c;
}

// LiveEphemerisStore's normalizeTk, so the clock polynomial is the same arithmetic.
double normalizeTk(double tk) {
    if (tk >  302400.0) tk -= 604800.0;
    if (tk < -302400.0) tk += 604800.0;
    return tk;
}

// The store's fit window (LiveEphemerisStore maxEphAgeForSystem): a stamped record must be
// one the consumer can evaluate.
double fitWindowS(System s) {
    switch (s) {
    case System::Galileo:
    case System::BeiDou:  return 2.0 * 3600.0;
    default:              return 4.0 * 3600.0;
    }
}

bool isKeplerian(System s) {
    return s == System::GPS || s == System::Galileo || s == System::BeiDou || s == System::QZSS;
}

} // namespace

BrdcClockApriori::BrdcClockApriori(std::vector<BroadcastEphemeris> records, double origin_s,
                                   double end_s, BrdcSelectRule rule, double node_s)
    : records_(std::move(records)), origin_s_(origin_s), end_s_(end_s),
      node_s_(node_s > 0.0 ? node_s : 30.0), rule_(rule) {
    stats_.n_records = records_.size();
    auto order = [&](int a, int b) {
        const double ta = brdcTransmissionGpsSeconds(records_[a]);
        const double tb = brdcTransmissionGpsSeconds(records_[b]);
        if (ta != tb) return ta < tb;
        return brdcTocGpsSeconds(records_[a]) < brdcTocGpsSeconds(records_[b]);
    };
    for (int i = 0; i < static_cast<int>(records_.size()); ++i) {
        const BroadcastEphemeris& e = records_[static_cast<std::size_t>(i)];
        if (e.satellite.empty() || !isKeplerian(e.system)) continue;
        if (!e.has_transmission_time) { ++stats_.n_no_ttom; continue; }
        if (e.health != 0)            { ++stats_.n_unhealthy; continue; }
        by_sat_[e.satellite].push_back(i);
        feed_order_.push_back(i);
        ++stats_.n_staged;
    }
    for (auto& [sat, idx] : by_sat_) std::stable_sort(idx.begin(), idx.end(), order);
    std::stable_sort(feed_order_.begin(), feed_order_.end(), order);
    stats_.n_sats = by_sat_.size();
    buildTable_();
}

double BrdcClockApriori::dayOrigin(double t_s) {
    return std::floor(t_s / 86400.0) * 86400.0;
}

std::optional<int> BrdcClockApriori::select(const std::string& sat, double t_s) const {
    const auto it = by_sat_.find(sat);
    if (it == by_sat_.end()) return std::nullopt;
    // rule_ is Ttom (the only rule): the latest in (ttom, toc) order transmitted by t.
    std::optional<int> out;
    for (int i : it->second) {
        if (brdcTransmissionGpsSeconds(records_[static_cast<std::size_t>(i)]) > t_s + kTtomTolS)
            break;
        out = i;
    }
    if (!out) return std::nullopt;
    const BroadcastEphemeris& e = records_[static_cast<std::size_t>(*out)];
    if (std::abs(t_s - toeGpsSeconds(e)) > fitWindowS(e.system)) return std::nullopt;
    return out;
}

double BrdcClockApriori::clockSeconds(int rec, double t_s) const {
    const BroadcastEphemeris& e = records_.at(static_cast<std::size_t>(rec));
    const KeplerianEphemeris& k = e.kepler;
    double q = t_s;
    if (e.system == System::BeiDou) q -= kBdtToGpsOffsetSec;
    const double dt = normalizeTk(q - continuousSeconds(k.toc));
    return k.af0 + k.af1 * dt + k.af2 * dt * dt;
}

void BrdcClockApriori::buildTable_() {
    // Nodes from the origin to the first node at or after end_s (one more for margin).
    const long long n_nodes =
        std::max<long long>(1, static_cast<long long>(std::ceil((end_s_ - origin_s_) / node_s_)) + 2);
    stats_.origin_s = origin_s_;
    stats_.end_s    = origin_s_ + static_cast<double>(n_nodes - 1) * node_s_;
    double o_min = std::numeric_limits<double>::max();
    double o_max = -std::numeric_limits<double>::max();
    for (const auto& [sat, idx] : by_sat_) {
        (void)idx;
        std::vector<Segment>& segs = segments_[sat];
        int    prev   = -1;     // last selected record (kept across a gap, as the bed)
        double off    = 0.0;
        for (long long n = 0; n < n_nodes; ++n) {
            const double t = origin_s_ + static_cast<double>(n) * node_s_;
            const std::optional<int> sel = select(sat, t);
            if (!sel) {
                if (!segs.empty() && segs.back().record >= 0)
                    segs.push_back(Segment{t, -1, off});
                continue;
            }
            if (prev >= 0 && *sel != prev) {
                // The bed's rule (scl_brdc_apriori.py:56-61): remove the switch step at t.
                off += clockSeconds(prev, t) - clockSeconds(*sel, t);
                ++stats_.n_switches;
            }
            if (segs.empty() || segs.back().record != *sel)
                segs.push_back(Segment{t, *sel, off});
            prev = *sel;
        }
        for (const Segment& s : segs) {
            if (s.record < 0) continue;
            o_min = std::min(o_min, s.offset_s);
            o_max = std::max(o_max, s.offset_s);
        }
    }
    if (o_min <= o_max) {
        stats_.offset_min_m = o_min * Constants::SPEED_OF_LIGHT;
        stats_.offset_max_m = o_max * Constants::SPEED_OF_LIGHT;
    }
}

const std::vector<BrdcClockApriori::Segment>* BrdcClockApriori::segments(const std::string& sat) const {
    const auto it = segments_.find(sat);
    return it == segments_.end() ? nullptr : &it->second;
}

std::optional<BrdcClockApriori::Evaluation>
BrdcClockApriori::evaluate(const std::string& sat, const GnssTime& t) const {
    const auto it = segments_.find(sat);
    if (it == segments_.end() || it->second.empty()) return std::nullopt;
    const double t_s = continuousSeconds(t);
    // The node at or before t, clamped to the built span: an emission time a fraction of
    // a second before 00:00 takes the first node's record (the continuous extension), and
    // a query past the span keeps the last segment.
    double node = origin_s_ + std::floor((t_s - origin_s_) / node_s_ + 1e-9) * node_s_;
    node = std::clamp(node, origin_s_, stats_.end_s);
    const std::vector<Segment>& segs = it->second;
    auto ub = std::upper_bound(segs.begin(), segs.end(), node,
                               [](double v, const Segment& s) { return v < s.start_s; });
    if (ub == segs.begin()) return std::nullopt;
    const Segment& seg = *std::prev(ub);
    if (seg.record < 0) return std::nullopt;
    Evaluation ev;
    ev.record   = seg.record;
    ev.offset_s = seg.offset_s;
    ev.brdc_s   = clockSeconds(seg.record, t_s);
    ev.clock_s  = ev.brdc_s + ev.offset_s;
    ev.iode     = LiveEphemerisStore::ssrIode(records_[static_cast<std::size_t>(seg.record)]);
    return ev;
}

std::size_t BrdcClockApriori::feedStore(LiveEphemerisStore& store, double now_s) {
    std::size_t n = 0;
    while (feed_cursor_ < feed_order_.size()) {
        const BroadcastEphemeris& e = records_[static_cast<std::size_t>(feed_order_[feed_cursor_])];
        if (brdcTransmissionGpsSeconds(e) > now_s + kTtomTolS) break;
        store.insertTransmitted(e);
        ++feed_cursor_;
        ++n;
    }
    return n;
}

std::vector<BrdcClockApriori::WireRow>
BrdcClockApriori::stampWire(NetworkSolutionEpoch& sol, const GnssTime& t,
                            const LiveEphemerisStore* store,
                            std::map<std::string, int>& prev_iode) const {
    std::vector<WireRow> rows;
    rows.reserve(sol.sats.size());
    for (auto& s : sol.sats) {
        if (!s.has_clock || s.sat.empty()) continue;
        WireRow r;
        r.sat     = s.sat;
        r.c_est_m = s.clock_c0_m;
        if (const auto ev = evaluate(s.sat, t)) {
            r.iode     = ev->iode;
            r.offset_m = ev->offset_s * Constants::SPEED_OF_LIGHT;
            r.brdc_m   = ev->brdc_s * Constants::SPEED_OF_LIGHT;
            s.clock_iode = ev->iode;
            s.clock_c0_m = s.clock_c0_m + r.offset_m;
            if (store) {
                const auto held = store->computeSatelliteClock(s.sat, t, ev->iode);
                r.held = held && held->iode == ev->iode;
            }
            const auto pit = prev_iode.find(s.sat);
            r.switched = (pit != prev_iode.end() && pit->second != ev->iode);
            prev_iode[s.sat] = ev->iode;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

} // namespace GnssProcessor
