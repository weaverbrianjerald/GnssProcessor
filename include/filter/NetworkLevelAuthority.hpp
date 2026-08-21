#pragma once
/**
 * @file NetworkLevelAuthority.hpp
 * @brief LV-6 — the internal per-epoch level-authority CENSUS
 *        (`network.level_authority_csgf`; `docs/SPEC_level_authority_internal.md`).
 *
 * Measure-only: reduces THIS epoch's mature CS-GF states (`ctx.carry.iono_cs`,
 * joined against `modeled[i]` rows — never the map alone, which is
 * deliberately unpruned) to one fleet level L_cs(t) in vertical TECU via the
 * two-tier robust reduction (`levelAuthorityReduce`), traces it beside the
 * per-station means (the d_r,GF-class diagnostic), and folds NOTHING.  The
 * fold is a separate future card gated on this census's tracking verdict.
 *
 * Placement: serial, after PASS A's CS-GF advance, BEFORE
 * `networkAssemblyRegisterColumns_`'s SkipEpoch (an empty-system epoch still
 * gets its census row) — and therefore also upstream of every withhold
 * decision: the census measures THROUGH withhold eras
 * (`SPEC_withhold_era_release.md` — instruments upstream of emission survive
 * them).
 *
 * Vertical mapping: exact reuse of `computeIppGeometry` with the SH
 * configuration's shell constants, and the PAIR's own primary-band wavelength
 * (`IonoCsState::lam1_m`) for the TECU conversion — never an assumed L1.
 */

#include "common/PppObservation.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <memory>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/// The LV-6 census (no-op unless `network.level_authority_csgf`).
void networkLevelAuthorityCensus(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    const SyncedEpoch& se,
    const std::vector<NetStation>& stations,
    const std::vector<std::vector<PppObservation>>& modeled,
    const std::vector<bool>& present);

}  // namespace GnssProcessor
