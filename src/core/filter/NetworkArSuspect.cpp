/**
 * @file NetworkArSuspect.cpp
 * @brief The producer AR search's suspect-corner mask (card ARL, slip-ladder defect 3):
 *        the ledger's stamp, query and per-epoch fold, the per-search mask and its two
 *        edges, and the run-end report.
 *
 * What the mask is, who writes the ledger, what a stamp means and why the release is
 * time-only are in the header's file banner, include/filter/NetworkArSuspect.hpp.  The
 * search seam that calls the per-search functions is networkProducerAr
 * (filter/NetworkProducerAr.cpp).
 */

#include "filter/NetworkArSuspect.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.counters.netar_suspect, ctx.carry.ar_suspect (run end)
#include "config/GnssConfig.hpp"         // network.ambiguity_resolution (run-end gate)
#include "filter/NetworkRunEnd.hpp"      // NetworkRunEndView
#include "filter/StateKeySchema.hpp"     // StateKeys::parseAmbiguityKey

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace GnssProcessor {

namespace {

/// Bit position of a single @ref ArSuspectCause bit; -1 for anything else.
int causeIndex(std::uint8_t cause) {
    for (int k = 0; k < kArSuspectCauseCount; ++k)
        if (cause == (1u << k)) return k;
    return -1;
}

/// Tolerance (s) of the fold clock's second-to-epoch map: the carry stamps and the fold
/// are written from the same epoch time, so any real difference is a different epoch.
constexpr double kEpochSecondTolS = 1.0e-3;

/// The newest stamp a corner holds, any cause; 0 when none.
long long newestStamp(const ArSuspectCorner& k) {
    return *std::max_element(k.last_ep.begin(), k.last_ep.end());
}

/// Whether the corner holds a free-latch stamp at exactly @p ep.
bool freeStampAt(const ArSuspectLedger& L, const LosId& los, long long ep) {
    const auto it = L.corners.find(los);
    return it != L.corners.end()
        && it->second.last_ep[static_cast<std::size_t>(causeIndex(kArSuspectFree))] == ep;
}

/// Per-cause counts of a set of bit masks.
template <class Range, class BitsOf>
std::array<long long, kArSuspectCauseCount> countByCause(const Range& r, BitsOf bits_of) {
    std::array<long long, kArSuspectCauseCount> by{};
    for (const auto& x : r) {
        const std::uint8_t b = bits_of(x);
        for (int k = 0; k < kArSuspectCauseCount; ++k)
            if (b & (1u << k)) ++by[static_cast<std::size_t>(k)];
    }
    return by;
}

}  // namespace

const char* arSuspectCauseName(int k) {
    static constexpr const char* kNames[kArSuspectCauseCount] = {
        "free", "relevel", "drop", "raim", "mw_unknown", "gf_jump"};
    return (k >= 0 && k < kArSuspectCauseCount) ? kNames[k] : "?";
}

// =====================================================================
// The ledger
// =====================================================================

void arSuspectNote(ArSuspectLedger& L, const LosId& los, long long ep, std::uint8_t cause,
                   ArSuspectCounters* c) {
    const int k = causeIndex(cause);
    if (k < 0 || ep <= 0) return;
    ArSuspectCorner& corner = L.corners[los];
    long long& last = corner.last_ep[static_cast<std::size_t>(k)];
    if (ep <= last) return;   // not a new stamp: a carry stamp re-read, or a second signal
    last = ep;
    if (!c) return;
    ++c->stamps[static_cast<std::size_t>(k)];
    // One event, several causes: a stamp another cause of this corner already holds is
    // the same event.
    for (int j = 0; j < kArSuspectCauseCount; ++j)
        if (j != k && corner.last_ep[static_cast<std::size_t>(j)] == ep) return;
    ++c->events;
}

bool arSuspectNote(ArSuspectLedger& L, int sta, std::string_view sat, long long ep,
                   std::uint8_t cause, ArSuspectCounters* c) {
    const std::optional<SatId> s = SatId::parse(sat);
    if (!s || sta < 0) return false;
    arSuspectNote(L, LosId{StationId{sta}, *s}, ep, cause, c);
    return true;
}

std::uint8_t arSuspectBits(const ArSuspectLedger& L, const LosId& los, long long now_ep,
                           int window_ep) {
    if (window_ep <= 0) return 0;
    const auto it = L.corners.find(los);
    if (it == L.corners.end()) return 0;
    std::uint8_t bits = 0;
    for (int k = 0; k < kArSuspectCauseCount; ++k) {
        const long long e = it->second.last_ep[static_cast<std::size_t>(k)];
        // A stamp masks the searches e .. e + window - 1; a post-search stamp (the next
        // epoch's) is not live yet in the epoch that wrote it.
        if (e > 0 && now_ep >= e && now_ep - e < window_ep)
            bits |= static_cast<std::uint8_t>(1u << k);
    }
    return bits;
}

long long arSuspectEpochOfSecond(const std::deque<std::pair<long long, double>>& recent,
                                 double t_s) {
    if (recent.empty() || !std::isfinite(t_s)) return 0;
    if (t_s < recent.front().second - kEpochSecondTolS) return 0;   // older than the window
    long long ep = 0;
    for (const auto& [e, s] : recent) {
        if (s > t_s + kEpochSecondTolS) break;
        ep = e;
    }
    return ep;
}

void arSuspectFoldEpoch(ArSuspectLedger& L, long long now_ep, double now_s, int window_ep,
                        const RelevelEpochState& relevel,
                        const std::map<std::tuple<int, std::string, std::string>, LocalCarry>&
                            local_carry,
                        ArSuspectCounters* c) {
    if (window_ep <= 0) return;
    // The epoch of the previous fold (0 before the first): a drop stamped after it is new
    // to this fold.
    const long long prev_fold_ep = L.recent.empty() ? 0 : L.recent.back().first;
    // (1) The fold clock: the last window_ep folds.
    L.recent.emplace_back(now_ep, now_s);
    while (static_cast<int>(L.recent.size()) > window_ep) L.recent.pop_front();
    // (2) Age out the corners whose newest stamp has left the window.  A corner may stay
    //     in L.masking: its release is counted when the search next meets it.
    for (auto it = L.corners.begin(); it != L.corners.end();) {
        const long long newest = newestStamp(it->second);
        if (newest <= 0 || now_ep - newest >= window_ep) it = L.corners.erase(it);
        else ++it;
    }
    // (3) The carry stamps (file banner, the ledger table).
    for (const auto& [key, lc] : local_carry) {
        const int sta = std::get<0>(key);
        const std::string& sat = std::get<1>(key);
        // A drop no earlier fold has seen: this epoch's pre-search drop (the latch and the
        // stale gap), or one on an epoch whose search never ran.  A drop at the previous
        // fold's own epoch was written after that fold, by RAIM, whose own stamp owns it.
        if (lc.dropped_ep > prev_fold_ep && lc.dropped_ep <= now_ep
            && now_ep - lc.dropped_ep < window_ep)
            arSuspectNote(L, sta, sat, now_ep, kArSuspectDrop, c);
        // The re-level safety net: a relevel_s that is not a first stash, not written by
        // the drop of the same epoch and not the harvest re-stash of an arc in that
        // epoch's latch.
        if (lc.relevel_s > 0.0 && lc.relevel_s != lc.birth_s) {
            const long long e = arSuspectEpochOfSecond(L.recent, lc.relevel_s);
            if (e <= 0 || e == lc.dropped_ep) continue;
            const std::optional<SatId> s = SatId::parse(sat);
            if (!s || sta < 0) continue;
            const LosId los{StationId{sta}, *s};
            if (!freeStampAt(L, los, e)) arSuspectNote(L, los, e, kArSuspectRelevel, c);
        }
    }
    // (4) This epoch's free-epoch latch, whatever detector filled it.
    for (const auto& [sta, amb_key] : relevel.free_ambs) {
        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
        if (parts) arSuspectNote(L, sta, parts->satellite, now_ep, kArSuspectFree, c);
    }
}

// =====================================================================
// One search epoch
// =====================================================================

ArSuspectCornerState arSuspectCombine(ArSuspectCornerState a, ArSuspectCornerState b,
                                      ArSuspectCornerState c, ArSuspectCornerState d) {
    ArSuspectCornerState s;
    s.bits     = static_cast<std::uint8_t>(a.bits | b.bits | c.bits | d.bits);
    s.released = a.released || b.released || c.released || d.released;
    return s;
}

ArSuspectSearch arSuspectOpenSearch(long long now_ep, int window) {
    ArSuspectSearch S;
    S.now_ep = now_ep;
    S.window = std::max(0, window);
    return S;
}

ArSuspectCornerState arSuspectPoolCorner(ArSuspectSearch& S, const ArSuspectLedger& L,
                                         int sta, std::string_view sat) {
    ArSuspectCornerState st;
    if (S.window <= 0 || sta < 0) return st;
    const std::optional<SatId> s = SatId::parse(sat);
    if (!s) return st;
    const LosId los{StationId{sta}, *s};
    st.bits = arSuspectBits(L, los, S.now_ep, S.window);
    if (st.bits != 0) {
        S.pool_suspect[los] = st.bits;
    } else if (L.masking.count(los) > 0) {
        st.released = true;
        S.pool_released.insert(los);
    }
    return st;
}

void arSuspectMaskBasis(ArSuspectSearch& S, const std::vector<ArSuspectCornerState>& dd_state) {
    if (S.window <= 0) return;
    const std::size_t n = dd_state.size();
    S.masked.assign(n, 0);
    S.reentry.assign(n, 0);
    S.dd_bits.assign(n, 0);
    S.n_masked = 0;
    S.n_reentry = 0;
    for (std::size_t a = 0; a < n; ++a) {
        const ArSuspectCornerState& st = dd_state[a];
        S.dd_bits[a] = st.bits;
        if (st.bits != 0) {
            S.masked[a] = 1;
            ++S.n_masked;
        } else if (st.released) {
            S.reentry[a] = 1;
            ++S.n_reentry;
        }
    }
    S.masked_by = countByCause(S.dd_bits, [](std::uint8_t b) { return b; });
}

std::string arSuspectCloseSearch(ArSuspectSearch& S, ArSuspectLedger& L,
                                 ArSuspectCounters& C, const ArSuspectReentry& re,
                                 double tow) {
    if (S.window <= 0) return std::string();
    const auto corners_by = countByCause(
        S.pool_suspect, [](const std::pair<const LosId, std::uint8_t>& kv) { return kv.second; });
    // --- The decoupling edge.
    ++C.epochs;
    if (S.n_masked > 0) ++C.epochs_masked;
    C.dd_total  += static_cast<long long>(S.masked.size());
    C.dd_masked += S.n_masked;
    C.corners   += static_cast<long long>(S.pool_suspect.size());
    for (std::size_t k = 0; k < static_cast<std::size_t>(kArSuspectCauseCount); ++k) {
        C.dd_masked_by[k] += S.masked_by[k];
        C.corners_by[k]   += corners_by[k];
    }
    if (S.n_masked > C.max_dd_masked) {
        C.max_dd_masked     = S.n_masked;
        C.max_dd_masked_tow = tow;
    }
    for (const auto& [los, bits] : S.pool_suspect) {
        (void)bits;
        L.masking.insert(los);
        const auto it = L.corners.find(los);
        if (it == L.corners.end()) continue;
        const long long newest = newestStamp(it->second);
        if (newest != it->second.met_ep) {
            ++C.events_met;
            it->second.met_ep = newest;
        }
    }
    // --- The release edge (time-only: nothing above re-validated these corners).
    if (!S.pool_released.empty()) ++C.epochs_released;
    C.corners_released += static_cast<long long>(S.pool_released.size());
    for (const LosId& los : S.pool_released) L.masking.erase(los);
    C.dd_reentered            += re.dd;
    C.dd_reentered_fixed      += re.dd_fixed;
    C.pairs_reentered         += re.pairs;
    C.pairs_reentered_singles += re.pairs_singles;
    C.pairs_reentered_wl_only += re.pairs_wl_only;

    std::string text = fmt::format(", suspect mask {} DD over {} corner(s) [{}], released {} "
                                   "corner(s)",
                                   S.n_masked, S.pool_suspect.size(),
                                   arSuspectCauseSummary(corners_by), S.pool_released.size());
    if (!S.pool_released.empty())
        text += fmt::format(" ({} DD back: {} fixed, {} WL-only pair(s))", re.dd, re.dd_fixed,
                            re.pairs_wl_only);
    return text;
}

std::string arSuspectCauseSummary(const std::array<long long, kArSuspectCauseCount>& by) {
    std::string s;
    for (int k = 0; k < kArSuspectCauseCount; ++k) {
        const long long n = by[static_cast<std::size_t>(k)];
        if (n == 0) continue;
        s += fmt::format("{}{} {}", s.empty() ? "" : ", ", arSuspectCauseName(k), n);
    }
    return s.empty() ? std::string("none") : s;
}

// =====================================================================
// Run-end report
// =====================================================================

int networkArSuspectRunEnd(const NetworkRunEndView& v) {
    const auto& netar = v.config.getNetworkConfig().ambiguity_resolution;
    if (!netar.enable || netar.suspect_epochs <= 0) return 0;
    const ArSuspectCounters& C = v.ctx.counters.netar_suspect;
    long long n_stamps = 0;
    for (long long n : C.stamps) n_stamps += n;

    v.log->info("Network (producer AR suspect-corner mask, card ARL): window {} search(es).  "
                "Decoupling edge: {} suspect event(s) (distinct corner and stamp), {} of them "
                "met a search while suspect; {} stamp(s) by cause [{}] (one event can carry "
                "several causes).  {} search epoch(s), {} of them masked something; {} of {} "
                "basis DD(s) kept out of the search ({:.2f} %) [{}], at most {} in one epoch "
                "(TOW {:.1f}); {} suspect corner-epoch(s) met in the search pool [{}].",
                netar.suspect_epochs, C.events, C.events_met, n_stamps,
                arSuspectCauseSummary(C.stamps), C.epochs, C.epochs_masked, C.dd_masked,
                C.dd_total,
                C.dd_total > 0 ? 100.0 * static_cast<double>(C.dd_masked)
                                     / static_cast<double>(C.dd_total)
                               : 0.0,
                arSuspectCauseSummary(C.dd_masked_by), C.max_dd_masked, C.max_dd_masked_tow,
                C.corners, arSuspectCauseSummary(C.corners_by));
    v.log->info("Network (producer AR suspect-corner mask, card ARL): release edge "
                "(time-only, nothing re-validated): {} corner(s) released on {} search "
                "epoch(s); {} DD(s) back in the search, {} of them given an integer at that "
                "first search; {} L1/L2 pair(s) back, {} with both singles fixed and {} with "
                "the wide lane alone determined; {} corner(s) still masking or awaiting their "
                "next search at the run end.",
                C.corners_released, C.epochs_released, C.dd_reentered, C.dd_reentered_fixed,
                C.pairs_reentered, C.pairs_reentered_singles, C.pairs_reentered_wl_only,
                v.ctx.carry.ar_suspect.masking.size());
    if (C.epochs == 0)
        v.log->warn("Network (producer AR suspect-corner mask, card ARL): ARMED, but the "
                    "search ran on ZERO epochs, so the mask never met a search (standing "
                    "invariant 4).");
    else if (n_stamps == 0)
        v.log->info("Network (producer AR suspect-corner mask, card ARL): no corner was "
                    "stamped all run: either a clean run or no slip source wrote to the "
                    "ledger (the free-epoch latch needs cycle_slip.relevel_free_epoch).");
    if (C.stamps[static_cast<std::size_t>(causeIndex(kArSuspectRelevel))] > 0)
        v.log->warn("Network (producer AR suspect-corner mask, card ARL): {} re-level "
                    "stamp(s) that no drop and no free-epoch latch explains: a writer of "
                    "LocalCarry::relevel_s the ledger does not know (filter/NetworkArSuspect.hpp).",
                    C.stamps[static_cast<std::size_t>(causeIndex(kArSuspectRelevel))]);
    if (C.dd_reentered_fixed > 0 || C.pairs_reentered_wl_only > 0)
        v.log->info("Network (producer AR suspect-corner mask, card ARL): the release is "
                    "time-only, so the {} integer(s) and {} wide lane(s) given at re-entry "
                    "were not re-validated; the mask is field-blocking as the guard of a "
                    "solve-affecting search.",
                    C.dd_reentered_fixed, C.pairs_reentered_wl_only);
    return 0;
}

} // namespace GnssProcessor
