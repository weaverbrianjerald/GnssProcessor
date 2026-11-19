#pragma once

/**
 * @file RxSatCodeBiasStore.hpp
 * @brief The file that keeps the learned receiver-satellite code biases from one run to the next.
 *
 * The receiver-satellite code-bias family (@c StateFamily::RxSatCodeBias) holds, per
 * satellite and raw code signal, the receiver's deviation from the published satellite code
 * bias.  The deviation is a property of the receiver and the satellite and is constant over
 * days, so a run that has learned it can hand it to the next run.  The store is a small JSON
 * file: a header that names what the values belong to, and one entry per (satellite, code
 * signal) with the value, its standard deviation and the epoch of the last update.
 *
 * ### What the header guards
 *
 * The values are the deviation of one receiver (type and firmware version) from one source of
 * published biases (the analysis-centre product).  A store is used only when the receiver type,
 * the receiver version and the bias source of the run all equal the header's
 * (@ref checkRxSatCodeBiasStore); any other store is ignored, and the ignoring is an event.  The
 * bias source is the product series of the .BIA file name (@ref biasSourceIdentity), so a
 * store written on one day is used on the next day's file of the same series.
 *
 * ### What an entry means
 *
 * The values of one constellation and signal are written with zero mean: the common part of
 * the deviation over the satellites is the receiver code bias of the signal, a datum that the
 * rest of the model owns, not information (@ref buildRxSatCodeBiasStore in
 * filter/RxSatCodeBiasLearning.hpp).  When a state is born from an entry, its variance is the
 * entry's variance grown by the random walk over the age of the entry
 * (@ref storedRxSatCodeBiasPrior).
 *
 * Plain value types and free functions; the file format is versioned by
 * @ref kRxSatCodeBiasStoreSchema.
 */

