#pragma once

#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include <Eigen/Dense>
#include <filesystem>
#include "math/TimeUtils.hpp"
#include "corrections/GnssCorrections.hpp"
#include "products/ProductParser.hpp"
#include "model/Planetary.hpp"

namespace GnssProcessor {

	/**
	 * @file Antenna.hpp
	 * @brief Antenna phase-center offset/variation and related corrections.
	 *
	 * This header declares the `Antenna` class, which reads the cached antenna
	 * models (PCO/PCV), applies antenna corrections (including phase wind-up),
	 * and exposes helpers that query the receiver antenna metadata from the
	 * RINEX parser.
	 */

	class RinexParser; // forward-declare

	/**
	 * @brief Provides antenna-related corrections and metadata access.
	 *
	 * The `Antenna` class reads the antenna models (PCO/PCV) held by the
	 * ProductCache, interpolates PCV grids, and computes per-satellite antenna
	 * corrections (in meters): phase center offsets, phase center variations
	 * and, for carrier phase, phase wind-up.
	 *
	 * The class depends on `GnssConfig` for configuration, `ProductParser`
	 * for product/auxiliary data, `LogManager` for logging, and a
	 * `RinexParser` reference for receiver metadata.
	 */
	class Antenna {
	public:

		/**
		 * @brief Construct an Antenna helper.
		 *
		 * Binds `antennas_` to the antenna list held by the ProductCache (no
		 * copy, no file parsing) and initializes the internal correction
		 * objects.  The constructor also dumps the matched receiver antenna
		 * model at trace level (@ref dumpAntennaModels).
		 *
		 * @param config Configuration reference (read-only).
		 * @param product_parser Product parser used to access auxiliary files.
		 * @param log_mgr Log manager used for warnings/errors.
		 * @param rinex_parser Reference to the RINEX parser for receiver metadata.
		 */
		Antenna(const GnssConfig& config, ProductParser& product_parser, LogManager& log_mgr, const RinexParser& rinex_parser);

		/**
		 * @brief Bilinearly interpolate an ANTEX PCV grid at a look direction.
		 *
		 * Pure, stateless helper (exposed for unit testing).  The table rows
		 * are zenith/nadir rings, indexed from ZEN1 (the caller subtracts
		 * ZEN1), and the columns are azimuth sectors, or a single column for
		 * an azimuth-independent (NOAZI) table.  Linear in zenith, bilinear in
		 * (zenith, azimuth) for 2-D tables, with circular azimuth wrap.  A
		 * query at or beyond the table's largest zenith ring is held flat
		 * (clamped to the last ring) rather than extrapolated or read out of
		 * bounds.  A negative zenith is clamped to 0; an empty table or
		 * @p dzen <= 0 returns 0.
		 *
		 * @param pcv_table 2-D PCV grid: rows = zenith rings, cols = azimuth sectors (1 = NOAZI).
		 * @param zenith Zenith/nadir angle in degrees, ZEN1-relative (>= 0).
		 * @param azimuth Azimuth angle in degrees [0, 360).
		 * @param dazi Azimuth grid spacing (degrees); ignored for NOAZI tables.
		 * @param dzen Zenith grid spacing (degrees); must be > 0.
		 * @return Interpolated PCV value in the table's units (millimetres as
		 *         stored from the ATX; getAntennaCorrection() scales by 1e-3).
		 */
		static double interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
			double zenith, double azimuth, double dazi, double dzen);

