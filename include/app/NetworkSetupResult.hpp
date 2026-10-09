#pragma once
/**
 * @file NetworkSetupResult.hpp
 * @brief @internal Handoff aggregate of Application::runNetwork's setup phases
 *        (decomposition INC-5): everything Phases 2-4 and the product sink /
 *        contract / closed-loop-consumer setup produce for the run loop.
 *
 * Application::networkSetup_() fills it in place as an out-parameter.  It is
 * non-copyable and non-movable because three run-long references pin member
 * addresses:
 *   - MultiRinexSync stores a `const BaselineConfig&` to @ref roster,
 *   - the closed-loop RealTimeEngine stores a `GnssConfig&` to
 *     @ref consumer_config and a raw pointer to @ref network_store
 *     (setSsrStore), and
 *   - every NetStation's PppModel holds a reference into @ref sync's parsers
 *     (so @ref stations is declared after @ref sync and destroyed first).
 * The member declaration order below is normative: it encodes the construction
 * prerequisites and, reversed, the destruction-order dependencies.  The run loop
 * binds `auto& X = setup.X;` aliases under the former local names (the INC-3/4
 * migration pattern), so every downstream use site is textually unchanged.
 */

#include "config/GnssConfig.hpp"                    // GnssConfig (consumer_config), BaselineConfig via GnssConfigTypes
#include "rinex/MultiRinexSync.hpp"
#include "filter/NetworkRunTypes.hpp"               // NetStation
#include "filter/NetworkSbasis.hpp"                 // card HLT: SatHealth
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "products/BroadcastEphemerisTypes.hpp"     // BroadcastEphemeris (brdc_rts nav staging)
#include "products/BrdcClockApriori.hpp"            // card NBP native broadcast clock a priori
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"            // NetworkProductWriter, SbasisContract
#include "realtime/RealTimeEngine.hpp"

