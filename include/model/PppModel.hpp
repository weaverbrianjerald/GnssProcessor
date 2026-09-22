#pragma once


/**
 * @file PppModel.hpp
 * @brief PPP/PPP-RTK measurement modelling and correction utilities.
 *
 * Bundles the geometry, atmospheric models and GNSS corrections needed to form
 * modeled code and carrier-phase observations for the estimator, over config,
 * product data, a RINEX parser and the StateManager.
 */

#include "config/GnssConfig.hpp"
#include "products/ProductParser.hpp"
#include "logging/LogManager.hpp"
#include "rinex/RinexParser.hpp"
#include "filter/StateManager.hpp"
#include "common/PppTypes.hpp"
#include "model/Planetary.hpp"
#include "atmosphere/Ionosphere.hpp"
#include "atmosphere/GlobalIonosphereMap.hpp"
#include "atmosphere/HigherOrderIonosphere.hpp"
#include "model/Antenna.hpp"
#include "model/ModelGeometry.hpp"
#include "model/StochasticModel.hpp"
#include <Eigen/Dense>
#include <filesystem>
#include <optional>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <deque>

namespace GnssProcessor {

    /**
     * @brief Every output of one Model::Geometry::computeSatelliteGeometry() call.
     *
     * Keeps 15+ separate references out of the pipeline and makes the
     * computeModels() / observation-helper interface self-documenting.
     */
    struct SatGeometry {
        Eigen::Vector3d sat_pos  = Eigen::Vector3d::Zero();
        Eigen::Vector3d sat_vel  = Eigen::Vector3d::Zero();
        Eigen::Vector3d rx_pos   = Eigen::Vector3d::Zero();
        Eigen::Vector3d los      = Eigen::Vector3d::Zero();
        double elevation         = 0.0;
        double azimuth           = 0.0;
        double range             = 0.0;
        double tropo_zhd         = 0.0;
        double sat_clock         = 0.0;
        double shapiro_delay     = 0.0;
        double sagnac_corr       = 0.0;
        double otl_corr          = 0.0;   ///< Ocean tidal loading site displacement projected onto -LOS (m).
        double apl_corr          = 0.0;   ///< Atmospheric pressure loading (S1/S2 thermal tide) projected onto -LOS (m).
        double relativistic_corr = 0.0;
        double solid_tide        = 0.0;
        double mf_w              = 0.0;
        double mf_d              = 0.0;
        bool   broadcast_only    = false;   ///< Clock is broadcast-grade (metre level) → code-only (phase dropped).
    };

    /**
     * @class PppModel
     * @brief Computes corrected observations and delays for PPP.
     *
     * Combines geometry and correction models (orbits/clocks, tides, loading),
     * applies troposphere and ionosphere models with their iono-free
     * combinations, and provides GLONASS rescaling and signal-selection helpers.
     * Callers supply RINEX observations plus the current PPP state and get back
     * per-observation modeled values and diagnostics.
     */
    class PppModel {
    public:
        /**
         * @brief Construct the PppModel.
         *
         * @param config Read-only configuration providing defaults and parameters.
         * @param product_parser Parser/loader for external products (SP3, CLK, ERP, etc.).
         * @param log_manager Reference to the logging manager used for diagnostics.
         * @param rinex_parser Reference to the RINEX parser providing observation metadata.
         * @param state_manager Reference to StateManager for accessing/adding ambiguities and state values.
         */
        PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
            const RinexParser& rinex_parser, StateManager& state_manager);

        /**
         * @brief Seed a fresh PppState from configuration defaults.
         *
         * Sets position, clock, troposphere and ionosphere entries, and the
         * ambiguity container where required.
         *
         * @param state Output PppState to initialize.
         */
        void initializeState(PppState& state);

        /**
         * @brief Compute modeled observations for a set of RINEX observations.
         *
         * Computes satellite geometry and applies clock, orbit, bias,
         * troposphere and ionosphere corrections, filling @p modeled_obs with
         * observables ready for residual formation.
         *
         * @param observations RINEX observation records for the epoch.
         * @param state Current PPP state (not modified).
         * @param modeled_obs Output, one entry per processed observable.
         * @param drop_reasons Optional map naming why each dropped satellite
         *        failed (BelowMask / NoProduct / NoModel), so callers such as the
         *        satellite-status trace can distinguish failure modes instead of
         *        one undifferentiated bucket.  nullptr disables tracking.
         * @return true if at least one observation succeeded.
         */
        bool computeModels(const std::vector<RinexObservation>& observations,
                           PppState& state,
                           std::vector<PppObservation>& modeled_obs,
                           std::map<std::string, DropReason>* drop_reasons = nullptr);