		/**
		 * @brief Correct a measured azimuth for receiver antenna orientation.
		 *
		 * Pure, stateless helper (exposed for unit testing).  Subtracts the
		 * antenna mounting orientation (degrees, clockwise from local North)
		 * from the measured azimuth and wraps the result once in each
		 * direction by 360.  For both inputs in [0, 360) the result is in
		 * [0, 360).  Ported from M-PAGES @c NGS::RxAntenna::correctAzimuth.
		 * Used by getAntennaCorrection() to rotate the azimuth into the
		 * antenna frame before the receiver PCV look-up when
		 * @c GnssConfig::enableRxAntennaOrientation() is set.
		 *
		 * @param azimuth_deg Measured azimuth (degrees, clockwise from North).
		 * @param orientation_deg Antenna mounting orientation (degrees, CW from North).
		 * @return Orientation-corrected azimuth in degrees.
		 */
		static double correctAzimuth(double azimuth_deg, double orientation_deg);

		/**
		 * @brief Result of receiver-antenna matching (@ref matchReceiverAntenna).
		 */
		struct ReceiverMatch {
			const AntennaData* entry = nullptr;   ///< Matched entry (nullptr = no match); points into the supplied list.
			bool via_radome_none = false;         ///< Matched only via the always-on type-preserving radome→NONE fallback (Tier 1).
			std::string type_substitution;        ///< Deprecated-code TYPE-remap candidate that hit (empty unless matched via the opt-in table).
			int ambiguous_candidates = 0;         ///< Substring candidates in the winning pass (>1 ⇒ ambiguous; 1 for an exact hit).
			std::string sanitized;                ///< Whitespace-stripped receiver type (diagnostics).
			std::string underscore;               ///< Underscore-normalized receiver type (diagnostics).
		};

		/**
		 * @brief Pure, stateless receiver-antenna matcher (exposed for unit testing).
		 *
		 * The single implementation of receiver-antenna resolution.  Like
		 * @ref interpolatePcv and @ref correctAzimuth it is a side-effect-free
		 * static, so the order-sensitive matching logic can be tested directly on
		 * a hand-built @p antennas list without a ProductCache or RinexParser.
		 * Given the loaded ATX antenna list and a raw RINEX receiver antenna type
		 * it applies, in order: (1) exact, then first-substring, match on the
		 * normalized type; (2) the always-on, type-preserving radome→NONE
		 * fallback (Tier 1), which retries the same antenna type with a "NONE"
		 * radome; (3) only when @p enable_type_substitution is true, the opt-in
		 * deprecated-code TYPE-remap table.  Step (2) never remaps the antenna
		 * type, so an unknown model still resolves to nothing.  Does not log;
		 * @ref findReceiverAntennaEntry wraps this and emits the correction-path
		 * ambiguity and substitution warnings.
		 *
		 * @param antennas Loaded ATX antenna models to match against.
		 * @param rx_antenna_type Raw receiver antenna type (RINEX header field, "<16 model><4 radome>").
		 * @param enable_type_substitution Whether the opt-in deprecated-code TYPE-remap table is consulted.
		 * @return A @ref ReceiverMatch; @c entry is nullptr on no match.
		 */
		static ReceiverMatch matchReceiverAntenna(const std::vector<AntennaData>& antennas,
			const std::string& rx_antenna_type,
			bool enable_type_substitution);

