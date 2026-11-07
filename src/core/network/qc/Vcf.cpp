/**
 * @file Vcf.cpp
 * @brief The per-class variance factor; see Vcf.hpp for the statistics and the classes.
 *
 * Measure-only with network.vcf alone: nothing here writes a weight, a state the solve reads,
 * or a decision.  The EWMAs this file keeps are read by the three stage-2 readers only under
 * network.vcf_apply (@ref networkVcfApplyScale).
 */

#include "network/qc/Vcf.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "config/GnssConfig.hpp"
#include "network/assembly/RowClassify.hpp"   // rowObservationCode
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"           // NetworkEpoch
#include "network/driver/Run.hpp"             // NetworkRun
#include "network/runend/RunEnd.hpp"
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

// ===========================================================================
//  Classes
// ===========================================================================

const char* vcfKindName(VcfKind k) {
    switch (k) {
        case VcfKind::Code:          return "code";
        case VcfKind::Phase:         return "phase";
        case VcfKind::CodeCompanion: return "code_companion";
        case VcfKind::PhaseSecond:   return "phase_second";
    }
    return "?";
}

bool vcfKindOf(ContributionKind k, VcfKind& out) {
    switch (k) {
        case ContributionKind::DataCode:          out = VcfKind::Code;          return true;
        case ContributionKind::DataPhase:         out = VcfKind::Phase;         return true;
        case ContributionKind::DataCodeCompanion: out = VcfKind::CodeCompanion; return true;
        case ContributionKind::DataPhaseSecond:   out = VcfKind::PhaseSecond;   return true;
        default:                                  return false;
    }
}

std::string vcfSignalClassKey(char sys, const std::string& signal) {
    std::string key(1, sys);
    key += ' ';
    key += signal;
    return key;
}

void NetworkVcfEpoch::reset(double new_tow) {
    cls.clear();
    rows_skipped = 0;
    stations = 0;
    fed = false;
    tow = new_tow;
}

// ===========================================================================
//  The feeder
// ===========================================================================

namespace {

/// The class of @p key, created with its identity on first use.
NetworkVcfClassAcc& classAcc(NetworkVcfEpoch& acc, const std::string& key, VcfKind kind,
                             char sys, const std::string& signal) {
    auto it = acc.cls.find(key);
    if (it != acc.cls.end()) return it->second;
    NetworkVcfClassAcc& a = acc.cls[key];
    a.kind = kind;
    a.sys = sys;
    a.signal = signal;
    return a;
}

/// Add one row to one class.
void addRow(NetworkVcfClassAcc& a, double w, double v, double r, bool deweighted) {
    if (deweighted) { ++a.n_dw; return; }
    ++a.n;
    a.sum_wv2 += w * v * v;
    a.sum_r += r;
    if (r > kVcfRedundancyFloor) a.z.push_back(std::abs(v) * std::sqrt(w / r));
    else ++a.n_unchk;
}

}  // namespace

NetworkVcfStationFeed::NetworkVcfStationFeed(NetworkVcfEpoch& acc, double tow) : acc_(acc) {
    if (acc_.tow != tow) acc_.reset(tow);
    acc_.fed = true;
    ++acc_.stations;
}

void NetworkVcfStationFeed::add(const RowTag& tag, double w, double v, double r,
                                bool deweighted) {
    VcfKind kind;
    if (!vcfKindOf(tag.kind, kind)) return;   // not an observation row
    if (!(w > 0.0) || !std::isfinite(w) || !std::isfinite(v) || !std::isfinite(r)) {
        ++acc_.rows_skipped;
        return;
    }
    const double rc = std::clamp(r, 0.0, 1.0);
    // The aggregate of the kind.
    addRow(classAcc(acc_, vcfKindName(kind), kind, '*', std::string()), w, v, rc, deweighted);
    // The per-signal class, for the two nominal kinds only (the counted kinds are one class each).
    if (kind != VcfKind::Code && kind != VcfKind::Phase) return;
    const std::string sig = rowObservationCode(tag);
    if (sig.empty() || !tag.sat.valid()) return;
    const char sys = tag.sat.letter();
    addRow(classAcc(acc_, vcfSignalClassKey(sys, sig), kind, sys, sig), w, v, rc, deweighted);
}

// ===========================================================================
//  Testable pieces
// ===========================================================================

NetworkVcfFactors networkVcfClassFactors(NetworkVcfClassAcc& a) {
    NetworkVcfFactors out;
    out.enough = a.n >= kVcfMinClassRows;
    if (a.n > 0) out.rho = a.sum_r / static_cast<double>(a.n);
    if (a.sum_r > 0.0) {
        out.f = a.sum_wv2 / a.sum_r;
        out.have_f = std::isfinite(out.f);
    }
    if (!a.z.empty()) {
        const std::size_t im = a.z.size() / 2;
        std::nth_element(a.z.begin(), a.z.begin() + static_cast<std::ptrdiff_t>(im), a.z.end());
        const double s = 1.4826 * a.z[im];
        out.g = s * s;
        out.have_g = std::isfinite(out.g);
    }
    out.identifiable = out.enough && out.rho >= kVcfMinRedundancyPerRow;
    return out;
}