        /**
         * @brief Compute ionospheric delay for a single observation path.
         *
         * Uses the internal Ionosphere model, or the Klobuchar fallback.
         *
         * @param time GNSS time of the observation.
         * @param rx_pos Receiver ECEF position (m).
         * @param sat_pos Satellite ECEF position (m).
         * @param freq_type Frequency or signal identifier (e.g. "L1", "C1C").
         * @param system GNSS system.
         * @param measurements Observed code/phase values, if the model needs them.
         * @return Slant ionospheric delay along the ray path (m).
         */
        double computeIonosphericDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, const std::string& freq_type,
            System system, const std::map<std::string, double>& measurements) const;

        /**
         * @brief Rescale GLONASS observation values to a common frequency reference.
         *
         * GLONASS is FDMA, so raw measurements are rescaled to the nominal
         * frequency reference the rest of the chain expects.
         *
         * @param observed_data Raw observed value (m).
         * @param freq_type Frequency identifier (e.g. "L1", "L2").
         * @param system GNSS system; must be GLONASS to rescale.
         * @param satellite Satellite identifier (e.g. "R01").
         * @return Rescaled measurement (m).
         */
        double rescaleGlonassObservation(double observed_data, const std::string& freq_type,
            System system, const std::string& satellite) const;

        /**
         * @brief Access the RINEX parser used by this model.
         * @return Const reference to the configured RinexParser.
         */
        const RinexParser& getRinexParser() const;

        /**
         * @brief Compute ionosphere-free (LC) combination from two signals.
         *
         * The standard iono-free linear combination of two frequencies
         * (e.g. L1/L2 or C1C/C2W).
         *
         * @param measurements Measurement name -> value (m).
         * @param system GNSS system of the satellite.
         * @param signal1 First signal identifier, a key in @p measurements.
         * @param signal2 Second signal identifier, a key in @p measurements.
         * @return Combined observable (m).
         */
        double computeIonoFreeCombination(const std::map<std::string, double>& measurements,
            System system, const std::string& signal1, const std::string& signal2) const;

        /**
         * @brief Get read-only access to the internal geometry helper.
         * @return Const reference to the Geometry instance used for satellite/receiver geometry computations.
         */
        const Model::Geometry& getGeometry() const { return geometry_; }

        /**
         * @brief Get mutable access to the internal geometry helper.
         * @return Reference to the Geometry instance.
         */
        Model::Geometry& getGeometry() { return geometry_; }

        /**
         * @brief Enable the phase wind-up diagnostic CSV trace.
         *
         * Call once at startup when logging.enable_windup_trace is set.
         *
         * @param log_dir Directory where windup_trace.csv will be created.
         * @return True if the trace file opened successfully.
         */
        bool enableWindUpTracing(const std::filesystem::path& log_dir) {
            return antenna_.enableWindUpTracing(log_dir);
        }

        /// @brief Arm the per-receiver phase wind-up census.  Telemetry only —
        ///        it cannot move a byte of the solve.
        void enableWindUpCensus(double wu_gap_s, double carry_gap_s) {
            antenna_.enableWindUpCensus(wu_gap_s, carry_gap_s);
        }
        /// @brief The census counters for this model's receiver.
        const GnssCorrections::WindUpCensus& windUpCensus() const {
            return antenna_.windUpCensus();
        }
        /// @brief The accumulator-restart ledger.
        const std::map<std::string, GnssCorrections::WuRestart>& wuRestarts() const {
            return antenna_.wuRestarts();
        }
        /// @brief Clear the restart ledger (the drain's consume edge).
        void clearWuRestarts() { antenna_.clearWuRestarts(); }

        /**
         * @brief Whether the BIA product covers this satellite+signal pair.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    RINEX observation code (e.g. "C7Q").
         * @return True if BIA coverage exists.
         */
        bool hasBiasEntry(const std::string& satellite, const std::string& signal) const {
            return gnss_corrections_.hasBiasEntry(satellite, signal);
        }

        /**
         * @brief Get read-only access to the internal GnssCorrections instance.
         * @return Const reference to the corrections helper.
         */
        const GnssCorrections& getCorrections() const { return gnss_corrections_; }

        /**
         * @brief Update the GLONASS FDMA frequency-slot map (satellite → channel k).
         *
         * The real-time counterpart of the constructor's one-shot seed from the
         * RINEX "GLONASS SLOT / FRQ #" header, for slots arriving incrementally
         * from the broadcast stream (RTCM 1020 DF039 / SBF GLONav).  Lets
         * per-satellite wavelengths resolve from live assignments rather than the
         * hard-coded default table.  Idempotent.
         *
         * @param slots GLONASS satellite id ("R01") -> FDMA channel k (-7..+6).
         */
        void setGlonassSlots(const std::map<std::string, int>& slots) {
            ppp_utils_.setGlonassSlots(slots);
        }

        /**
         * @brief The model's own @ref PppUtils — the SEEDED one.
         *
         * Card ALC needs carrier wavelengths in a seam that holds only the
         * engine, and a locally constructed @c PppUtils would carry the built-in
         * default GLONASS slot table rather than the one @ref setGlonassSlots
         * pushed from the live ephemeris store.  Handing out the model's own
         * instance is the only way a caller gets the frequencies the MEASUREMENT
         * MODEL actually used.  Read-only: the slot map is still written through
         * @ref setGlonassSlots alone.
         *
         * @return The model's utility helper (frequencies, wavelengths, system ids).
         */
        const PppUtils& pppUtils() const { return ppp_utils_; }

        /**
         * @brief GLONASS FDMA channel k for a satellite, from the SEEDED slot map.
         *
         * Reads the map @ref setGlonassSlots populated, so callers holding only a
         * const PppModel& (e.g. StateInitializer) get the true current channel
         * instead of their own un-seeded default table.
         *
         * @param satellite GLONASS satellite id ("R07").
         * @return Channel k in [-7,+6], or -999 if the slot is unknown.
         */
        int getGlonassChannel(const std::string& satellite) const {
            return ppp_utils_.getGlonassChannel(satellite);
        }

        /**
         * @brief Whether the receiver antenna has ATX coverage for a signal.
         *
         * @param signal RINEX observation code (e.g. "L5Q").
         * @param system GNSS system of the observation.
         * @return True if the antenna model has PCO data for this signal.
         */
        bool hasReceiverAtxEntry(const std::string& signal, System system) const {
            return antenna_.hasReceiverAtxEntry(signal, system);
        }

        /**
         * @brief Whether the RINEX receiver antenna type matched an ATX model entry.
         *
         * False means every receiver PCO/PCV correction is silently zero for this
         * station's carrier phase.  The network driver warns at startup and can
         * withhold the station from phase-bias emission
         * (network.phase_bias_exclude_unmatched_antenna).
         *
         * @return True iff an ATX entry matches the receiver antenna type.
         */
        bool hasReceiverAntennaMatch() const {
            return antenna_.hasReceiverAntennaMatch();
        }

        /**
         * @brief Whether the receiver antenna resolved only via the always-on
         *        type-preserving radome→NONE fallback (Tier 1).
         *
         * True means the PCO/PCV comes from the radome-agnostic "NONE"
         * calibration of the same physical antenna because the specific radome
         * code is uncalibrated — standard IGS/EPN practice, mm-level.  The
         * network driver reports it as a startup diagnostic.
         *
         * @param out_detail [out,optional] Human-readable substitution detail.
         * @return True iff the antenna matched only through radome→NONE.
         */
        bool receiverAntennaViaRadomeFallback(std::string* out_detail = nullptr) const {
            return antenna_.receiverAntennaViaRadomeFallback(out_detail);
        }

        /**
         * @brief Check whether both satellite and receiver ATX entries
         *        are fully populated for a (sat, signal) pair.
         *
         * The AR pipeline uses this to gate WL/NL constraints on signals lacking
         * strict ATX coverage when require_atx_coverage_for_ar is set.
         *
         * @param satellite Satellite identifier (e.g. "G05").
         * @param signal    RINEX-3 observation code (e.g. "L5Q").
         * @param system    GNSS system.
         * @return True iff both sat and rx have finite PCO and PCV.
         */
        bool hasFullAtxCoverage(const std::string& satellite,
                                const std::string& signal,
                                System system) const {
            return antenna_.hasFullAtxCoverage(satellite, signal, system);
        }

        /**
         * @brief Whether a (sat, signal) ATX correction comes from the
         *        frequency-fallback chain rather than exact entries on both sides.
         *
         * Read by the AR gate (restrict_ar_for_substituted_atx) and the
         * per-ambiguity process-noise loosening (loosen_amb_q_for_substituted_atx).
         */
        bool wasAtxSubstituted(const std::string& satellite,
                                const std::string& signal,
                                System system) const {
            return antenna_.wasAtxSubstituted(satellite, signal, system);
        }

        /**
         * @brief Access the parsed ATX antenna entries.
         * @return Const reference to the antenna data vector.
         */
        const std::vector<AntennaData>& getAntennas() const {
            return antenna_.getAntennas();
        }

        /**
         * @brief Inject the GIM used for the higher-order ionosphere correction.
         *
         * Non-owning; the GlobalIonosphereMap lifetime belongs to Application or
         * the baseline / real-time drivers.  nullptr disables the correction, so
         * computeHigherOrderIonoCorrection returns 0 m whatever the config says.
         * Idempotent, and inert unless
         * model_corrections.enable_higher_order_ionosphere is true.
         *
         * @param gim Loaded GIM, or nullptr.
         */
        void setGim(const GlobalIonosphereMap* gim) { gim_ = gim; }

        /**
         * @brief Inject a non-owning SSR-correction store override (DI seam for
         *        the PPP-RTK closed-loop harness).
         *
         * Routes both the orbit and clock overlays, via @ref setSsrOrbitStore and
         * @ref setSsrClockStore.  nullptr (the default) uses the process-wide
         * singleton, so every non-harness caller is byte-identical.  The network
         * closed-loop consumer points this at the producer's OWNED store, letting
         * a held-out station consume freshly-emitted satellite clocks without
         * polluting global state.
         *
         * @param store Non-owning store pointer (must outlive this model), or
         *        nullptr for the singleton.
         */
        void setSsrStore(const SsrCorrectionStore* store) {
            setSsrOrbitStore(store);
            setSsrClockStore(store);
        }

        /**
         * @brief Route ONLY the SSR ORBIT overlay at @p store.
         *
         * Hits the RAC-delta application in Geometry::getSatellitePV; the clock
         * path keeps its current routing.  This is how the standalone
         * network-consumer wiring keeps orbits on the external RTS stream
         * (singleton) while clocks come from the injected network-product store
         * (processing.network_consumer.orbit_source = "external").
         *
         * @param store Non-owning store pointer (must outlive this model), or
         *        nullptr for the singleton.
         */
        void setSsrOrbitStore(const SsrCorrectionStore* store) {
            geometry_.setSsrOrbitStore(store);
            // The model's own GnssCorrections applies clocks too, so its
            // orbit-coherence echo check must read the same store as the geometry
            // orbit overlay.  Geometry forwards to its own internal corrections.
            gnss_corrections_.setOrbitEchoStore(store);
        }

        /**
         * @brief Route ONLY the SSR CLOCK overlay at @p store.
         *
         * Covers both satellite-clock consumers behind this model: geometry's
         * internal GnssCorrections (emission-time refinement) and the model's own
         * (the per-observation clock in computeModels).
         *
         * @param store Non-owning store pointer (must outlive this model), or
         *        nullptr for the singleton.
         */
        void setSsrClockStore(const SsrCorrectionStore* store) {
            geometry_.setSsrClockStore(store);
            gnss_corrections_.setSsrStore(store);
        }

        /**
         * @brief Keep carrier phase on broadcast-grade satellites instead of
         *        gating them to code-only.
         *
         * computeModels normally drops every phase observable for a satellite
         * whose range model is metre-level (broadcast clock, no precise CLK, no
         * SSR clock — the broadcast_only flag): at mm phase precision the float
         * ambiguity is inseparable from the clock error, so phase cannot help a
         * single-receiver PPP user.  The brdc_rts NETWORK PRODUCER is the
         * exception — it ESTIMATES the satellite clock (per-sat state Δδtˢ against
         * the broadcast a-priori), so its stations' phase carries exactly the
         * information the estimator needs.
         *
         * ApplicationNetwork sets this on PRODUCER station models only, when
         * network.apriori_source == brdc_rts.  It MUST NOT be set on the
         * closed-loop CONSUMER: its broadcast-grade sats genuinely cannot use
         * phase, and sats that did get a network clock are not broadcast_only.
         *
         * @param suppressed true keeps phase; false (default) restores the
         *        code-only gate, byte-identical.
         */
        void setBroadcastPhaseGateSuppressed(bool suppressed) {
            broadcast_phase_gate_suppressed_ = suppressed;
        }

        /**
         * @brief Install a network-wide signal allowlist (coverage gate).
         *
         * System display name (e.g. "GPS") -> the RINEX codes admitted to the
         * solve (e.g. {"C1C","L1C","C2W","L2W"}).  An absent constellation or an
         * empty map admits every tracked code (the byte-identical default).  Where
         * a constellation is listed, both the canonical per-band pick
         * (@ref decideConstellationCanonicals) and the channel_mode="all"
         * augmentation skip non-admitted codes, so the canonical falls through to
         * the best admitted channel.  The network producer populates this from
         * per-station coverage (network.signal_min_station_fraction).
         */
        void setSignalAllowlist(std::map<std::string, std::set<std::string>> allowlist) {
            signal_allowlist_ = std::move(allowlist);
        }

        /**
         * @struct CanonicalSignal
         * @brief One locked per-(constellation, band, obs-type) canonical pick.
         *
         * The flat, container-free row shape of @c constellation_canonical_, so a
         * caller (the network producer's state snapshot) can carry the latch across
         * a process boundary without naming the nested map type.  RS-8,
         * docs/SPEC_event_harness.md §21.16.
         */
        struct CanonicalSignal {
            std::string constellation;   /**< System display name, e.g. "GPS". */
            std::string band;            /**< Band name from the system→bands table, e.g. "L2". */
            char        obs_type{ 'C' }; /**< 'C' (code) or 'L' (phase). */
            std::string signal;          /**< The locked RINEX code, e.g. "L2X". */
        };

        /**
         * @struct CanonicalCandidate
         * @brief One channel offered to the canonical-signal decision (card CSL).
         *
         * The caller supplies these in PRIORITY-LIST ORDER, already filtered by
         * the network coverage allowlist and already resolved to a band name, so
         * the decision itself is a pure function of counts and order.
         */
        struct CanonicalCandidate {
            std::string signal;          /**< RINEX code, e.g. "L1W". */
            std::string band;            /**< Band name from the system→bands table, e.g. "L1". */
            char        obs_type{ 'C' }; /**< 'C' (code) or 'L' (phase). */
            int         count{ 0 };      /**< Satellites observing @c signal at the decision epoch. */
        };

        /**
         * @struct CanonicalPick
         * @brief The latch and its runner-up for one (band, obs-type) (card CSL).
         */
        struct CanonicalPick {
            std::string signal;             /**< The latched RINEX code. */
            int         count{ 0 };         /**< Its satellite count at the decision epoch. */
            std::string runner_up;          /**< What the same rule would latch without @c signal; empty when there is no alternative. */
            int         runner_up_count{ 0 };/**< The runner-up's satellite count; 0 when there is none. */
        };

        /**
         * @brief Decide the per-(band, obs-type) canonical signals (card CSL).
         *
         * Pure: no member state, no logging, no I/O — the rule the network
         * producer's latch is built from, exposed so it can be pinned directly
         * (tests/test_canonical_signal_latch.cpp).
         *
         * Candidates with a non-positive count, an empty band or an obs-type
         * that is neither 'C' nor 'L' are ignored.  Within a (band, obs-type)
         * the candidate order is the caller's priority-list order.
         *
         *  - @c CanonicalSignalRule::Count — the highest count wins and a tie
         *    goes to the earlier candidate, i.e. the historical rule exactly.
         *  - @c CanonicalSignalRule::Priority — the FIRST candidate whose count
         *    is at least @p min_coverage times the largest count of that (band,
         *    obs-type) wins.  Afterwards, and only when @p code_follows_phase,
         *    every CODE row follows its band's PHASE tracking attribute, if a
         *    code candidate with that attribute exists and clears the same
         *    floor; the displaced pick becomes the runner-up.
         *
         * The runner-up is defined under the SAME rule: what would have been
         * latched had the winner not been offered (so under @c Count it is the
         * second-highest count, and under @c Priority the next eligible entry of
         * the priority list, its floor recomputed over the remaining candidates).
         *
         * @param candidates   Channels in priority-list order (any band, any type).
         * @param rule         The decision rule.
         * @param min_coverage Coverage floor for @c Priority; clamped to [0, 1]
         *                     internally, unread under @c Count.
         * @param code_follows_phase Card CSL-3: true (the default, and the CSL-1
         *                     behaviour) runs the follow clause above; false
         *                     leaves each CODE row on the pick the priority rule
         *                     made from the code's own list.  Unread under
         *                     @c Count, which never had the clause.
         * @return (band, obs-type) → pick, for every (band, obs-type) that had at
         *         least one candidate with a positive count.
         */
        static std::map<std::pair<std::string, char>, CanonicalPick>
        decideCanonicalPicks(const std::vector<CanonicalCandidate>& candidates,
                             CanonicalSignalRule rule,
                             double min_coverage,
                             bool code_follows_phase = true);

        /**
         * @brief Scope the canonical-signal rule to a station list (card CSL-2).
         *
         * Pure: the per-station half of @ref decideCanonicalPicks' rule argument.
         * CSL section 4 left the consumer's gain complete with four of the six
         * stations that move to L1W, with equal first-epoch margins at three of
         * them, so a coverage floor cannot split them further and the next split
         * needs a per-station override.
         *
         * An EMPTY @p priority_stations is the pre-card default: @p rule applies
         * at every station.  A non-empty list is a SCOPE, and it decides on its
         * own: a listed station runs @c CanonicalSignalRule::Priority and every
         * other station runs @c CanonicalSignalRule::Count, whatever @p rule says.
         * That is the card's definition, so a list alone is sufficient to arm the
         * scope and @p rule cannot silently widen it.
         *
         * The comparison is case-insensitive and the empty station label (the
         * off-network path, which has no label) is never a member.
         *
         * @param rule              The configured global rule.
         * @param priority_stations The scope; empty means "no scope".
         * @param station           This station's label (@ref setCanonicalTraceLabel).
         * @return The rule this station decides its latch under.
         */
        static CanonicalSignalRule
        canonicalRuleForStation(CanonicalSignalRule rule,
                                const std::vector<std::string>& priority_stations,
                                const std::string& station);

        /**
         * @brief Export the locked canonical-signal latch (RS-8, §21.16).
         *
         * One entry per DECIDED (constellation, band, obs-type) triple, in
         * @c constellation_canonical_ iteration order (deterministic — nested
         * @c std::map).  A constellation not yet decided contributes nothing, and
         * so does the degenerate case of one decided with an EMPTY candidate set
         * (no admitted priority signal was tracked at its first epoch): such a
         * constellation restores as UNDECIDED and re-decides, which is exactly the
         * pre-card behaviour for it.
         *
         * @return The flat rows; empty before the first processed epoch.
         */
        std::vector<CanonicalSignal> exportCanonicalSignals() const;

        /**
         * @brief Restore a previously exported canonical-signal latch (RS-8, §21.16).
         *
         * Refills @c constellation_canonical_ from @p entries and marks every
         * constellation they name DECIDED, so @c decideConstellationCanonicals'
         * first-epoch guard leaves it alone instead of re-deciding it from the
         * resume epoch's satellite counts — the defect §21.16 names (at a warm
         * resume GAMB00PYF flipped L2X→L2W and CPVG00CPV L2W→L2X, the L2X tracker
         * set changed, and the eq34 L2X component lost its datum receiver).
         *
         * Each restored constellation is also marked AUDIT-PENDING: the next
         * @c decideConstellationCanonicals call runs its tally diagnostically —
         * writing nothing — and counts the entries that differ from what this
         * station WOULD have decided at that epoch
         * (@ref canonicalAuditDisagreements).  That audit is the only behaviour a
         * restore adds beyond the latch itself, and it is read-only.
         *
         * Emits the canonical-signal rider on @c main_log, tagged @c restored.
         *
         * @param entries Rows from @ref exportCanonicalSignals, in any order.  A row
         *        with an empty constellation / band / signal, or an obs-type that is
         *        neither 'C' nor 'L', is ignored.
         * @return The number of entries actually installed.
         */
        std::size_t restoreCanonicalSignals(const std::vector<CanonicalSignal>& entries);

        /**
         * @brief The CODE signal this station LATCHED on one RINEX band digit.
         *
         * A const read of @c constellation_canonical_, which
         * @c decideConstellationCanonicals fills on the constellation's first observed
         * epoch and then locks for the run (or which @ref restoreCanonicalSignals
         * refills at a warm resume).  Card SCC needs it OUTSIDE this class: the
         * receiver side (R2) routes a band-1 code through an inter-signal state exactly
         * when it is not the station's latched canonical code, and the row-weight seam
         * (R5) has to reach the same verdict from the assembly.
         *
         * Keyed on the signal's own BAND DIGIT rather than on the band NAME the map
         * uses, because that name is constellation-specific ("L1" on GPS, "E1" on
         * Galileo, "B1C" on BeiDou) while the card's scope — "band 1" — is the digit in
         * the RINEX code.  The two are 1:1 within a constellation: the prefix table
         * maps each "C<digit>" to exactly one band name.
         *
         * An EMPTY return means the latch does not know this cell yet, which every
         * caller must read as "change nothing" — not as "no signal is canonical".
         *
         * @param system     System name string, e.g. "GPS".
         * @param band_digit RINEX band digit, e.g. '1'.
         * @return The latched RINEX code signal, or empty when undecided.
         */
        std::string latchedCanonicalCodeOnBandDigit(const std::string& system,
                                                    char band_digit) const {
            const auto cit = constellation_canonical_.find(system);
            if (cit == constellation_canonical_.end()) return {};
            for (const auto& [key, sig] : cit->second) {
                if (key.second != 'C') continue;
                if (sig.size() >= 2 && sig[1] == band_digit) return sig;
            }
            return {};
        }

        /**
         * @brief The PHASE signal this station LATCHED on one RINEX band digit.
         *
         * The exact analogue of @ref latchedCanonicalCodeOnBandDigit over the 'L'
         * entries of @c constellation_canonical_, and read the same way: filled on
         * the constellation's first observed epoch, locked for the run, refilled by
         * @ref restoreCanonicalSignals at a warm resume.  Card GRK needs it OUTSIDE
         * this class: the arc-level slip tracker picks one phase per band from the
         * raw record, the carried ambiguity is keyed on the latch, and the gap
         * re-entry response cannot reach an arc when the two disagree.
         *
         * The LATCH rather than the epoch's admitted row set is the right source for
         * that comparison, because the admitted set is per epoch: a returning arc's
         * phase can be withheld from it for the first epochs by the admission or
         * readmit machinery, which is exactly the re-entry epoch the tracker has to
         * name correctly.  The latch does not move.
         *
         * Keyed on the signal's own BAND DIGIT rather than on the band NAME the map
         * uses, for the reason given on the code accessor.
         *
         * An EMPTY return means the latch does not know this cell yet, which every
         * caller must read as "change nothing" - not as "no signal is canonical".
         *
         * @param system     System name string, e.g. "GPS".
         * @param band_digit RINEX band digit, e.g. '1'.
         * @return The latched RINEX phase signal, or empty when undecided.
         */
        std::string latchedCanonicalPhaseOnBandDigit(const std::string& system,
                                                     char band_digit) const {
            const auto cit = constellation_canonical_.find(system);
            if (cit == constellation_canonical_.end()) return {};
            for (const auto& [key, sig] : cit->second) {
                if (key.second != 'L') continue;
                if (sig.size() >= 2 && sig[1] == band_digit) return sig;
            }
            return {};
        }

        /**
         * @brief Set the label this model prints in the canonical-signal rider (RS-8).
         *
         * The network producer sets it to the station name at construction; the
         * single-receiver paths leave it empty and the rider prints "-".  Log-only —
         * nothing else reads it.
         *
         * @param label Station / receiver label.
         */
        void setCanonicalTraceLabel(std::string label) {
            canonical_trace_label_ = std::move(label);
        }

        /**
         * @brief Canonical entries the restore audit compared (RS-8, §21.16).
         * @return Restored entries the first post-resume epoch's tally could be
         *         compared against; 0 on any run that never restored a latch.
         */
        long long canonicalAuditEntries() const { return canon_audit_entries_; }

        /**
         * @brief Canonical entries the restore audit found DISAGREEING (RS-8, §21.16).
         *
         * A restored (band, obs-type) whose signal differs from the one this
         * station's first post-resume epoch would have picked — exactly the flips
         * §21.16 measured.  A nonzero count is the defect being PREVENTED, not an
         * error; a zero count on a leg that resumed is the invariant-#4 reading that
         * the seam epoch happened to agree with the writer.
         *
         * @return Disagreeing entries; 0 on any run that never restored a latch.
         */
        long long canonicalAuditDisagreements() const { return canon_audit_disagree_; }

        /**
         * @brief Compute the per-signal I₂ + I₃ correction (m) to add
         *        to the modeled observable.
         *
         * Returns 0 when the correction is disabled in config, no GIM was
         * injected, or the GIM has no usable VTEC for this line of sight (below
         * horizon, sentinel cell).
         *
         * @param time         Epoch time of the observation.
         * @param rx_pos       Receiver ECEF position (m).
         * @param sat_pos      Satellite ECEF position (m).
         * @param frequency_hz Carrier frequency (Hz).
         * @param meas_type    'C' for code (positive group delay) or 'L' for
         *                     phase (negative, scaled 1/2 for I₂, 1/3 for I₃).
         * @return Signed correction (m), added directly to the modeled value.
         */
        double computeHigherOrderIonoCorrection(const GnssTime& time,
                                                const Eigen::Vector3d& rx_pos,
                                                const Eigen::Vector3d& sat_pos,
                                                double frequency_hz,
                                                char meas_type) const;

        /**
         * @brief Default destructor.
         */
        ~PppModel() = default;

    private:
        // Test seam: PppModelCanonicalTestAccess (tests/test_resume_signal_canon_restore.cpp)
        // reaches decideConstellationCanonicals + the latch members so the RS-8 restore's
        // contract — the first-epoch guard must NOT re-decide a restored latch — can be
        // pinned without a full geometry / product chain.  Defined only in gnss_tests.
        friend struct PppModelCanonicalTestAccess;

        const GnssConfig& config_;             /**< Reference to configuration (read-only). */
        ProductParser& product_parser_;        /**< Product parser providing SP3/CLK/ERP/etc. */
        LogManager& log_manager_;              /**< Logging manager used for diagnostic messages. */
        const RinexParser& rinex_parser_;      /**< RINEX parser for observation metadata. */
        StateManager& state_manager_;          /**< State manager for ambiguity and state bookkeeping. */

        PppUtils ppp_utils_;                   /**< Utility helpers for PPP math and constants. */
        std::optional<Ionosphere> ionosphere_; /**< Klobuchar fallback model.  Constructed only when @c model_corrections.enable_klobuchar_ionosphere is true; std::nullopt otherwise.  Constructing it pulls in another @c Model::Geometry plus a Klobuchar coefficient parse, both of which are wasted in IF / DD / PPP-RTK-uncombined where Klobuchar is never queried. */
        const GlobalIonosphereMap* gim_{ nullptr }; /**< Non-owning pointer to the GIM that supplies absolute STEC and IPP geometry for the higher-order correction.  Injected via @c setGim() by the Application driver once both the PppModel and the GIM exist.  Nullptr when higher-order ionosphere is disabled or no IONEX product resolved — in which case @c computeHigherOrderIonoCorrection returns 0 m and the modeled values reduce to the first-order-only path. */
        HigherOrderIonosphere higher_order_iono_; /**< Stateless physics helper for the second- and third-order ionospheric delay formulae.  Always constructed; @c computeHigherOrderIonoCorrection consults @c gim_ and the config toggle to decide whether to evaluate. */
        Model::Geometry geometry_;             /**< Geometry and satellite interpolation helpers. */
        Antenna antenna_;                      /**< Antenna model for receiver and satellite corrections. */
        GnssCorrections gnss_corrections_;     /**< Container for clock/orbit/bias/erp corrections. */
        StochasticModelWeight stochastic_model_; /**< Stochastic (weighting) model for variance computation. */

        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priority lists (by system). */
        std::vector<double> klobuchar_coeffs_;  /**< Klobuchar coefficients (if available) */

        /// Satellites already logged as phase-gated (broadcast-grade clock →
        /// code-only).  Without this the info line fires once per epoch per
        /// satellite for the whole run; we log once per satellite per session.
        mutable std::set<std::string> sats_phase_gated_;

        /// Card 5 (RTF-1 / plan D1): when true, @c computeModels KEEPS carrier phase
        /// on broadcast-grade satellites (the brdc_rts network producer estimates the
        /// satellite clock, so phase is informative) instead of the default code-only
        /// gate.  Set via @ref setBroadcastPhaseGateSuppressed on producer station
        /// models only; the default @c false is byte-identical for every other caller.
        bool broadcast_phase_gate_suppressed_{false};

        /// Satellites already logged as phase-RETAINED under the gate-suppressed
        /// (brdc_rts producer) mode — the one-shot companion to @c sats_phase_gated_,
        /// so the operator sees the mode once per satellite, not once per epoch.
        mutable std::set<std::string> sats_phase_retained_;

        /**
         * @brief Per-constellation canonical signal cache.
         *
         * Constellation -> (band, obs-type char {'C','L'}) -> canonical signal
         * code (e.g. "C1W", "L5Q").  decideConstellationCanonicals fills it on
         * the constellation's first observed epoch and it is then locked for the
         * run.  The stability matters: without it a brief low-elevation dropout of
         * the priority canonical would switch the KF to another tracking
         * attribute (L1W → L1C) and leak an inter-channel-bias-contaminated
         * ambiguity state.
         */
        std::map<std::string,
                 std::map<std::pair<std::string, char>, std::string>>
                constellation_canonical_;
        std::set<std::string> constellation_canonical_decided_; /**< Constellations whose canonical set is locked. */

        /**
         * @brief Constellations whose latch was RESTORED and not yet audited (RS-8).
         *
         * Populated only by @ref restoreCanonicalSignals.  The next
         * @c decideConstellationCanonicals call runs the tally for such a
         * constellation diagnostically — comparing, never writing — then erases it,
         * so the audit costs exactly one extra tally per restored constellation and
         * is unreachable on a run that never restores.
         */
        std::set<std::string> constellation_canonical_audit_pending_;
        long long canon_audit_entries_{ 0 };   /**< RS-8: restored entries the audit compared. */
        long long canon_audit_disagree_{ 0 };  /**< RS-8: restored entries the first epoch's tally would have picked differently. */
        /// RS-8 rider label (@ref setCanonicalTraceLabel); empty off the network path.
        std::string canonical_trace_label_;

        /**
         * @brief Network-wide signal coverage allowlist (see @ref setSignalAllowlist).
         *        system → admitted RINEX codes.  Empty (default) = admit all.
         */
        std::map<std::string, std::set<std::string>> signal_allowlist_;

        /**
         * @brief Whether a RINEX code is admitted for @p system under the coverage
         *        allowlist.  True (admit) when the feature is off (empty allowlist)
         *        or the system is not gated; otherwise membership in its admitted set.
         */
        bool signalAdmitted(const std::string& system, const std::string& code) const {
            if (signal_allowlist_.empty()) return true;
            auto it = signal_allowlist_.find(system);
            if (it == signal_allowlist_.end()) return true;
            return it->second.count(code) > 0;
        }

        /**
         * @brief Decide the per-constellation canonical signal set from
         *        the first epoch's observations.
         *
         * For each not-yet-decided constellation, counts how many satellites
         * observe each priority-list signal this epoch.  Per (band, obs_type) the
         * highest count wins, ties broken by signal_priorities_ order.  The
         * constellation is then marked decided, so later epochs are no-ops.
         *
         * @param observations Observations for the current epoch.
         */
        void decideConstellationCanonicals(const std::vector<RinexObservation>& observations);

        // --- Signal selection helpers (decomposed from selectSignals) ---

        /**
         * @brief Top-level signal selection: observation pairs or singles.
         *
         * Delegates to buildFrequencyBandMap(), selectUncombinedSignals(),
         * selectIonoFreePairs() and selectSingleSignalFallback() as needed.
         */
        bool selectSignals(const RinexObservation& obs, const std::string& system_str,
            std::vector<std::pair<std::string, std::string>>& selected_signals,
            bool& is_iono_free, std::string& comb_type,
            std::set<std::string>* companion_out = nullptr);

        /**
         * @brief Map available RINEX measurements to frequency bands.
         *
         * Records the best code and best phase signal per band, over the
         * configured priorities present in @p measurements.
         *
         * @param measurements Available measurements.
         * @param system_str   GNSS system name (e.g. "GPS").
         * @param signals      Priority-ordered signal list for the system.
         * @param[out] freq_band_to_signal  Map from band name to (best_code, best_phase).
         * @param[out] freq_bands           Frequency bands in discovery order.
         * @return true if at least one band was populated.
         */
        bool buildFrequencyBandMap(const ObsValueMap<double>& measurements,
            const std::string& system_str,
            const std::vector<std::string>& signals,
            std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
            std::vector<std::string>& freq_bands) const;

        /**
         * @brief Select individual code/phase signals per band (PPP-RTK uncombined).
         *
         * @param companion_out Optional; card SCC (R1).  When non-null and
         *        `network.second_code_channel` is armed on the network path, receives
         *        the allow-listed band-1 CODE signals this call added BESIDES the one
         *        the band map selected.  Those keys are also in @p selected_signals —
         *        the measurement model needs all three of observed / modeled /
         *        variances to accept a row — so the set is what tells every downstream
         *        consumer which rows are companions (the Melbourne-Wubbena guard, R4,
         *        and the cell telemetry, R7).  Left untouched when the knob is off, so
         *        an unarmed run never allocates.
         */
        bool selectUncombinedSignals(const RinexObservation& obs, const std::string& system_str,
            const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
            const std::vector<std::string>& freq_bands,
            std::vector<std::pair<std::string, std::string>>& selected_signals,
            std::set<std::string>* companion_out = nullptr) const;

        /**
         * @brief Select best iono-free code+phase pairs from available bands.
         */
        bool selectIonoFreePairs(const RinexObservation& obs, const std::string& system_str,
            const std::vector<std::string>& signals,
            const std::map<std::string, std::pair<std::string, std::string>>& freq_band_to_signal,
            const std::vector<std::string>& freq_bands,
            std::vector<std::pair<std::string, std::string>>& selected_signals) const;

        /**
         * @brief Fall back to best available single code + single phase signal.
         */
        void selectSingleSignalFallback(const RinexObservation& obs,
            const std::vector<std::string>& signals,
            std::vector<std::pair<std::string, std::string>>& selected_signals) const;

        /**
         * @brief Determine which observation types (keys) to process from the measurements map.
         */
        std::vector<std::string> selectObservationTypes(const ObsValueMap<double>& measurements,
            System system, const std::string& system_str) const;

        // --- Observation processing helpers ---

        /**
         * @brief Base modeled range shared by the iono-free and uncombined paths.
         *
         * Covers geometric range, satellite clock, relativistic correction, dry
         * troposphere, antenna PCO/PCV, code/phase bias, Shapiro delay,
         * atmospheric loading, solid tide and pole tide.
         *
         * @param obs           RINEX observation (satellite ID, system, time).
         * @param ppp_obs       PppObservation with LOS vector and mapping factors.
         * @param geom          Pre-computed satellite geometry.
         * @param signal_type   Signal key (e.g. "C1W", "L1C") for antenna/bias lookup.
         * @return Base modeled value in meters.
         */
        double computeBaseModeledValue(const RinexObservation& obs,
            const PppObservation& ppp_obs,
            const SatGeometry& geom,
            const std::string& signal_type) const;

        /**
         * @brief Additive phase-variance term (m²) for a (sat, signal)
         *        whose receiver ATX correction was frequency-fallback-
         *        substituted.
         *
         * Returns 0 for code rows, non-substituted signals, or when
         * model_corrections.atx_substitution_phase_std_m ≤ 0 (the default).
         * Otherwise @f$(\sigma_\mathrm{atx}/\sin\,\mathrm{el})^2@f$, added to the
         * stochastic-model phase variance BEFORE any iono-free combination so the
         * IF coefficients propagate it automatically.
         *
         * This targets the azimuthally-symmetric, elevation-dependent receiver-PCV
         * residual a substituted antenna pattern leaves behind — common across
         * satellites and aliasing into ZWD / station height.  It is NOT the
         * per-satellite slowly-varying bias that the ambiguity_q_substituted_atx
         * random walk already absorbs.
         *
         * @param signal        RINEX-3 code (e.g. "L8X"); 'C…' rows return 0.
         * @param satellite     Satellite identifier (e.g. "E18").
         * @param system        GNSS system enum.
         * @param elevation_deg Satellite elevation (degrees).
         * @return Additive phase variance (m²), or 0 when not applicable.
         */
        double phaseAtxSubstitutionVarInflation(const std::string& signal,
            const std::string& satellite,
            System system,
            double elevation_deg) const;

        /**
         * @brief Compute pole-tide LOS projection shared by iono-free and uncombined paths.
         * @param rx_pos Receiver ECEF position.
         * @param time   Epoch time.
         * @param los    LOS unit vector (receiver?satellite).
         * @return Pole tide correction projected onto the LOS (meters).
         */
        double computePoleTideProjection(const Eigen::Vector3d& rx_pos,
            const GnssTime& time,
            const Eigen::Vector3d& los) const;

        /**
         * @brief Process a single iono-free (or composed) observation and fill a PppObservation.
         */
        bool processIonoFreeObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::pair<std::string, std::string>> selected_signals,
            const std::string& comb_type, const SatGeometry& geom);

        /**
         * @brief Process a single iono-free code or phase pair and write results into ppp_obs.
         *
         * Extracted from the inner lambda of processIonoFreeObservation.
         *
         * @return true if the pair was successfully modeled.
         */
        bool processIonoFreePair(const RinexObservation& obs, PppObservation& ppp_obs,
            const std::string& system_str,
            const std::pair<std::string, std::string>& sig_pair,
            char meas_type,
            const SatGeometry& geom,
            double pole_tide_corr,
            const std::string& comb_type);

        /**
         * @brief Process a single uncombined observation and fill a PppObservation.
         */
        bool processUncombinedObservation(const RinexObservation& obs, PppState& state,
            PppObservation& ppp_obs, const std::string& system_str,
            const std::vector<std::pair<std::string, std::string>>& selected_signals,
            const SatGeometry& geom);

    };

    /**
     * @brief RINEX band digits activated by a @c FrequencyMode for a constellation.
     *
     * Free so signal-selection logic outside PppModel can use it, most notably
     * MwSlipDetector filtering raw observations by band.  Returns the digits
     * appearing as the second character of RINEX-3 codes: GPS Dual → {'1','2'},
     * Galileo Dual → {'1','5'} (E1 + E5a), GPS Triple → {'1','2','5'}.
     *
     * @param constellation Constellation name ("GPS", "Galileo", ...).
     * @param mode          Frequency mode (Dual / Triple / All).
     * @return Set of RINEX band digits; empty if the constellation is unknown.
     */
    std::set<char> getAllowedBandDigits(const std::string& constellation, FrequencyMode mode);

} // namespace GnssProcessor