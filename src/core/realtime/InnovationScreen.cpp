/**
 * @file InnovationScreen.cpp
 * @brief Implementation of the consumer's pre-update innovation screen; see
 *        InnovationScreen.hpp for the statistic, the removal order and the episodes.
 */

#include "realtime/InnovationScreen.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace GnssProcessor {

namespace {

/// Index of the histogram bucket holding @p w (an absolute value).
int histBucket(double w) {
    static constexpr double kEdges[InnovationScreen::kHistBuckets - 1] =
        { 2.0, 3.0, 4.0, 5.0, 6.0, 10.0, 100.0 };
    int b = 0;
    while (b < InnovationScreen::kHistBuckets - 1 && w >= kEdges[b]) ++b;
    return b;
}

} // namespace

const char* InnovationScreen::abstainName(Abstain a) {
    switch (a) {
    case Abstain::None:      return "none";
    case Abstain::TooMany:   return "too_many_rows_flagged";
    case Abstain::TooFew:    return "too_few_rows";
    case Abstain::Numerical: return "numerical";
    }
    return "none";
}

const char* InnovationScreen::eventName(EventKind k) {
    switch (k) {
    case EventKind::Exclude:  return "exclude";
    case EventKind::Readmit:  return "readmit";
    case EventKind::Refloat:  return "refloat";
    case EventKind::Lost:     return "lost";
    case EventKind::Released: return "released";
    }
    return "exclude";
}

InnovationScreen::Statistics InnovationScreen::leaveOneOut(const Eigen::MatrixXd& S,
                                                           const Eigen::VectorXd& nu) {
    Statistics st;
    const Eigen::Index m = nu.size();
    if (m == 0 || S.rows() != m || S.cols() != m) return st;
    if (!S.allFinite() || !nu.allFinite()) return st;

    const Eigen::MatrixXd Ssym = 0.5 * (S + S.transpose());
    Eigen::LDLT<Eigen::MatrixXd> ldlt(Ssym);
    if (ldlt.info() != Eigen::Success || !ldlt.isPositive()) return st;

    st.T = ldlt.solve(Eigen::MatrixXd::Identity(m, m));
    st.T = 0.5 * (st.T + st.T.transpose());
    if (!st.T.allFinite()) return st;
    for (Eigen::Index i = 0; i < m; ++i)
        if (!(st.T(i, i) > 0.0)) return st;

    st.g = st.T * nu;
    st.w.resize(m);
    st.sigma.resize(m);
    for (Eigen::Index i = 0; i < m; ++i) {
        const double root = std::sqrt(st.T(i, i));
        st.w(i) = st.g(i) / root;
        st.sigma(i) = 1.0 / root;
    }
    st.ok = true;
    return st;
}

void InnovationScreen::releaseAll(Outcome& out) {
    for (const auto& kv : open_) {
        Event e;
        e.kind = EventKind::Released;
        e.sat = kv.second.sat;
        e.signal = kv.second.signal;
        e.streak = kv.second.streak;
        out.events.push_back(std::move(e));
        ++counters_.released;
    }
    open_.clear();
}