double networkVcfEwma(double prev, double x, double alpha, bool seeded) {
    if (!seeded) return x;
    return alpha * x + (1.0 - alpha) * prev;
}

double networkVcfApplyScale(const NetworkVcfState& st, VcfKind kind) {
    if (kind != VcfKind::Code && kind != VcfKind::Phase) return 1.0;
    const auto it = st.classes.find(vcfKindName(kind));
    if (it == st.classes.end()) return 1.0;
    const NetworkVcfClassState& c = it->second;
    if (c.sweeps < kVcfApplyMinSweeps || !c.identifiable_last) return 1.0;
    if (!(c.ewma_g > 0.0) || !std::isfinite(c.ewma_g)) return 1.0;
    return std::sqrt(c.ewma_g);
}

// ===========================================================================
//  The flush
// ===========================================================================

namespace {

/// A number for the CSV, or an empty field when it is unavailable.
std::string num(bool have, double v) {
    return have ? fmt::format("{:.6e}", v) : std::string();
}

}  // namespace

void networkVcfFlush(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (writes are marked `// written`) ----
    NetworkRunContext& ctx = run.ctx;          // written
    NetworkRunTraces& traces = run.traces;     // written
    const SyncedEpoch& se = ep.se;
    // ---- End of the bindings ----
    if (!ctx.knobs.vcf_on) return;   // network.vcf on a live redundancy census
    NetworkVcfState& st = ctx.carry.vcf;
    VcfCounters& q = ctx.counters;
    ++q.vcf_passes;
    NetworkVcfEpoch& acc = st.ep;
    if (acc.tow != se.time.tow || !acc.fed) return;   // not a sweep epoch: a legal state

    ++q.vcf_sweeps;
    q.vcf_rows_skipped += acc.rows_skipped;
    const double t_s = static_cast<double>(se.time.week) * 604800.0 + se.time.tow;
    if (st.first_sweep_s < 0.0) st.first_sweep_s = t_s;
    const int hour = 1 + static_cast<int>(std::floor((t_s - st.first_sweep_s) / 3600.0));
    const double alpha = ctx.knobs.vcf_alpha;

    for (auto& [key, a] : acc.cls) {
        if (a.n == 0 && a.n_dw == 0) continue;
        const NetworkVcfFactors fx = networkVcfClassFactors(a);
        NetworkVcfClassState& cs = st.classes[key];
        cs.kind = a.kind;
        cs.sys = a.sys;
        cs.signal = a.signal;
        cs.rows += a.n;
        cs.rows_dw += a.n_dw;
        if (a.n > 0) ++cs.sweeps_seen;
        // Aggregates count every row once; the per-signal classes would count them twice.
        if (a.sys == '*') {
            q.vcf_rows += a.n;
            q.vcf_rows_dw += a.n_dw;
            q.vcf_rows_unchk += a.n_unchk;
        }
        if (fx.enough && fx.have_f && fx.have_g) {
            const bool seeded = cs.sweeps > 0;
            cs.ewma_f = networkVcfEwma(cs.ewma_f, fx.f, alpha, seeded);
            cs.ewma_g = networkVcfEwma(cs.ewma_g, fx.g, alpha, seeded);
            ++cs.sweeps;
            cs.rho_last = fx.rho;
            cs.identifiable_last = fx.identifiable;
            cs.hour_f[hour].push_back(fx.f);
            cs.hour_g[hour].push_back(fx.g);
            cs.hour_ewma_f[hour] = cs.ewma_f;
            cs.hour_ewma_g[hour] = cs.ewma_g;
        }
        if (traces.vcf_trace.is_open()) {
            const bool in_ewma = cs.sweeps > 0;
            traces.vcf_trace
                << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                << key << ',' << a.sys << ',' << a.signal << ',' << vcfKindName(a.kind) << ','
                << a.n << ',' << fmt::format("{:.6e}", a.sum_wv2) << ','
                << fmt::format("{:.6e}", a.sum_r) << ','
                << num(a.n > 0, fx.rho) << ','
                << num(fx.have_f, fx.f) << ',' << num(fx.have_g, fx.g) << ','
                << num(in_ewma, cs.ewma_f) << ',' << num(in_ewma, cs.ewma_g) << ','
                << (fx.identifiable ? 1 : 0) << ',' << a.n_dw << ',' << a.n_unchk << '\n';
        }
    }
    acc.reset(-1.0);   // consumed; the next feed re-stamps
}

// ===========================================================================
//  Run-end report
// ===========================================================================

namespace {

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const std::size_t im = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(im), v.end());
    return v[im];
}

/// "h1 0.123 h2 0.130 ..." of the hourly medians, as scales (square roots).
std::string hourlyScales(const std::map<int, std::vector<double>>& by_hour) {
    std::string out;
    for (const auto& [h, vals] : by_hour) {
        if (!out.empty()) out += ' ';
        out += fmt::format("h{} {:.3f}", h, std::sqrt(std::max(0.0, medianOf(vals))));
    }
    return out.empty() ? std::string("-") : out;
}