#include "math/TimeUtils.hpp"   // GnssTime

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace GnssProcessor {

/// The schema number the store is written with and the only one it is read with.
inline constexpr int kRxSatCodeBiasStoreSchema = 1;

/// The marker string of a store file (its "marker" member): tells the file from any other JSON.
inline constexpr const char* kRxSatCodeBiasStoreMarker = "gnss_processor.rx_sat_code_bias_store";

/// The smallest standard deviation (m) an entry is written with: a state born from an entry
/// of zero sigma would have a zero variance and take no data.
inline constexpr double kRxSatCodeBiasMinStoredSigmaM = 1.0e-3;

/**
 * @brief What a set of stored values belongs to.
 *
 * The receiver and bias-source texts are compared as written, after the trimming of
 * @ref normalizeIdentityText.
 */
struct RxSatCodeBiasIdentity {
    std::string receiver_type;      ///< RINEX "REC # / TYPE / VERS" type field ("TRIMBLE ALLOY"); empty when the header has none.
    std::string receiver_version;   ///< The version (firmware) field of the same record; empty when the header has none.
    std::string bias_source;        ///< The product series of the published biases (@ref biasSourceIdentity); "none" without a .BIA file.
};

/**
 * @brief One stored deviation: a satellite, a raw code signal and its value.
 */
struct RxSatCodeBiasEntry {
    std::string satellite;   ///< RINEX satellite id ("G05").
    std::string signal;      ///< Raw code observation code ("C1W").
    double      value_m{ 0.0 };   ///< The deviation (m), zero mean over the entries of its constellation and signal.
    double      sigma_m{ 0.0 };   ///< Its standard deviation (m).
    GnssTime    updated{};        ///< The epoch of the last update (the end of the run that wrote it).
};

/**
 * @brief The content of a store file: the identity it belongs to and its entries.
 *
 * The entries are keyed by the state key "<satellite>|<code signal>"
 * (@c StateKeys::makeRxSatCodeBiasKey), so the file lists them in key order.
 */
struct RxSatCodeBiasStore {
    RxSatCodeBiasIdentity identity;                       ///< What the entries belong to.
    std::map<std::string, RxSatCodeBiasEntry> entries;    ///< The entries by state key.
};

/**
 * @brief The prior a state is born with from an entry.
 */
struct RxSatCodeBiasPrior {
    double value_m{ 0.0 };        ///< The stored value (m).
    double variance_m2{ 0.0 };    ///< The stored variance grown over the entry's age (m²).
};

/**
 * @brief How a store compares with the run.
 */
enum class RxSatCodeBiasVerdict {
    Usable,                ///< The identity equals the run's: the entries are used.
    ReceiverTypeDiffers,   ///< Another receiver type: ignored.
    ReceiverVersionDiffers,///< Another receiver version: ignored.
    BiasSourceDiffers,     ///< Another source of published biases: ignored.
};

/**
 * @brief Trim the blanks around an identity text (a RINEX field is padded with blanks).
 * @param text The text.
 * @return The text without leading and trailing blanks and tabs.
 */
std::string normalizeIdentityText(const std::string& text);

/**
 * @brief The product series of a bias file name, the identity of the source of published biases.
 *
 * For a file named in the long IGS convention ("COD0MGXFIN_20241450000_01D_01D_OSB.BIA": a
 * ten-character product id, an underscore, an eleven-digit start epoch, ...), the series is the
 * text before the first underscore ("COD0MGXFIN"), so the daily files of one product share it.
 * Any other name is its own series (the file name as written).  An empty name gives "none".
 *
 * @param bia_file_name The file name of the .BIA product (no directory).
 * @return The series text.
 */
std::string biasSourceIdentity(const std::string& bia_file_name);

/**
 * @brief Compare a store's identity with the run's.
 * @param store The store.
 * @param run   The run's identity.
 * @return @c Usable when all three fields are equal, else the first field that differs
 *         (receiver type, receiver version, bias source).
 */
RxSatCodeBiasVerdict checkRxSatCodeBiasStore(const RxSatCodeBiasStore& store,
                                             const RxSatCodeBiasIdentity& run);

/**
 * @brief The text of a verdict for a log line.
 * @param verdict The verdict.
 * @return "usable", "receiver type differs", "receiver version differs" or "bias source differs".
 */
const char* rxSatCodeBiasVerdictText(RxSatCodeBiasVerdict verdict);

/**
 * @brief The variance of an entry grown by the random walk over its age.
 *
 * sigma² + q × age, with the age clamped at zero (an entry from the future is not shrunk).
 *
 * @param sigma_m The entry's standard deviation (m).
 * @param q_m2_per_s The random-walk spectral density (m²/s), @c filter.process_noise.rx_sat_code_bias_q.
 * @param age_s   Seconds from the entry's epoch to the birth of the state.
 * @return The variance (m²).
 */
double ageInflatedVariance(double sigma_m, double q_m2_per_s, double age_s);

/**
 * @brief Seconds from an earlier epoch to a later one (negative when the order is reversed).
 * @param later   The later epoch.
 * @param earlier The earlier epoch.
 * @return The difference (s), across GPS-week boundaries.
 */
double secondsBetween(const GnssTime& later, const GnssTime& earlier);

/**
 * @brief The prior a state is born with from the store, if the store has an entry for its key.
 *
 * Value: the entry's value.  Variance: the entry's sigma squared grown by @p q_m2_per_s over the
 * time from the entry's epoch to @p now.
 *
 * @param store      The store (already checked usable).
 * @param key        State key "<satellite>|<code signal>".
 * @param now        The epoch the state is born at.
 * @param q_m2_per_s The random-walk spectral density (m²/s).
 * @return The prior, or nothing when the store has no entry for the key.
 */
std::optional<RxSatCodeBiasPrior> storedRxSatCodeBiasPrior(const RxSatCodeBiasStore& store,
                                                           const std::string& key,
                                                           const GnssTime& now, double q_m2_per_s);

/**
 * @brief Read a store file.
 *
 * Refuses a file that is not JSON, has another marker or schema, or whose entries are
 * malformed; an entry whose key is not a receiver-satellite code-bias key is refused with the
 * file.  The reason is returned in @p why.
 *
 * @param path The file.
 * @param why  Out, optional: the reason when nothing is returned.
 * @return The store, or nothing when the file does not exist or cannot be used.
 */
std::optional<RxSatCodeBiasStore> readRxSatCodeBiasStore(const std::filesystem::path& path,
                                                         std::string* why = nullptr);

/**
 * @brief Write a store file.
 *
 * The file is written beside its final name and renamed over it, so a failed write leaves the
 * previous file intact.  The parent directory is created when missing.
 *
 * @param path  The file.
 * @param store The store.
 * @return True when the file was written.
 */
bool writeRxSatCodeBiasStore(const std::filesystem::path& path, const RxSatCodeBiasStore& store);

} // namespace GnssProcessor