		/**
		 * @brief Compute antenna correction for a satellite/receiver observation.
		 *
		 * Computes the combined antenna correction (satellite and receiver
		 * phase center offset (PCO), phase center variation (PCV)
		 * interpolation and, for carrier phase, phase wind-up) and returns a
		 * scalar correction in meters.
		 *
		 * @param satellite Satellite identifier string (e.g. "G01", "E11").
		 * @param freq_type RINEX signal code (e.g. "L1C", "C5Q"); the first two
		 *        characters select the ATX frequency key, and a leading 'L' marks
		 *        carrier phase (wind-up is applied only then).
		 * @param system GNSS system enum for the satellite.
		 * @param rx_pos Receiver ECEF position (meters).
		 * @param sat_pos Satellite ECEF position (meters).
		 * @param sat_vel Satellite ECEF velocity (meters/sec).
		 * @param elevation Satellite elevation angle at the receiver, in degrees
		 *        (values outside [0, 90] return 0).
		 * @param azimuth Satellite azimuth angle at the receiver, in degrees
		 *        (values outside [0, 360) return 0).
		 * @param rx_antenna_type Receiver antenna model/type string (from RINEX header or configuration).
		 * @param time Epoch time at which the correction is computed.
		 * @param sun_pos_hint Optional precomputed Sun ECEF position (meters),
		 *        the rigorous per-epoch value cached by Geometry; a norm below
		 *        1e6 m means "compute internally".
		 * @param skip_satellite_pco When true, the satellite PCO range term is
		 *        omitted (satellite PCV, receiver PCO/PCV and wind-up are
		 *        unaffected).  Used by PppModel when the applied SSR orbit
		 *        correction is APC-referenced (legacy-SSR dialect): the
		 *        corrected orbit already points at the antenna phase center, so
		 *        applying the CoM->APC PCO again would double-count its z-offset
		 *        (0.6-2.6 m along the LOS).  Default false; with it false the
		 *        output is unchanged (RTB-5(c)).
		 * @return Antenna correction in meters (0 for non-finite or out-of-range
		 *         inputs, or a signal with no ATX frequency key).  Positive values
		 *         should be applied to measured ranges as defined by the caller's
		 *         convention.
		 */
		double getAntennaCorrection(const std::string& satellite, const std::string& freq_type, System system,
			const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos, const Eigen::Vector3d& sat_vel,
			double elevation, double azimuth, const std::string& rx_antenna_type,
			const GnssTime& time,
			const Eigen::Vector3d& sun_pos_hint = Eigen::Vector3d::Zero(),
			bool skip_satellite_pco = false) const;

		/**
		 * @brief Get receiver antenna type from the RINEX header.
		 *
		 * Returns the receiver antenna model/type string parsed from the
		 * observation RINEX file header, with all whitespace removed.  If no
		 * receiver antenna metadata exists, logs a warning and returns an
		 * empty string.
		 *
		 * @return Whitespace-stripped receiver antenna type (e.g. "TRMxxxxx") or empty string if unknown.
		 */
		std::string getReceiverAntennaType() const;

		/**
		 * @brief Enable the phase wind-up diagnostic CSV trace.
		 *
		 * Delegates to the internal GnssCorrections instance that computes
		 * the wind-up correction applied by getAntennaCorrection().
		 *
		 * @param log_dir Directory where windup_trace.csv will be created.
		 * @return True if the trace file was opened successfully.
		 */
		bool enableWindUpTracing(const std::filesystem::path& log_dir);

		/// @brief Arm the per-receiver phase wind-up census (see
		///        GnssCorrections::enableWindUpCensus).  Telemetry only.
		void enableWindUpCensus(double wu_gap_s, double carry_gap_s);
		/// @brief The census counters for this receiver's corrections instance.
		const GnssCorrections::WindUpCensus& windUpCensus() const;
		/// @brief The accumulator-restart ledger (§10.4 response; see
		///        GnssCorrections::wuRestarts).
		const std::map<std::string, GnssCorrections::WuRestart>& wuRestarts() const;
		/// @brief Clear the restart ledger (called when the response consumes it).
		void clearWuRestarts();
		/// @brief Install the satellite-state source of the wind-up gap bridge (see
		///        GnssCorrections::setSatStateProvider); inert unless
		///        @c estimation.phase_windup_gap_bridge is on.
		void setWindUpSatStateProvider(GnssCorrections::SatStateProvider provider);
		/// @brief The gap-bridge counters for this receiver's corrections instance.
		const WindUpBridge::Counters& windUpBridgeCounters() const;
		/// @brief The named gap-bridge attempts of this receiver (capped; see WindUpBridge::kMaxEvents).
		const std::vector<WindUpBridge::Event>& windUpBridgeEvents() const;

