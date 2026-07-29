#pragma once
/**
 * @file NetworkSetupResult.hpp
 * @brief @internal Handoff aggregate of Application::runNetwork's setup phases
 *        (decomposition INC-5) — everything Phases 2-4 + the product sink /
 *        contract / closed-loop-consumer setup produce that the run loop
 *        consumes.
 *
 * Filled IN PLACE by Application::networkSetup_() as an out-parameter and
 * deliberately NON-COPYABLE / NON-MOVABLE: three run-long references pin
 * member addresses —
 *   - MultiRinexSync stores a `const BaselineConfig&` to @ref roster,
 *   - the closed-loop RealTimeEngine stores a `GnssConfig&` to
 *     @ref consumer_config and a raw pointer to @ref network_store
 *     (setSsrStore), and
 *   - every NetStation's PppModel holds a reference into @ref sync's parsers
 *     (so @ref stations is declared AFTER @ref sync: destroyed first).
 * Member DECLARATION ORDER below is normative — it encodes both construction
 * prerequisites and (reversed) the destruction-order web.  The run loop binds
 * `auto& X = setup.X;` aliases under the former local names (the INC-3/4
 * migration mechanic), so every downstream use-site is textually unchanged.
 */

#include "config/GnssConfig.hpp"                    // GnssConfig (consumer_config), BaselineConfig via GnssConfigTypes
#include "rinex/MultiRinexSync.hpp"
#include "filter/NetworkRunTypes.hpp"               // NetStation
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "products/BroadcastEphemerisTypes.hpp"     // BroadcastEphemeris (brdc_rts nav staging)
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"            // NetworkProductWriter, SbasisContract
#include "realtime/RealTimeEngine.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace GnssProcessor {

    /// Card 8 (replay parity): availability instant of a staged broadcast nav
    /// record = GPS-continuous-scale toc (BDS toc is native BDT → shifted to the
    /// GPS scale, mirroring LiveEphemerisStore's toe convention).  ONE shared
    /// definition for the setup-side staging sort and the epoch-loop feed —
    /// duplicating it risks sort-vs-feed drift (formerly a runNetwork lambda).
    inline double navAvailabilityGpsSeconds(const BroadcastEphemeris& e) {
        double c = static_cast<double>(e.kepler.toc.week) * 604800.0 + e.kepler.toc.tow;
        if (e.system == System::BeiDou) c += 1356.0 * 604800.0 + 14.0;   // BDT week 0 = GPS week 1356, BDT = GPS − 14 s
        return c;
    }

    /// @internal See the file banner.  Plain aggregate; members carry the former
    /// runNetwork() local names and initializers verbatim.
    struct NetworkSetupResult {
        NetworkSetupResult() = default;
        NetworkSetupResult(const NetworkSetupResult&) = delete;
        NetworkSetupResult& operator=(const NetworkSetupResult&) = delete;

        /// Phase 2: sync roster (clock-datum station as reference).  sync holds a
        /// reference to this member — declared first, never reassigned after the
        /// sync construction.
        BaselineConfig roster;
        /// Phase 2: multi-station ingestion/alignment.  unique_ptr because the
        /// class deletes its assignments (reference members) and its ctor needs
        /// the finished roster.
        std::unique_ptr<MultiRinexSync> sync;
        bool closed_loop_on = false;
        bool brdc_rts = false;
        /// Card 8: staged broadcast nav records + progressive feed cursor
        /// (brdc_rts only; empty otherwise).  The cursor is MUTATED by the epoch
        /// loop's LiveEphemerisStore catch-up feed.
        std::vector<BroadcastEphemeris> brdc_nav_staged;
        std::size_t brdc_nav_cursor = 0;
        /// Phase 3b: the co-estimated SH-VTEC field model (estimate_iono_sh), or
        /// null (external-GIM / no-iono paths).
        std::unique_ptr<SphericalHarmonicIonosphere> sh_model;
        /// Iono-2 mechanism A: the dual-layer field's second-shell basis
        /// (layer2_max_degree > 0), or null (single-layer legacy path).  Only
        /// ever non-null when sh_model is non-null.
        std::unique_ptr<SphericalHarmonicIonosphere> sh_model2;
        /// Phase 4: per-station Kalman resources, pivot at index 0.  Elements
        /// hold references into sync's parsers — declared after sync.
        std::vector<NetStation> stations;
        // Per-station compute parallelism knobs (seam_on / hw_max_threads are
        // setup-internal and deliberately not exported).
        int  n_sta_total  = 0;
        bool parallel_on  = false;
        int  n_threads    = 1;
        int  seam_threads = 1;
        bool carry_par    = false;
        /// Owned product sink (closed-loop isolation — deliberately NOT the
        /// SsrCorrectionStore singleton).  Non-movable type (mutex); the consumer
        /// retains &network_store for the whole run — this member is why the
        /// struct itself is non-movable.
        SsrCorrectionStore network_store;
        /// Product emitter (ctor takes the disc-threshold + channel toggles →
        /// optional, emplaced at the former construction point).  Handed off, not
        /// re-derived: the RTB-7 warm resume mutates its counters.
        std::optional<NetworkProductEmitter> emitter;
        /// Optional on-disk product serialization + deferred v2 S-basis contract.
        std::unique_ptr<NetworkProductWriter> product_writer;
        std::filesystem::path prod_path;
        SbasisContract pending_contract;
        bool           contract_pending = false;
        /// Closed-loop consumer: the engine stores a GnssConfig& to
        /// consumer_config (address-pinned) — declared before consumer, which is
        /// destroyed first.
        GnssConfig consumer_config;
        std::unique_ptr<RealTimeEngine> consumer;
    };

} // namespace GnssProcessor
