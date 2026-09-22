/**
 * @file NetworkLedgerEmit.cpp
 * @brief Implementation of card SID phase 5a: the emitter's per-family
 *        bookkeeping read against the arc ledger, and the producer's declaration
 *        of its signal roster on the wire (@c network.ledger_emit).
 *
 * New code, not a relocation.  Seam contract: free functions over
 * @c (NetworkRunContext&, …) that own NO state; every persistent object is a
 * reference into @c ctx.
 */

#include "filter/NetworkLedgerEmit.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

namespace {

/// One scoped wall-clock accumulator, the pattern the roster seam uses.
class Stopwatch {
public:
    explicit Stopwatch(double& sink)
        : sink_(sink), t0_(std::chrono::steady_clock::now()) {}
    ~Stopwatch() {
        sink_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0_).count();
    }
    Stopwatch(const Stopwatch&) = delete;
    Stopwatch& operator=(const Stopwatch&) = delete;
private:
    double& sink_;
    std::chrono::steady_clock::time_point t0_;
};

/// Order a (signal -> station count) tally into the declaration's preference
/// order: support descending, lexical tie break.  Both halves are stated because
/// a tie broken by map order would make the wire depend on iteration accident.
std::vector<std::string> orderBySupport(const std::map<std::string, long long>& tally) {
    std::vector<std::pair<std::string, long long>> v(tally.begin(), tally.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    std::vector<std::string> out;
    out.reserve(v.size());
    for (auto& [sig, n] : v) { (void)n; out.push_back(sig); }
    return out;
}

/// The one signal an epoch's pins agreed on, or empty when they did not.
std::string modeOrEmpty(const std::map<std::string, long long>& tally) {
    if (tally.size() != 1) return {};
    return tally.begin()->first;
}

}  // namespace

// =====================================================================
// Arming
// =====================================================================

void networkLedgerEmitArm(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log) {
    auto& le = ctx.ledger_emit;
    le.armed = config_.networkLedgerEmit();
    if (!le.armed || !log) return;
    log->info("Network (card SID phase 5a / LEDGER EMIT): ARMED (network.ledger_emit).  "
              "OBSERVATORY: the emitter's per-family bookkeeping is READ against the arc "
              "ledger (the card PBH hold generation against the ledger's admitted arc "
              "births, and the emitter's first-sighting test against the ledger's memory "
              "of the family), and the producer DECLARES its signal roster on the wire as "
              "a block of N records.  Nothing on the producer reads either back, so the "
              "solve and every C / B / P / E record are byte-identical against an unarmed "
              "run; the only difference in products.txt is the N records themselves.");
}

// =====================================================================
// E2: the declaration
// =====================================================================