#include <Eigen/Dense>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

    /// Availability instant of a staged broadcast nav record: toc on the
    /// GPS-continuous scale (a BDS toc in native BDT is shifted to the GPS scale,
    /// following LiveEphemerisStore's toe convention).  One shared definition for
    /// the setup-side staging sort and the epoch-loop feed, so the sort order and
    /// the feed cannot diverge (card 8, replay parity).
    inline double navAvailabilityGpsSeconds(const BroadcastEphemeris& e) {
        return brdcTocGpsSeconds(e);   // one definition, shared with the group-delay latch (card OWD)
    }

    /// @internal See the file banner.  Plain aggregate; members carry the former
    /// runNetwork() local names and initializers verbatim.
    struct NetworkSetupResult {
        NetworkSetupResult() = default;
        NetworkSetupResult(const NetworkSetupResult&) = delete;
        NetworkSetupResult& operator=(const NetworkSetupResult&) = delete;

        /// Phase 2: sync roster (clock-datum station as reference).  sync holds a
        /// reference to this member, so it is declared first and never reassigned
        /// after the sync is constructed.
        BaselineConfig roster;
        /// Phase 2: multi-station ingestion/alignment.  unique_ptr because the
        /// class deletes its assignments (reference members) and its ctor needs
        /// the finished roster.
        std::unique_ptr<MultiRinexSync> sync;
        bool closed_loop_on = false;
        bool brdc_rts = false;
        /// Staged broadcast nav records and the progressive feed cursor (brdc_rts
        /// only; empty otherwise).  The epoch loop's LiveEphemerisStore catch-up feed
        /// advances the cursor.  (Card 8)
        std::vector<BroadcastEphemeris> brdc_nav_staged;
        std::size_t brdc_nav_cursor = 0;
        /// Card NBP (@c network.apriori_source @c sp3_brdc_clock): the native broadcast
        /// clock a priori (selector, offset table from 00:00 of the observation day,
        /// transmission-time store feed, wire stamp), built in Phase 3 from the navigation
        /// file's GPS records.  Every producer station's model reads its a priori clock
        /// from it; the epoch loop feeds the store from it; the emit seam stamps the wire
        /// with it.  Null in every other mode.
        std::unique_ptr<BrdcClockApriori> brdc_apriori;
        /// Per-satellite geometry-free pin target in metres, `c * T_GD` from the
        /// navigation message, for `network.sat_gf_datum: "broadcast_tgd"` (RF-0c).
        /// Latched for the run: read once at setup and never updated, because a
        /// datum that changed mid-run would step the emitted code biases and the
        /// estimable ionosphere together with nothing on the wire declaring it
        /// (the decoupling edge of standing invariant #7, with no response wired).
        /// Empty in every other mode, so the row builder falls back to a zero
        /// target instead of guessing.
        std::map<std::string, double> sat_gf_target_m;
        /// The run's processing window [window_t0_s, window_t1_s) in continuous GPS
        /// seconds, computed once right after the multi-station sync with the epoch
        /// loop's own bounds (the first synced epoch plus @c network.start_offset_hours;
        /// the start plus @c processing.max_duration_s, else 1 s past the last synced
        /// epoch).  Read by the group-delay latch (the records in force during the
        /// run) and by the S contract's a-priori identity (the product files in force
        /// during the run).  (Card OWD)
        double window_t0_s = 0.0;
        double window_t1_s = 0.0;
        /// Card HLT (@c network.sat_health): every satellite's broadcast health over time,
        /// built at setup from the navigation file.  Read by the synced-epoch door
        /// ("exclude") and by the clock-pivot election ("pivot").  Empty when the knob is off.
        NetworkSbasis::SatHealth sat_health;
        /// Card HLT, "exclude": the observations the door removed inside the processing
        /// window, per satellite.  (The door also clears the synced epochs outside the
        /// window, which no reader processes.)
        std::map<std::string, long long> sat_health_removed;
        /// Phase 3b: the co-estimated SH-VTEC field model (estimate_iono_sh), or
        /// null (external-GIM / no-iono paths).
        std::unique_ptr<SphericalHarmonicIonosphere> sh_model;
        /// The dual-layer field's second-shell basis (layer2_max_degree > 0), or
        /// null (single-layer path).  Non-null only when sh_model is non-null.
        /// (Iono-2 mechanism A)
        std::unique_ptr<SphericalHarmonicIonosphere> sh_model2;
        /// Phase 4: per-station Kalman resources, pivot at index 0.  Elements
        /// hold references into sync's parsers, so this is declared after sync.
        std::vector<NetStation> stations;
        // Per-station compute parallelism knobs (seam_on / hw_max_threads are
        // setup-internal and not exported).
        int  n_sta_total  = 0;
        bool parallel_on  = false;
        int  n_threads    = 1;
        int  seam_threads = 1;
        bool carry_par    = false;
        /// Owned product sink, kept separate from the SsrCorrectionStore singleton
        /// for closed-loop isolation.  The type is non-movable (mutex), and the
        /// consumer retains &network_store for the whole run; this member is why the
        /// struct itself is non-movable.
        SsrCorrectionStore network_store;
        /// Product emitter.  Its ctor takes the disc-threshold and channel toggles,
        /// so it is an optional emplaced at the former construction point.  It is
        /// handed off rather than re-derived because the RTB-7 warm resume mutates
        /// its counters.
        std::optional<NetworkProductEmitter> emitter;
        /// Optional on-disk product serialization + deferred v2 S-basis contract.
        std::unique_ptr<NetworkProductWriter> product_writer;
        std::filesystem::path prod_path;
        SbasisContract pending_contract;
        bool           contract_pending = false;
        /// H heartbeats for withheld epochs that precede the deferred writer's open
        /// (RTA-10 gap fix).  Without them, a leg that opens inside a withhold era
        /// writes nothing for the whole era, which leaves "withheld" and "producer
        /// stopped" indistinguishable, the ambiguity the H line exists to resolve.
        /// The three withhold sites (rank gate, A4 σ̂0 ceiling, SH health gate)
        /// append here while product_writer is null and a deferred v2 open is
        /// pending (contract_pending); the buffer is flushed in tow order right after
        /// the writer opens at the first emit.  A run that never emits logs a warning
        /// with the count at teardown (the v2 contract header cannot be stamped
        /// without a realized datum, so no file is created for it).
        std::vector<NetworkProducerStatus> pending_h_pre_open;
        /// Closed-loop consumer: the engine stores a GnssConfig& to
        /// consumer_config (address-pinned), so consumer_config is declared before
        /// consumer and outlives it.
        GnssConfig consumer_config;
        std::unique_ptr<RealTimeEngine> consumer;
        /// Closed-loop truth, reduced into the frame the consumer estimates.
        /// `network.closed_loop.station.position_ecef` is a published coordinate
        /// (IGS SSC / SIF / surveyed RINEX header) and therefore
        /// monument-referenced, whereas the consumer's position state is the ARP.
        /// Grading one against the other puts a constant vertical bias equal to the
        /// station eccentricity into `up_m`.  Resolved once here (the held-out
        /// station's RINEX header is loaded by `sync`); every consumer-error site
        /// reads this instead of re-deriving it from config.  Equals the configured
        /// value exactly when the header eccentricity is zero.
        Eigen::Vector3d cl_truth_arp{ Eigen::Vector3d::Zero() };
        /// The `ANTENNA: DELTA H/E/N` used for @ref cl_truth_arp (zero when none).
        Eigen::Vector3d cl_truth_hen{ Eigen::Vector3d::Zero() };

        /// @name Marker-to-antenna-reference-point roster census (card UPO)
        /// Filled at station build whether @c network.station_marker_to_arp is on
        /// or off, so an off run reports the size of the class it does not correct
        /// (standing invariant #4).  Read by the run-end readout and the run
        /// manifest.
        /// @{
        /// Loaded roster stations whose header carries a nonzero eccentricity.
        int n_marker_arp_ecc       = 0;
        /// Of those, the ones reduced to the ARP (0 with the knob off).
        int n_marker_arp_reduced   = 0;
        /// Of those, the ones with |H|, |E| or |N| above 0.01 m.
        int n_marker_arp_over_1cm  = 0;
        /// Loaded roster stations whose header carries no eccentricity record at
        /// all; left at the configured coordinate in either state, and warned.
        int n_marker_arp_no_record = 0;
        /// Largest |H| on the roster, signed (0 when no station carries one).
        double marker_arp_max_h    = 0.0;
        /// The station @ref marker_arp_max_h belongs to (empty when none).
        std::string marker_arp_max_h_station;
        /// @}
    };

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the marker to ARP roster reduction (UPO).
 *
 * The marker to antenna-reference-point roster reduction (card UPO), in both knob
 * states.
 *
 * Gate: returns 0 at once unless `ctx.counters.n_marker_arp_ecc > 0 ||
 * ctx.counters.n_marker_arp_no_record > 0`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkMarkerArpRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