/// Sort rank of a class key: the aggregates first, then the per-signal classes.
int classRank(const NetworkVcfClassState& c) {
    if (c.sys == '*') return static_cast<int>(c.kind);
    return 10 + static_cast<int>(c.kind);
}

}  // namespace

int networkVcfRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    if (!ctx.knobs.vcf_on) return 0;
    const VcfCounters& q = ctx.counters;
    const NetworkVcfState& st = ctx.carry.vcf;

    log->info("Network (VCF): {} flush pass(es), {} of them census sweeps carrying rows; {} "
              "observation row(s) pooled, {} left out for a line-of-sight deweight, {} pooled "
              "at r <= {:g} (in the sums, not in the robust pool), {} dropped for an unusable "
              "weight, {} swept station(s) without a residual.  EWMA alpha {:g} per sweep.  "
              "Factor f = sum w v^2 / sum r, robust g = (1.4826 med |v| sqrt(w/r))^2; the "
              "numbers below are SCALES (square roots), a priori = 1.",
              q.vcf_passes, q.vcf_sweeps, q.vcf_rows, q.vcf_rows_dw, q.vcf_rows_unchk,
              kVcfRedundancyFloor, q.vcf_rows_skipped, q.vcf_no_resid, ctx.knobs.vcf_alpha);
    if (q.vcf_passes == 0)
        log->warn("Network (VCF): ARMED but the flush ran on ZERO epochs: the wiring never "
                  "executed.  Nothing in this run is evidence about the variance factor.");
    else if (q.vcf_rows == 0)
        log->warn("Network (VCF): ARMED, {} pass(es) and ZERO rows pooled: the census sweep "
                  "never fed the rider (check the redundancy census's own run-end line).  A "
                  "miswire, not a calibrated network.", q.vcf_passes);

    std::vector<const std::pair<const std::string, NetworkVcfClassState>*> order;
    for (const auto& kv : st.classes) order.push_back(&kv);
    std::stable_sort(order.begin(), order.end(), [](const auto* a, const auto* b) {
        const int ra = classRank(a->second), rb = classRank(b->second);
        if (ra != rb) return ra < rb;
        return a->first < b->first;
    });
    for (const auto* kv : order) {
        const NetworkVcfClassState& c = kv->second;
        log->info("Network (VCF) [{:<14}] {} sweep(s) in the EWMA of {} seen, {} row(s) ({} "
                  "deweighted); final EWMA scale robust {:.3f} / weighted {:.3f} (factor "
                  "{:.4f} / {:.4f}); rho {:.3f} ({}); hourly robust: {}; hourly weighted: {}",
                  kv->first, c.sweeps, c.sweeps_seen, c.rows, c.rows_dw,
                  std::sqrt(std::max(0.0, c.ewma_g)), std::sqrt(std::max(0.0, c.ewma_f)),
                  c.ewma_g, c.ewma_f, c.rho_last,
                  c.identifiable_last ? "identifiable" : "NOT identifiable",
                  hourlyScales(c.hour_g), hourlyScales(c.hour_f));
    }

    // The stage-2 precondition, stated for the aggregate code class: the hourly median of the
    // robust factor against the EWMA at the end of the same hour, from hour 2.
    const auto it = st.classes.find(vcfKindName(VcfKind::Code));
    if (it != st.classes.end()) {
        const NetworkVcfClassState& c = it->second;
        std::string lad;
        bool all_in = true;
        int n_h = 0;
        for (const auto& [h, vals] : c.hour_g) {
            if (h < 2) continue;
            const auto e = c.hour_ewma_g.find(h);
            if (e == c.hour_ewma_g.end() || !(e->second > 0.0)) continue;
            const double ratio = medianOf(vals) / e->second;
            lad += fmt::format("{}h{} {:.3f}", lad.empty() ? "" : " ", h, ratio);
            ++n_h;
            if (std::abs(ratio - 1.0) > 0.20) all_in = false;
        }
        log->info("Network (VCF): code factor stability (hourly median of g over the EWMA at "
                  "the hour's end, hours >= 2): {} => {}.",
                  lad.empty() ? std::string("no hour >= 2") : lad,
                  n_h == 0 ? "UNTESTED" : (all_in ? "within 20 % every hour"
                                                  : "OUTSIDE 20 % in at least one hour"));
    }
    if (ctx.knobs.vcf_apply)
        log->info("Network (VCF apply): the scales were read by the within-epoch pool on {} "
                  "assessment(s), the post-fit pool on {} pass(es) and the URA EWMA on {} "
                  "update(s) (counts of reads with a scale other than 1, the URA every update); "
                  "final scales: code {:.3f}, phase {:.3f}.",
                  q.vcf_we_applied, q.vcf_pf_applied, q.vcf_ura_applied,
                  networkVcfApplyScale(st, VcfKind::Code), networkVcfApplyScale(st, VcfKind::Phase));
    return 0;
}

}  // namespace GnssProcessor