NetworkSignalDeclaration networkLedgerEmitBuildDeclaration(
        NetworkRunContext& ctx, const std::vector<NetStation>& stations) {
    NetworkSignalDeclaration decl;
    const auto& rs  = ctx.roster;
    auto&       ctr = ctx.counters.le;
    if (!rs.armed) return decl;     // no roster, nothing to declare

    // (constellation, band) -> signal -> stations that model it this epoch.
    std::map<std::pair<std::string, char>, std::map<std::string, long long>> ph, co, comp;
    std::map<std::pair<std::string, char>, std::map<std::string, long long>> dph, dco;
    for (std::size_t i = 0; i < rs.by_station.size(); ++i) {
        const StationRoster& sr = rs.by_station[i];
        const PppModel* model = (i < stations.size()) ? stations[i].model.get() : nullptr;
        for (const auto& [cons, per_band] : sr) {
            for (const auto& [band, cell] : per_band) {
                const auto key = std::make_pair(cons, band);
                ++ctr.decl_cells;
                // The first-epoch fall-back (see the header): a cell the roster
                // cannot answer yet takes the station's own latch, which is the
                // source the roster reproduces one epoch later.
                std::string mp = cell.modelled_phase, mc = cell.modelled_code;
                if (mp.empty() && mc.empty() && model != nullptr) {
                    mp = model->latchedCanonicalPhaseOnBandDigit(cons, band);
                    mc = model->latchedCanonicalCodeOnBandDigit(cons, band);
                    if (!mp.empty() || !mc.empty()) ++ctr.decl_cells_latch;
                }
                if (!mp.empty()) ++ph[key][mp];
                if (!mc.empty()) ++co[key][mc];
                for (const std::string& c : cell.companion_code) ++comp[key][c];
                if (!cell.datum_phase.empty())    ++dph[key][cell.datum_phase];
                if (!cell.datum_code.empty())     ++dco[key][cell.datum_code];
            }
        }
    }
    // The union of every key that had something to say.  Written into the
    // declaration in (constellation, band) order, which std::map gives.
    std::set<std::pair<std::string, char>> keys;
    for (const auto& m : { &ph, &co, &comp, &dph, &dco })
        for (const auto& [k, v] : *m) { (void)v; keys.insert(k); }
    for (const auto& k : keys) {
        SignalDeclarationBand row;
        if (auto it = ph.find(k);   it != ph.end())   row.phase     = orderBySupport(it->second);
        if (auto it = co.find(k);   it != co.end())   row.code      = orderBySupport(it->second);
        if (auto it = comp.find(k); it != comp.end()) row.companion = orderBySupport(it->second);
        // The datum designation is a statement about ONE signal, so it is written
        // only when the epoch's cells agreed; a split designation is left empty
        // rather than collapsed to a majority the S-basis never realized.
        if (auto it = dph.find(k); it != dph.end()) row.datum_phase = modeOrEmpty(it->second);
        if (auto it = dco.find(k); it != dco.end()) row.datum_code  = modeOrEmpty(it->second);
        // A row that names no MODELLED signal says nothing a consumer can act on
        // (a bare datum designation is a statement about a pin, not about a
        // family), so it is dropped rather than written as a half-declaration.
        if (row.phase.empty() && row.code.empty()) continue;
        decl.bands[k.first][k.second] = std::move(row);
    }
    // An EMPTY declaration is the one thing the wire must never carry: it tells a
    // consumer the network models nothing, which is the false statement the
    // loader interlock refuses the knob over.  Reported as its own class.
    if (decl.bands.empty()) {
        ++ctr.decl_empty;
        return NetworkSignalDeclaration{};
    }
    decl.present = true;
    return decl;
}

// =====================================================================
// E1: the capture half (at the emit site, before emitToStore)
// =====================================================================

void networkLedgerEmitCapture(NetworkRunContext& ctx,
                              const NetworkProductEmitter& emitter,
                              const NetworkSolutionEpoch& sol) {
    auto& le = ctx.ledger_emit;
    if (!le.armed) return;
    auto& ctr = ctx.counters.le;
    Stopwatch watch(ctr.capture_ms);

    le.epoch.clear();
    le.epoch_valid = true;
    ++ctr.epochs;

    // One entry per emitted (satellite, signal) family, phase and code together:
    // both baselines answer the same "first sighting" question about the same
    // family, and a reader of the table needs them side by side.
    std::map<std::string, LedgerEmitFamily> by_key;
    for (const auto& s : sol.sats) {
        if (s.sat.empty()) continue;
        for (const auto& [signal, npb] : s.phase_bias) {
            LedgerEmitFamily& f = by_key[s.sat + "|" + signal];
            f.satellite       = s.sat;
            f.signal          = signal;
            f.hold_generation = npb.hold_generation;
            f.has_phase       = true;
            f.phase_first     = emitter.isPhaseFirstSighting(s.sat, signal);
        }
        for (const auto& [signal, bias_m] : s.code_bias_m) {
            (void)bias_m;
            LedgerEmitFamily& f = by_key[s.sat + "|" + signal];
            f.satellite  = s.sat;
            f.signal     = signal;
            f.has_code   = true;
            f.code_first = emitter.isCodeFirstSighting(s.sat, signal);
        }
    }
    le.epoch.reserve(by_key.size());
    for (auto& [k, f] : by_key) { (void)k; le.epoch.push_back(std::move(f)); }
    ctr.families += static_cast<long long>(le.epoch.size());
}

