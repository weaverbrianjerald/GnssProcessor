#pragma once
/**
 * @file BrdcClockApriori.hpp
 * @brief The native broadcast-clock a priori: the ephemeris selector, the
 *        continuous internal a priori with its per-ephemeris offset table, the
 *        transmission-time store feed and the per-IODE wire stamp.
 *
 * Under @c network.apriori_source @c sp3_brdc_clock the producer's orbits come from SP3
 * and its a priori satellite clock from the broadcast navigation message; no CLK product
 * is loaded.  The record of satellite s at epoch t is the latest one with a transmission
 * time at or before t and health 0 (@c network.brdc_select @c ttom, the real-time rule
 * and the rule of the bed `scripts/clock_level/scl_brdc_apriori.py --continuous --rule
 * ttom` that the clock-level studies measured).
 *
 * The a priori is kept continuous across every ephemeris cut-over by an offset table O:
 *
 *     a_s(t) = brdc_k(t) + O_s,k
 *
 * built from 00:00 of the observation day on the 30 s node grid.  At the first node at
 * or after a record's transmission time the selection switches from record k-1 to k and
 * O_k = O_k-1 + brdc_k-1(t_sw) - brdc_k(t_sw) (the bed's rule,
 * `scl_brdc_apriori.py:56-61`), so the estimator sees no step.  The table is built for
 * every satellite in the navigation file whether tracked or not, from a fixed origin, so
 * a park or a warm resume recomputes the same values.  Between nodes the selection of the
 * node at or before the query holds, which is what interpolating the bed's 30 s product
 * gives.
 *
 * The wire refers to the stamped ephemeris: C_wire = C_est + O_s,k with @c iode = k, so
 * a consumer holding record k reconstructs brdc_k(t) + C_wire = a_s(t) + C_est (the
 * card 7 IODE path of @c GnssCorrections::getSatelliteClockCorrection).  The store feed
 * by transmission time keeps the stamped record current (or previous) in
 * @c LiveEphemerisStore when the consumer reads it.
 *
 * Not in this class: a real-time switch delay (post-processing uses 0), multi-day
 * navigation, a mid-day health flip (none on DOY 129), the carry re-level design of
 * @c brdc_rts.
 */

#include "products/BroadcastEphemerisTypes.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

class LiveEphemerisStore;
struct NetworkSolutionEpoch;

/// Ephemeris selection rule of the native broadcast clock a priori (@c network.brdc_select).
enum class BrdcSelectRule {
    Ttom   ///< The latest healthy record whose transmission time is at or before the epoch.
};

/**
 * @class BrdcClockApriori
 * @brief Selector, continuous a priori, store feed and wire stamp.
 *
 * Built once at setup from the parsed navigation records.  Every query is const except
 * @ref feedStore, which advances its own cursor through the staged records.
 */
class BrdcClockApriori {
public:
    /// The a priori of one satellite at one epoch.
    struct Evaluation {
        double clock_s{0.0};    ///< a_s(t) = brdc_k(t) + O_s,k [s].
        double brdc_s{0.0};     ///< brdc_k(t) = af0 + af1 dt + af2 dt^2 of the selected record [s].
        double offset_s{0.0};   ///< O_s,k [s].
        int    iode{-1};        ///< The selected record's IODE in the IGS-SSR matching convention.
        int    record{-1};      ///< Index of the selected record in @ref record.
    };

    /// One constant-selection segment of a satellite's a priori.
    struct Segment {
        double start_s{0.0};    ///< First node of the segment (continuous GPS seconds).
        int    record{-1};      ///< Selected record index; -1 = no eligible record (satellite dropped).
        double offset_s{0.0};   ///< O_s,k of the segment [s].
    };

    /// Counts and ranges of the build, for the setup log and the run-end readout.
    struct BuildStats {
        std::size_t n_records{0};          ///< Records handed in.
        std::size_t n_staged{0};           ///< Healthy records with a transmission time (the selectable set).
        std::size_t n_unhealthy{0};        ///< Records with health != 0 (never selected).
        std::size_t n_no_ttom{0};          ///< Records without a transmission time (never selected).
        std::size_t n_sats{0};             ///< Satellites with at least one selectable record.
        std::size_t n_switches{0};         ///< Cut-overs over the built span, all satellites.
        double      offset_min_m{0.0};     ///< Smallest O over every segment [m].
        double      offset_max_m{0.0};     ///< Largest O over every segment [m].
        double      origin_s{0.0};         ///< The table origin (continuous GPS seconds).
        double      end_s{0.0};            ///< The last built node (continuous GPS seconds).
    };

    /// One satellite's line of the wire stamp (the O trace row).
    struct WireRow {
        std::string sat;          ///< Satellite identifier.
        int         iode{-1};     ///< The stamped IODE; -1 when no record was selectable.
        double      c_est_m{0.0}; ///< The estimate before the stamp [m].
        double      offset_m{0.0};///< O_s,k [m] added to the wire.
        double      brdc_m{0.0};  ///< c * brdc_k(t) [m].
        bool        held{false};  ///< The store holds the stamped record (current or previous).
        bool        switched{false}; ///< The stamped record differs from this satellite's previous stamp.
    };