		/**
		 * @brief Dump the receiver antenna model to trace log for ATX comparison.
		 *
		 * Looks up the receiver antenna type from the RINEX header, finds the
		 * AntennaData entry by exact, then substring, match on the normalized
		 * type (the radome→NONE fallback and the TYPE substitution of
		 * @ref findReceiverAntennaEntry are not applied here), and logs its type,
		 * validity interval, grid parameters (dazi, dzen, zen1), PCO vectors per
		 * frequency, and PCV table values.  The output is written at trace level
		 * to the "antenna_model" logger so it can be compared side-by-side with
		 * the source .ATX file.  Only the single receiver entry is dumped;
		 * satellite models are already traced per-observation in
		 * getAntennaCorrection().
		 */
		void dumpAntennaModels() const;

		/**
		 * @brief Check whether the receiver antenna model has ATX coverage
		 *        for a given signal.
		 *
		 * Maps the RINEX signal code (e.g. "L5Q") to the ATX frequency
		 * key (e.g. "G05") using the same mapping as getAntennaCorrection(),
		 * then checks whether the matched receiver antenna entry has a PCO
		 * vector for that key.  When
		 * @c enable_receiver_cross_system_atx_fallback is set, a PCO vector for
		 * a key on the receiver cross-system chain (nearest GPS band) also
		 * counts; the in-system @c enable_atx_frequency_fallback chain is not
		 * consulted.
		 *
		 * @param signal  RINEX observation code (e.g. "L5Q", "C7Q").
		 * @param system  GNSS system of the observation.
		 * @return True if the receiver antenna has PCO data for this signal's frequency.
		 */
		bool hasReceiverAtxEntry(const std::string& signal, System system) const;

		/**
		 * @brief The route by which the receiver antenna correction of a signal
		 *        resolves to an ATX frequency.
		 *
		 * Runs the same resolution @c getAntennaCorrection() runs for the receiver
		 * PCO (the same matching, the same in-system frequency chain under
		 * @c enable_atx_frequency_fallback, the same receiver-only cross-system chain
		 * under @c enable_receiver_cross_system_atx_fallback) and names which step
		 * supplied the key.  Measure-only.
		 *
		 * @param signal  RINEX observation code (e.g. "L1C", "C5X").
		 * @param system  GNSS system of the observation.
		 * @param out_key [out, optional] The ATX frequency key the correction uses
		 *                ("E01", "G05", ...), empty when none resolves.
		 * @return "exact" (the requested key), "same_system" (an in-system frequency
		 *         substitute), "cross_system" (the nearest GPS band), or "none" (no
		 *         entry resolves, so the receiver correction of the signal is zero).
		 *         An antenna type unknown to the ANTEX, or a signal with no ATX
		 *         frequency key, also returns "none".
		 */
		std::string receiverAtxFrequencySource(const std::string& signal, System system,
		                                       std::string* out_key = nullptr) const;

		/**
		 * @brief Whether the RINEX receiver antenna type matched an ATX model entry.
		 *
		 * Runs the same receiver-antenna matching as getAntennaCorrection()
		 * (@ref findReceiverAntennaEntry on the RINEX-header antenna string, with
		 * the always-on type-preserving radome→NONE fallback and the opt-in TYPE-remap
		 * substitution when @c GnssConfig::enableRxAntennaSubstitution() is set) and
		 * reports whether any entry matched.  A false result means that neither the
		 * antenna type with its own radome nor the same type with radome NONE is in
		 * the loaded ANTEX.  In that case every receiver PCO/PCV correction for this
		 * station is zero without a warning: getAntennaCorrection() returns only the
		 * satellite-side terms and wind-up.  The caller can therefore report it at
		 * startup instead of relying on the per-observation trace.  Emits no
		 * ambiguity warning (the correction path passes @c warn_on_ambiguity=true;
		 * this query passes false) and does not mutate state.  The TYPE-substitution
		 * warning still fires when the opt-in table supplies the entry.
		 *
		 * @return True iff an ATX entry matches the receiver antenna type.
		 */
		bool hasReceiverAntennaMatch() const;