// =====================================================================
// E1: the fold half (after the roster's audit pass advanced the ledger)
// =====================================================================

void networkLedgerEmitFold(NetworkRunContext& ctx, NetworkRunTraces& traces,
                           const SyncedEpoch& se) {
    auto& le = ctx.ledger_emit;
    if (!le.armed || !le.epoch_valid) return;
    auto& ctr = ctx.counters.le;
    Stopwatch watch(ctr.fold_ms);
    le.epoch_valid = false;

    const ArcLedger& led = ctx.arc_ledger;

    // (1) The ledger's arcs, grouped by the family the wire emits.  The wire is
    //     keyed (satellite, signal) and the ledger (station, satellite, signal),
    //     so the family's edge is the UNION over its stations: an arc birth
    //     anywhere in the family is a birth of the family's information.
    struct FamArcs {
        long long births{ 0 };       ///< arcs whose admitted generation advanced at this epoch.
        long long known{ 0 };        ///< arcs the ledger has a record for.
        long long ever_admitted{ 0 };///< of those, with an admitted generation of 1 or more.
        long long first_here{ 0 };   ///< of those, first admitted at THIS epoch.
        long long alive{ 0 };        ///< arcs admitted-alive (Tracking or Reentered) now.
        int       max_gen{ 0 };
    };
    std::map<std::string, FamArcs> fam;
    std::map<ArcLedger::Key, int>  now_gen;
    const long long ep = led.epochIndex();
    for (const auto& [key, rec] : led.arcs()) {
        const std::string fk = std::get<1>(key) + "|" + std::get<2>(key);
        FamArcs& fa = fam[fk];
        ++fa.known;
        if (rec.adm_generation >= 1) ++fa.ever_admitted;
        if (rec.ep_first_admitted == ep) ++fa.first_here;
        if (rec.adm_state == ArcState::Tracking || rec.adm_state == ArcState::Reentered)
            ++fa.alive;
        fa.max_gen = std::max(fa.max_gen, rec.adm_generation);
        now_gen[key] = rec.adm_generation;
        if (!le.prev_adm_gen_valid) continue;
        const auto pit = le.prev_adm_gen.find(key);
        const int  prev = (pit == le.prev_adm_gen.end()) ? 0 : pit->second;
        if (rec.adm_generation > prev) ++fa.births;
    }

    // (2) The comparison, one family at a time.
    const auto event = [&](long long& per_class, const char* name,
                           const std::string& sat, const char* action,
                           const std::string& detail) {
        if (per_class >= kLedgerEmitEventCap) { ++ctr.events_suppressed; return; }
        ++per_class;
        ++ctr.events;
        traces.emitEvent(se.time, "ARC", name, sat, "", std::nullopt, std::nullopt,
                         action, detail);
    };

    std::set<std::string> seen_fam;
    for (const LedgerEmitFamily& f : le.epoch) {
        const std::string fk = f.satellite + "|" + f.signal;
        seen_fam.insert(fk);
        const auto fit = fam.find(fk);
        const FamArcs  fa = (fit == fam.end()) ? FamArcs{} : fit->second;

        // --- E1a: the hold generation against the admitted arc births --------
        if (f.has_phase && f.hold_generation != 0) {
            ++ctr.gen_pop;
            const auto git = le.prev_hold_gen.find(fk);
            const bool gen_changed = (git == le.prev_hold_gen.end())
                                     || (git->second != f.hold_generation);
            // A key's FIRST emission is not a change the consumer can compare
            // against anything, so it is excluded from both classes and reported
            // through the first-sighting half instead.
            const bool comparable = (git != le.prev_hold_gen.end());
            if (fa.known == 0) {
                ++ctr.gen_no_arcs;
            } else if (comparable && gen_changed) {
                ++ctr.gen_changed;
                if (fa.births > 0) {
                    ++ctr.gen_with_birth;
                } else {
                    ++ctr.gen_no_birth;
                    event(le.ev_gen_no_birth, "emit_hold_gen_no_birth", f.satellite, "counted",
                          fmt::format("sig={} hold_gen={}->{} arcs={} admitted={} max_adm_gen={} "
                                      "births=0 (a hold restart with no arc birth behind it)",
                                      f.signal, git->second, f.hold_generation,
                                      fa.known, fa.ever_admitted, fa.max_gen));
                }
            } else if (comparable && !gen_changed && fa.births > 0) {
                ++ctr.birth_no_gen;
                // The SHARP case: the family itself had no admitted-alive arc at
                // the previous epoch, so the wire's family returned.  Everything
                // else is one more STATION joining a family the wire already
                // carries, which is not a re-levelling and is expected.
                const auto ait = le.prev_family_alive.find(fk);
                const bool family_return =
                    (ait == le.prev_family_alive.end() || ait->second == 0);
                if (family_return) ++ctr.birth_no_gen_family;
                event(le.ev_birth_no_gen, "emit_arc_birth_no_gen", f.satellite,
                      family_return ? "family_return" : "station_join",
                      fmt::format("sig={} hold_gen={} arcs={} births={} alive_prev={} "
                                  "max_adm_gen={} (an arc birth the consumer never "
                                  "learns about)",
                                  f.signal, f.hold_generation, fa.known, fa.births,
                                  ait == le.prev_family_alive.end() ? -1 : ait->second,
                                  fa.max_gen));
            }
            le.prev_hold_gen[fk] = f.hold_generation;
        }

        // --- E1b: the emitter's first sighting against the ledger ------------
        if (f.has_phase && f.phase_first) {
            ++ctr.first_phase;
            if (fa.ever_admitted > 0) {
                ++ctr.first_phase_return;
                // The ledger admitted every arc of this family for the first time
                // at THIS epoch, so "return" is the two clocks meeting rather than
                // a re-coupling edge.  Named, because it dominates the first epoch.
                const bool new_here = (fa.first_here == fa.ever_admitted);
                if (new_here) ++ctr.first_phase_return_new;
                event(le.ev_first_return, "emit_first_on_return", f.satellite,
                      new_here ? "phase_new_family" : "phase",
                      fmt::format("sig={} the emitter seeded a FIRST-SIGHTING baseline while "
                                  "the ledger holds {} arc(s) of this family, {} already "
                                  "admitted (max generation {})",
                                  f.signal, fa.known, fa.ever_admitted, fa.max_gen));
            } else {
                ++ctr.first_phase_true;
            }
        }
        if (f.has_code && f.code_first) {
            ++ctr.first_code;
            if (fa.ever_admitted > 0) {
                ++ctr.first_code_return;
                event(le.ev_first_return, "emit_first_on_return", f.satellite, "code",
                      fmt::format("sig={} the emitter seeded a FIRST-SIGHTING baseline while "
                                  "the ledger holds {} arc(s) of this family, {} already "
                                  "admitted (max generation {})",
                                  f.signal, fa.known, fa.ever_admitted, fa.max_gen));
            } else {
                ++ctr.first_code_true;
            }
        }
    }

    le.prev_adm_gen       = std::move(now_gen);
    le.prev_adm_gen_valid = true;
    le.prev_family_alive.clear();
    for (const auto& [fkey, fa] : fam) le.prev_family_alive[fkey] = fa.alive;
    le.epoch.clear();
}