InnovationScreen::Outcome InnovationScreen::evaluate(const Eigen::MatrixXd& S_all,
                                                     const Eigen::VectorXd& nu_all,
                                                     const std::vector<Row>& rows_all) {
    Outcome out;
    out.acted = (cfg_.mode == Mode::Act);

    if (static_cast<size_t>(nu_all.size()) != rows_all.size()
        || S_all.rows() != nu_all.size() || S_all.cols() != nu_all.size()) {
        ++counters_.skipped;
        return out;
    }

    // Only the eligible rows take part. The others are marginalized out of S (dropped
    // with their rows and columns), so a fault in a row the screen may not exclude
    // cannot make an eligible row look wrong.
    std::vector<Eigen::Index> take;
    for (size_t i = 0; i < rows_all.size(); ++i)
        if (rows_all[i].eligible) take.push_back(static_cast<Eigen::Index>(i));
    if (take.empty()) {
        ++counters_.skipped;
        return out;
    }
    const Eigen::Index m = static_cast<Eigen::Index>(take.size());
    std::vector<Row> rows;
    rows.reserve(take.size());
    Eigen::VectorXd nu(m);
    Eigen::MatrixXd S(m, m);
    for (Eigen::Index a = 0; a < m; ++a) {
        const Eigen::Index ia = take[static_cast<size_t>(a)];
        rows.push_back(rows_all[static_cast<size_t>(ia)]);
        nu(a) = nu_all(ia);
        for (Eigen::Index b = 0; b < m; ++b)
            S(a, b) = S_all(ia, take[static_cast<size_t>(b)]);
    }

    ++counters_.epochs;
    counters_.rows += static_cast<uint64_t>(m);

    // Keys present in this epoch, to tell a passing key from a vanished one.
    std::set<std::string> present;
    for (const auto& r : rows) present.insert(r.key());

    // Open episodes whose key has no row this epoch end here, whatever else happens.
    for (auto it = open_.begin(); it != open_.end();) {
        if (present.count(it->first)) { ++it; continue; }
        Event e;
        e.kind = EventKind::Lost;
        e.sat = it->second.sat;
        e.signal = it->second.signal;
        e.streak = it->second.streak;
        out.events.push_back(std::move(e));
        ++counters_.lost;
        it = open_.erase(it);
    }

    if (m < static_cast<Eigen::Index>(std::max(1, cfg_.min_rows))) {
        out.abstain = Abstain::TooFew;
        ++counters_.abstain_few;
        releaseAll(out);
        return out;
    }

    const Statistics full = leaveOneOut(S, nu);
    if (!full.ok) {
        out.abstain = Abstain::Numerical;
        ++counters_.abstain_numeric;
        releaseAll(out);
        return out;
    }

    // Largest |w| over the eligible rows of the full set: the epoch's place in the
    // histogram, whatever the sequential removal then does.
    for (Eigen::Index i = 0; i < m; ++i)
        if (rows[static_cast<size_t>(i)].eligible)
            out.max_abs_w = std::max(out.max_abs_w, std::fabs(full.w(i)));
    ++counters_.hist[static_cast<size_t>(histBucket(out.max_abs_w))];

    // Sequential removal on downdated copies of S^-1 and S^-1 nu.
    Eigen::MatrixXd T = full.T;
    Eigen::VectorXd g = full.g;
    std::vector<char> active(static_cast<size_t>(m), 1);
    Eigen::Index n_active = m;
    const int limit = std::max(1, static_cast<int>(std::floor(cfg_.max_fraction
                                                              * static_cast<double>(m))));
    struct Flag { Eigen::Index row; double w; double sigma; };
    std::vector<Flag> flagged;
    bool abstained = false;

    for (;;) {
        Eigen::Index best = -1;
        double best_w = 0.0;
        for (Eigen::Index i = 0; i < m; ++i) {
            const size_t u = static_cast<size_t>(i);
            if (!active[u] || !rows[u].eligible) continue;
            const double w = std::fabs(g(i)) / std::sqrt(T(i, i));
            if (w > best_w) { best_w = w; best = i; }
        }
        if (best < 0 || best_w <= cfg_.k) break;

        if (static_cast<int>(flagged.size()) >= limit) {
            out.abstain = Abstain::TooMany;
            abstained = true;
            break;
        }
        if (n_active - 1 < static_cast<Eigen::Index>(std::max(1, cfg_.min_rows))) {
            out.abstain = Abstain::TooFew;
            abstained = true;
            break;
        }

        const double tpp = T(best, best);
        if (!(tpp > 0.0) || !std::isfinite(tpp)) {
            out.abstain = Abstain::Numerical;
            abstained = true;
            break;
        }
        flagged.push_back({ best, g(best) / std::sqrt(tpp), 1.0 / std::sqrt(tpp) });

        // Rank-one downdate: remove row/column `best` from S, carrying S^-1 and
        // S^-1 nu along without refactoring.
        const double gp = g(best);
        for (Eigen::Index j = 0; j < m; ++j) {
            if (!active[static_cast<size_t>(j)] || j == best) continue;
            g(j) -= T(j, best) * gp / tpp;
        }
        Eigen::VectorXd col = T.col(best);
        for (Eigen::Index j = 0; j < m; ++j) {
            if (!active[static_cast<size_t>(j)] || j == best) continue;
            for (Eigen::Index k = 0; k < m; ++k) {
                if (!active[static_cast<size_t>(k)] || k == best) continue;
                T(j, k) -= col(j) * col(k) / tpp;
            }
        }
        active[static_cast<size_t>(best)] = 0;
        --n_active;
    }

    if (abstained) {
        out.flagged_before_abstain = static_cast<int>(flagged.size());
        flagged.clear();
        if (out.abstain == Abstain::TooMany)       ++counters_.abstain_many;
        else if (out.abstain == Abstain::TooFew)   ++counters_.abstain_few;
        else                                        ++counters_.abstain_numeric;
        releaseAll(out);
        return out;
    }

    if (!flagged.empty()) ++counters_.epochs_flagged;

    std::set<std::string> flagged_keys;
    for (const auto& f : flagged) {
        const Row& r = rows[static_cast<size_t>(f.row)];
        const std::string key = r.key();
        flagged_keys.insert(key);

        auto it = open_.find(key);
        int streak = 1;
        if (it == open_.end()) {
            ++counters_.episodes;
        } else {
            streak = it->second.streak + 1;
        }
        ++counters_.flagged;
        if (r.phase()) ++counters_.flagged_phase; else ++counters_.flagged_code;
        counters_.max_abs_w = std::max(counters_.max_abs_w, std::fabs(f.w));
        counters_.max_streak = std::max(counters_.max_streak, streak);

        Event e;
        e.sat = r.sat;
        e.signal = r.signal;
        e.w = f.w;
        e.sigma = f.sigma;
        e.innovation = nu(f.row);
        e.streak = streak;

        const bool refloat = r.phase() && streak >= std::max(1, cfg_.refloat_after_ep);
        if (refloat) {
            // A persistent step cannot heal while its ambiguity is held out of the
            // update: re-float it, and let the row back in with the fresh ambiguity.
            e.kind = EventKind::Refloat;
            ++counters_.refloated;
            if (it != open_.end()) open_.erase(it);
            if (out.acted) out.refloat.emplace_back(r.sat, r.signal);
        } else {
            e.kind = EventKind::Exclude;
            Open o;
            o.streak = streak;
            o.sat = r.sat;
            o.signal = r.signal;
            open_[key] = std::move(o);
            if (out.acted) out.exclude.push_back(key);
        }
        out.events.push_back(std::move(e));
    }

    // Open episodes whose key is present and passed: the re-validation edge.
    for (auto it = open_.begin(); it != open_.end();) {
        if (flagged_keys.count(it->first)) { ++it; continue; }
        Event e;
        e.kind = EventKind::Readmit;
        e.sat = it->second.sat;
        e.signal = it->second.signal;
        e.streak = it->second.streak;
        out.events.push_back(std::move(e));
        ++counters_.readmitted;
        it = open_.erase(it);
    }

    return out;
}

} // namespace GnssProcessor