		/**
		 * @brief Whether the receiver antenna resolved only via the always-on
		 *        type-preserving radome→NONE fallback (Tier 1).
		 *
		 * Runs the same matching as @ref hasReceiverAntennaMatch on the
		 * RINEX-header antenna string and reports whether the exact/substring
		 * match missed and the radome→NONE fallback then supplied the entry,
		 * i.e. the station's receiver PCO/PCV comes from the radome-agnostic
		 * "NONE" calibration of the same physical antenna, not from its
		 * uncalibrated radome code.  This is standard IGS/EPN practice
		 * (mm-level); a single startup diagnostic keeps the distinction from an
		 * exact match visible.  Returns false when the exact (type, radome)
		 * matched, or when nothing matched at all (that is
		 * @ref hasReceiverAntennaMatch == false).  Emits no ambiguity warning and
		 * does not mutate state.
		 *
		 * @param out_detail [out,optional] Human-readable description of the
		 *        substitution (the RINEX type, its uncalibrated radome code and the
		 *        NONE ATX entry used) for a caller's WARN; may be nullptr.
		 * @return True iff the receiver antenna matched only through radome→NONE.
		 */
		bool receiverAntennaViaRadomeFallback(std::string* out_detail = nullptr) const;

		/**
		 * @brief Strict ATX coverage check: both satellite and receiver
		 *        antenna entries must supply finite PCO and populated
		 *        PCV grids for the *exact* requested frequency.
		 *
		 * Used when @c model_corrections.require_atx_coverage is true
		 * to filter out observations whose (sat, signal) pair would
		 * otherwise process without proper antenna calibration.  The
		 * @c enable_atx_frequency_fallback chain is deliberately not
		 * consulted by this query.  The one exception is the receiver half:
		 * when @c enable_receiver_cross_system_atx_fallback is set, a receiver
		 * entry that has finite PCO and populated PCV for a key on the
		 * receiver cross-system chain (nearest GPS band) also counts.  The
		 * satellite entry is the first one whose type contains the PRN; its
		 * validity window is not checked.
		 *
		 * @param satellite Satellite identifier (e.g. "G05", "E11").
		 * @param signal    RINEX-3 observation code (e.g. "L5Q", "C7Q").
		 * @param system    GNSS system enum for the satellite.
		 * @return True iff sat and rx entries each have finite PCO
		 *         and non-empty PCV for the signal's exact frequency
		 *         (or, for the receiver, a cross-system key as above).
		 */
		bool hasFullAtxCoverage(const std::string& satellite,
		                        const std::string& signal,
		                        System system) const;

		/**
		 * @brief Check whether the ATX correction for this (sat, signal)
		 *        is being computed via the @c enable_atx_frequency_fallback
		 *        substitution chain rather than from an exact frequency
		 *        entry on both sides.
		 *
		 * Returns true iff @c hasFullAtxCoverage is false, the fallback is
		 * enabled, and the in-system fallback chain resolves a non-empty
		 * alternate frequency for at least one of the four entries (sat PCO,
		 * sat PCV, rx PCO, rx PCV), so a substitution on either the satellite
		 * side or the receiver side is enough.  In that case the antenna
		 * correction is an approximation derived from a neighbouring
		 * frequency, which matters for downstream uncertainty management
		 * (looser ambiguity process noise, AR exclusion, etc.).
		 *
		 * Returns false when the strict coverage holds, when the fallback is
		 * disabled, or when the (sat, signal) has no entries at all (the
		 * correction is then zero, not "substituted").  The receiver
		 * cross-system chain is not examined here; a receiver covered through
		 * it makes @c hasFullAtxCoverage true and this query false.
		 *
		 * @param satellite Satellite identifier (e.g. "G05").
		 * @param signal    RINEX-3 observation code (e.g. "L5Q").
		 * @param system    GNSS system enum.
		 */
		bool wasAtxSubstituted(const std::string& satellite,
		                       const std::string& signal,
		                       System system) const;

