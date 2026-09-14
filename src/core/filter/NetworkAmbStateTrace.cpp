/**
 * @file NetworkAmbStateTrace.cpp
 * @brief RF-1 §7.4 — row formatting and the serial drain for
 *        @c network_amb_state.csv.  @see include/filter/NetworkAmbStateTrace.hpp
 */
#include "filter/NetworkAmbStateTrace.hpp"

#include "app/NetworkRunContext.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

const char* ambStateRouteName(AmbFoldRoute r) {
    switch (r) {
        case AmbFoldRoute::Diag:           return "diag";
        case AmbFoldRoute::Block:          return "block";
        case AmbFoldRoute::CouplingScalar: return "coupling-scalar";
        case AmbFoldRoute::None:
        default:                           return "";
    }
}

std::string ambStateFormatRow(const AmbStateRow& r) {
    // {:.17g} on the graded quantities (the value and the four variances): D1 fits a
    // slope through fold_var over 30 folds at the 1e-4 m² scale and D2 compares
    // value_abs against a 0.3 m bar, so a 6-significant-digit default would quantize
    // exactly the signal the reads are built on.  TOW keeps 3 decimals — it is the join
    // key of every other trace CSV in the run and must match them cell for cell.
    return fmt::format("{},{:.3f},{},{},{},"
                       "{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},"
                       "{},{},{},"
                       "{},{},"
                       "{}\n",
                       r.week, r.tow, r.station, r.sat, r.signal,
                       r.value_abs_m, r.var_m2, r.var_first_m2, r.var_birth_m2,
                       r.fold_var_m2,
                       r.n_folds, r.age_ep, r.age_since_relevel_ep,
                       r.folded ? 1 : 0, ambStateRouteName(r.route),
                       r.event);
}

void ambStateNoteErase(NetworkRunContext& ctx, long long week, double tow,
                       const std::string& station, const std::string& sat,
                       const std::string& signal, const LocalCarry& lc,
                       const char* cause) {
    if (!ctx.knobs.trace_amb_state) return;
    AmbStateRow row;
    row.week = week;
    row.tow  = tow;
    row.station = station;
    row.sat     = sat;
    row.signal  = signal;
    // The entry AS IT STANDS at the moment of deletion: its last harvested value and
    // variance, its birth prior, and how many folds it had earned.  fold_var / folded /
    // route stay at their defaults — an erase row is not a fold, and D3 reads the
    // "variance before" off var_m2, which is the number the next arc has to re-earn.
    row.value_abs_m  = lc.value_abs;
    row.var_m2       = lc.var;
    row.var_first_m2 = lc.var_first;
    row.var_birth_m2 = lc.var_birth;
    row.n_folds      = lc.n_folds;
    row.age_ep = (lc.birth_ep > 0)
        ? (static_cast<long long>(ctx.counters.n_loop_epochs) - lc.birth_ep)
        : lc.birth_ep;
    row.age_since_relevel_ep = -1;   // no epoch clock at this site; the harvest owns it
    row.event = std::string("erase:") + cause;
    ctx.carry.amb_state_rows.push_back(std::move(row));
}

void networkAmbStateTrace(NetworkRunContext& ctx,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces) {
    // --- alias preamble (the seam contract): reconstruct the driver's names -------
    auto& rows     = ctx.carry.amb_state_rows;
    auto& counters = ctx.counters;

    if (rows.empty()) return;
    if (!traces.amb_state_trace.is_open()) {
        // The stream is opened once in openAll() under the same knob, so this can only
        // mean the open failed (already warned there).  Clear anyway: a buffer that
        // grows for the whole run because a file could not be created is a memory leak
        // dressed as a trace.
        if (log) log->warn("Network (RF-1 §7.4): {} buffered carry-state row(s) discarded — "
                           "network_amb_state.csv is not open.", rows.size());
        rows.clear();
        return;
    }
    long long events = 0, erases = 0;
    for (const AmbStateRow& r : rows) {
        traces.amb_state_trace << ambStateFormatRow(r);
        if (!r.event.empty()) {
            ++events;
            if (r.event.rfind("erase:", 0) == 0) ++erases;
        }
    }
    counters.n_amb_state_rows       += static_cast<long long>(rows.size());
    counters.n_amb_state_events     += events;
    counters.n_amb_state_erase_rows += erases;
    ++counters.n_amb_state_epochs;
    rows.clear();
}

}  // namespace GnssProcessor