// =====================================================================
// Run end
// =====================================================================

void networkLedgerEmitRunEnd(NetworkRunContext& ctx,
                             const std::shared_ptr<spdlog::logger>& log) {
    const auto& le  = ctx.ledger_emit;
    const auto& c   = ctx.counters.le;
    if (!le.armed || !log) return;

    const auto pct = [](long long n, long long d) {
        return d > 0 ? 100.0 * static_cast<double>(n) / static_cast<double>(d) : 0.0;
    };

    log->info("================ CARD SID PHASE 5a: THE EMITTER AGAINST THE LEDGER, AND THE "
              "DECLARED ROSTER (network.ledger_emit) ================");
    log->info("  capture ran on {} emit epoch(s); {} emitted (satellite, signal) "
              "famil(ies) examined, {:.3f} ms capture + {:.3f} ms fold per epoch",
              c.epochs, c.families,
              c.epochs > 0 ? c.capture_ms / static_cast<double>(c.epochs) : 0.0,
              c.epochs > 0 ? c.fold_ms / static_cast<double>(c.epochs) : 0.0);
    if (c.epochs == 0) {
        log->info("  ARMED AND INERT: no emitted epoch reached the capture, so every class "
                  "below is a zero against a zero denominator (dead wiring, not agreement).");
    }

    log->info("  E1a hold generation against the ledger's admitted arc births:");
    log->info("    families carrying a hold generation : {}  (0 here means "
              "network.emit_pb_hold is OFF, so the class is dead wiring)", c.gen_pop);
    log->info("    of those, no ledger arc at all      : {}  ({:.2f} % — not comparable)",
              c.gen_no_arcs, pct(c.gen_no_arcs, c.gen_pop));
    log->info("    hold generation CHANGED             : {}", c.gen_changed);
    log->info("      with an admitted arc birth        : {}  ({:.2f} % of the changes)",
              c.gen_with_birth, pct(c.gen_with_birth, c.gen_changed));
    log->info("      with NO admitted arc birth        : {}  ({:.2f} % of the changes) — a "
              "hold restart with no arc birth behind it", c.gen_no_birth,
              pct(c.gen_no_birth, c.gen_changed));
    log->info("    admitted arc BIRTH, generation UNCHANGED : {} — a generation the consumer "
              "never learns about; of those the FAMILY itself returned (no admitted-alive "
              "arc at the previous epoch) {} and the remaining {} are one more STATION "
              "joining a family the wire already carries",
              c.birth_no_gen, c.birth_no_gen_family, c.birth_no_gen - c.birth_no_gen_family);

    log->info("  E1b the emitter's first-sighting test against the ledger:");
    log->info("    PHASE first sightings {} -> true first {} / on a RETURN {} ({:.2f} %), of "
              "which {} are families the ledger admitted for the FIRST TIME at the same "
              "epoch (the two clocks meeting, not a re-coupling edge), leaving {} genuine",
              c.first_phase, c.first_phase_true, c.first_phase_return,
              pct(c.first_phase_return, c.first_phase), c.first_phase_return_new,
              c.first_phase_return - c.first_phase_return_new);
    log->info("    CODE  first sightings {} -> true first {} / on a RETURN {} ({:.2f} %).  The "
              "ledger's arcs are PHASE arcs, so a CODE family has no ledger counterpart and "
              "this row reads 'true first' by construction: dead wiring until the ledger "
              "carries code arcs.",
              c.first_code, c.first_code_true, c.first_code_return,
              pct(c.first_code_return, c.first_code));
    log->info("    ARC events written {} ({} suppressed by the per-class cap of {})",
              c.events, c.events_suppressed, kLedgerEmitEventCap);

    log->info("  E2 the declared roster on the wire:");
    log->info("    built on {} epoch(s), CHANGED {} time(s) -> {} N record(s) written, "
              "{:.0f} bytes added to products.txt; {} epoch(s) named no modelled signal at "
              "all and were NOT written",
              c.decl_builds, c.decl_changes, c.decl_records, c.decl_bytes, c.decl_empty);
    log->info("    station cells read {}, of which {} ({:.2f} %) were answered by the station "
              "LATCH because the roster cell had no modelled role yet (the first-epoch "
              "fall-back)",
              c.decl_cells, c.decl_cells_latch, pct(c.decl_cells_latch, c.decl_cells));
    log->info("    current declaration ({} row(s)): {}", c.decl_rows,
              signalDeclarationSummary(le.decl));
    log->info("=========================================================================="
              "=============================");
}

}  // namespace GnssProcessor