		/**
		 * @brief Access the parsed antenna model entries.
		 * @return Const reference to the antenna data vector.
		 */
		const std::vector<AntennaData>& getAntennas() const { return antennas_; }
	private:
		/** @brief Logger for informational and diagnostic messages. */
		LogManager& log_manager_;
		/** @brief Read-only configuration settings. */
		const GnssConfig& config_;
		/** @brief GNSS corrections helper (clocks, ERP, relativistic, etc.); used here for phase wind-up. */
		GnssCorrections gnss_corrections_;
		/** @brief Reference to cached antenna models and PCV/PCO data (zero-copy from ProductCache). */
		const std::vector<AntennaData>& antennas_;
		/** @brief Reference to RINEX parser (for receiver antenna metadata). */
		const RinexParser& rinex_parser_;
		/** @brief Product parser used to find auxiliary files (ATX, etc.). */
		ProductParser& product_parser_;
		/** @brief PPP helper utilities (frequencies, wavelengths, conversions). */
		PppUtils ppp_utils_;
		/** @brief Planetary helper used for Sun position / yaw-steering calculations. */
		Planetary planetary_;

		/**
		 * @brief Report the cached antenna model count.
		 *
		 * `antennas_` is a reference into the ProductCache, so there is nothing
		 * to load; this method only logs the entry count at debug level.  The
		 * constructor does not call it, and no other code does.
		 */
		void loadAntennaData();

		/**
		 * @brief Match a RINEX receiver antenna type string to an ATX entry.
		 *
		 * The member entry point for receiver-antenna matching, shared by
		 * getAntennaCorrection() and the coverage queries
		 * (hasReceiverAtxEntry / hasFullAtxCoverage / wasAtxSubstituted).  The
		 * matching itself is @ref matchReceiverAntenna.
		 * The raw type is whitespace-sanitized and underscore-normalized,
		 * then matched against @c antennas_ by (1) exact match on either
		 * normalized form, then (2) first substring containment.  On a miss,
		 * (3) an always-on, type-preserving radome→NONE fallback (Tier 1)
		 * retries the same antenna type with a "NONE" radome.  This is the
		 * documented IGS/EPN practice of borrowing a physical antenna's
		 * radome-agnostic calibration when its specific radome code is
		 * uncalibrated (mm-level).  Finally, only when
		 * @c GnssConfig::enableRxAntennaSubstitution() is set, (4) a rule-driven
		 * TYPE-remap substitution (deprecated-code table) is attempted, with a
		 * warning on success.  The radome→NONE step never remaps the antenna
		 * type, so an unknown model still resolves to nothing (zero receiver
		 * correction).  A station that matches at (1) or (2) returns before
		 * steps (3) and (4), so its result does not depend on them.
		 *
		 * @param rx_antenna_type Raw receiver antenna type (RINEX header field).
		 * @param warn_on_ambiguity Emit a warning when >1 substring candidate matches (true only for the correction path).
		 * @param out_sanitized [out,optional] Whitespace-stripped form (for caller diagnostics); may be nullptr.
		 * @param out_underscore [out,optional] Underscore-normalized form (for caller diagnostics); may be nullptr.
		 * @param out_via_radome_none [out,optional] Set true when the exact/substring
		 *        match missed but the always-on type-preserving radome→NONE fallback
		 *        (Tier 1) supplied the entry; false otherwise. May be nullptr.
		 * @return Pointer to the matched AntennaData (owned by @c antennas_), or nullptr.
		 */
		const AntennaData* findReceiverAntennaEntry(const std::string& rx_antenna_type,
			bool warn_on_ambiguity,
			std::string* out_sanitized = nullptr,
			std::string* out_underscore = nullptr,
			bool* out_via_radome_none = nullptr) const;

	};

}