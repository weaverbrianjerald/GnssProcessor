#pragma once

/**
 * @file NetworkIonoFile.hpp
 * @brief The network's slant ionosphere, read by the user from a network run's own traces (stage 1 of the user's
 *        network ionosphere, @c network.closed_loop.iono_file).
 *
 * The network estimates a free slant ionosphere per receiver and satellite each epoch and writes it to
 * @c logs/network_ionosphere.csv (@c iono_total_m: the absolute slant delay on GPS L1, the reference band of the one
 * observation equation the network and the user share). That value is at the centimetre level or better only where
 * the network holds the link's integers: a float link carries the decimetre errors of its ambiguities. The reader
 * therefore marks each link at each epoch as integer or not from two further traces of the same run:
 *  - @c network_ring_fix.csv: a row whose @c state is @c held or @c decided (the ring fix holds the link's pair);
 *  - @c network_chain_edges.csv: a row with a pin target (@c target_cyc) is a chain pin of one band; a link pinned on
 *    two bands has its integers by the datum's definition.
 * The receivers' positions come from @c network_stations.csv (the plane combination needs them).
 *
 * Only the listed receivers are kept. Epochs are keyed by continuous GPS seconds (week x 604800 + time of week) and
 * matched to the user's epoch within half a second. The table is read once, at the start of the run; the product
 * record that would carry the same values to a user anywhere is stage 2 and is not built here.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/// One network link (receiver, satellite) at one epoch.
struct NetworkIonoLink {
    double iono_m{ 0.0 };         ///< Absolute slant delay on GPS L1 (@c iono_total_m), metres.
    double elevation_deg{ 0.0 };  ///< Elevation of the satellite at the network receiver, degrees.
    bool   integer{ false };      ///< The network holds this link's integers at this epoch.
};

/// The links of one epoch: receiver -> satellite -> link.
using NetworkIonoEpoch = std::map<std::string, std::map<std::string, NetworkIonoLink>>;

/// What the reader kept, for the run-start line.
struct NetworkIonoFileCounts {
    std::size_t rows{ 0 };           ///< Rows of network_ionosphere.csv of the listed receivers.
    std::size_t rows_integer{ 0 };   ///< Of those, links whose integers the network holds at that epoch.
    std::size_t epochs{ 0 };         ///< Distinct epochs.
    std::size_t ring_fix_rows{ 0 };  ///< Held or decided ring-fix rows of the listed receivers.
    std::size_t pin_rows{ 0 };       ///< Chain pin rows (one band each) of the listed receivers.
};

/**
 * @class NetworkIonoFile
 * @brief The slant ionosphere of the listed network receivers, epoch by epoch, with each link's integer flag.
 */
class NetworkIonoFile {
public:
    /**
     * @brief Read one network run's traces for the listed receivers.
     *
     * @param logs_dir  The network run's logs folder (holding network_ionosphere.csv, network_ring_fix.csv,
     *                  network_chain_edges.csv and network_stations.csv).
     * @param stations  Receivers to keep (nine-character names as the network writes them).
     * @param[out] error  Why the read failed (a missing file, a missing column, a receiver with no row); empty on
     *                  success.
     * @return The table; empty when @p error is set.
     */
    static NetworkIonoFile read(const std::string& logs_dir, const std::vector<std::string>& stations,
                                std::string& error);

    /**
     * @brief The links of the epoch nearest @p gps_seconds, within half a second.
     * @param gps_seconds  Continuous GPS seconds of the user's epoch.
     * @return The epoch's links, or nullptr when the network run has no epoch there.
     */
    const NetworkIonoEpoch* epochAt(double gps_seconds) const;

    /**
     * @brief The ECEF position of a listed receiver (network_stations.csv).
     * @param station  Receiver name.
     * @param[out] ecef  Position, metres.
     * @return false when the receiver has no position.
     */
    bool position(const std::string& station, std::array<double, 3>& ecef) const;

    /// What the reader kept.
    const NetworkIonoFileCounts& counts() const { return counts_; }

    /// One line for the run start: receivers, epochs, rows and the integer share.
    std::string runStartLine() const;

    /**
     * @brief Add one link (the reader's own path, and the unit tests' way to build a table).
     * @param gps_seconds  Continuous GPS seconds of the epoch.
     * @param station  Receiver name.
     * @param sat  Satellite identifier.
     * @param link  The link.
     */
    void addLink(double gps_seconds, const std::string& station, const std::string& sat, const NetworkIonoLink& link);

    /**
     * @brief Set a receiver's position (the reader's own path, and the unit tests' way).
     * @param station  Receiver name.
     * @param ecef  Position, metres.
     */
    void setPosition(const std::string& station, const std::array<double, 3>& ecef);

private:
    static std::int64_t keyOf(double gps_seconds);           ///< Epoch key: tenths of a second.

    std::map<std::int64_t, NetworkIonoEpoch> epochs_;           ///< Epoch key -> links.
    std::map<std::string, std::array<double, 3>> positions_;    ///< Receiver -> ECEF position.
    std::vector<std::string> stations_;                         ///< The listed receivers.
    NetworkIonoFileCounts counts_;                              ///< What was kept.
};

}  // namespace GnssProcessor