    /**
     * @brief Build the selector and the offset table.
     * @param records  Parsed navigation records (any constellation; records without a
     *                 transmission time or with health != 0 are kept but never selected).
     * @param origin_s The table origin, 00:00 of the observation day (continuous GPS s).
     * @param end_s    The last time the table must cover (continuous GPS s); the build
     *                 runs to the first node at or after it.
     * @param rule     The selection rule (only @c Ttom).
     * @param node_s   The node spacing [s] (30, the bed's product interval).
     */
    BrdcClockApriori(std::vector<BroadcastEphemeris> records, double origin_s, double end_s,
                     BrdcSelectRule rule = BrdcSelectRule::Ttom, double node_s = 30.0);

    /**
     * @brief 00:00 of the GPS day containing @p t_s.
     * @param t_s Continuous GPS seconds.
     * @return The day's origin in continuous GPS seconds.
     */
    static double dayOrigin(double t_s);

    /**
     * @brief The rule's record for @p sat at exactly @p t_s (no node rounding).
     *
     * The latest record, in (transmission time, toc) order, with health 0, a transmission
     * time at or before @p t_s (1 microsecond tolerance, as the bed) and |t - toe| inside
     * the store's fit window (4 h GPS / QZSS, 2 h Galileo / BeiDou), so a consumer can
     * always evaluate the stamped record.
     *
     * @param sat Satellite identifier.
     * @param t_s Continuous GPS seconds.
     * @return The record index, or @c std::nullopt when none is eligible.
     */
    std::optional<int> select(const std::string& sat, double t_s) const;

    /**
     * @brief The continuous a priori of @p sat at @p t.
     *
     * Uses the segment of the node at or before @p t (clamped to the table's span), so
     * the switch happens exactly at the node where O changes.
     *
     * @param sat Satellite identifier.
     * @param t   Query epoch (GPS scale; an emission time is fine).
     * @return The evaluation, or @c std::nullopt when the satellite has no eligible record
     *         at that node (the caller drops it, as a missing CLK sample).
     */
    std::optional<Evaluation> evaluate(const std::string& sat, const GnssTime& t) const;

    /**
     * @brief The broadcast clock polynomial of record @p rec at @p t_s.
     *
     * af0 + af1 dt + af2 dt^2, dt = t - toc reduced on the native scale exactly as
     * @c LiveEphemerisStore::computeSatelliteClock does (BeiDou on BDT), so the producer's
     * value and the consumer's are the same arithmetic.
     *
     * @param rec Record index.
     * @param t_s Continuous GPS seconds.
     * @return The clock bias [s].
     */
    double clockSeconds(int rec, double t_s) const;

    /// The record at index @p i (as handed in).
    const BroadcastEphemeris& record(int i) const { return records_.at(static_cast<std::size_t>(i)); }

    /// Number of records handed in.
    std::size_t recordCount() const { return records_.size(); }

    /// The segments of @p sat in time order, or @c nullptr when it has none.
    const std::vector<Segment>* segments(const std::string& sat) const;

    /// Build counts and offset range.
    const BuildStats& stats() const { return stats_; }

    /**
     * @brief Feed the store every staged record whose transmission time is at or before
     *        @p now_s (healthy records only, in (transmission time, toc) order).
     *
     * The store's current record is then the latest transmitted one, which the selection
     * at @p now_s stamps, so the consumer's IODE pin finds it as current or previous.
     * Records go in through @c LiveEphemerisStore::insertTransmitted, because the plain
     * insert's freshest-toe rule rejects a new upload whose toe is 16 s earlier.  The
     * first call catches up over any start offset in one sorted batch.  Unhealthy records
     * are not fed: a consumer never sees a broadcast clock the producer would not select.
     *
     * @param store  Destination store (the process singleton in a run).
     * @param now_s  Continuous GPS seconds of the epoch.
     * @return The number of records inserted by this call.
     */
    std::size_t feedStore(LiveEphemerisStore& store, double now_s);

    /// Records fed to the store so far.
    std::size_t fedCount() const { return feed_cursor_; }

    /**
     * @brief Stamp the wire: per clock-bearing satellite of @p sol, set @c clock_iode to
     *        the selected record's IODE and add O to @c clock_c0_m.
     *
     * Applied after the emit-side hold and before the store push and the product write,
     * so every reader of the solve (CLK-R, the holds) has already read C_est.  A
     * satellite without a selectable record keeps its value and @c clock_iode -1, which
     * the row reports.
     *
     * @param sol   The epoch's emission (modified in place).
     * @param t     The epoch time (the selection node).
     * @param store When non-null, each stamped IODE is checked as held (current or
     *              previous) by the store at @p t.
     * @param prev_iode Per-satellite previous stamp, updated in place (for the switch flag).
     * @return One row per clock-bearing satellite, in @p sol order.
     */
    std::vector<WireRow> stampWire(NetworkSolutionEpoch& sol, const GnssTime& t,
                                   const LiveEphemerisStore* store,
                                   std::map<std::string, int>& prev_iode) const;

private:
    void buildTable_();

    std::vector<BroadcastEphemeris>                 records_;
    std::map<std::string, std::vector<int>>         by_sat_;     ///< selectable records per sat, (ttom, toc) order
    std::map<std::string, std::vector<Segment>>     segments_;
    std::vector<int>                                feed_order_; ///< healthy records with ttom, (ttom, toc) order
    std::size_t                                     feed_cursor_{0};
    double                                          origin_s_{0.0};
    double                                          end_s_{0.0};
    double                                          node_s_{30.0};
    BrdcSelectRule                                  rule_{BrdcSelectRule::Ttom};
    BuildStats                                      stats_;
};

} // namespace GnssProcessor
