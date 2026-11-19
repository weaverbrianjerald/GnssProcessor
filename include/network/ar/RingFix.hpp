#pragma once
/**
 * @file RingFix.hpp
 * @brief Integer fixing in the network, ring by ring outward from the eq34 chain's root
 *        (@c network.ring_fix.*, default off).
 *
 * ## What it does
 *
 * The eq34 chain pins the ambiguities of a spanning tree per band at integer targets and the
 * root receiver's phase bias, so every other link (a chord) has, in the chain's frame, an
 * absolute ambiguity equal to its wavelength times an integer.  This mechanism decides those
 * integers for chords of a pair family (band 1 and band 2 of a constellation), holds them in
 * the network's own solve, and releases them at every event that can invalidate them:
 *
 *  1. **Decide** (after the solve of epoch t, @ref networkRingFixDecide): a chord is a candidate
 *     ring by ring outward from the root (@ref ringFixRings); its two-band float passes the
 *     gate (@ref ringFixGate: the wide lane rounded first, the ionosphere-free row with the wide
 *     lane rounded, formal sigma and fraction bounds), the damage gate (an event window, the
 *     lattice veto) and a persistence count (@ref ringFixDecideStep).
 *  2. **Hold** (from epoch t + 1, in PASS B, @ref networkRingFixPriors): one diagonal prior row
 *     per band at lambda times the integer, the row shape of the simulation-only oracle pin.
 *  3. **Release** (before PASS B, @ref networkRingFixHoldEpoch and @ref ringFixReleaseStep): at a
 *     slip latch, a refloat by another mechanism, a gap beyond the leveling horizon, a chain event
 *     that touches the link, or the residual monitor; the carried entries are re-opened by the
 *     vehicle's drop (or erased on the leveling vehicle), so a released link is never held at a
 *     stale level.
 *  4. **Audit** (simulation only, @c network.ring_fix.audit_frame_file): every held integer at
 *     every epoch against a frame-target file.
 *
 * Both edges of a hold are events with identities (standing invariant 7): a @c RINGFIX @c hold
 * row at the decision and a @c RINGFIX @c release row with its cause, in network_events.csv; the
 * per-epoch trace is network_ring_fix.csv and the run-end line counts every decision, release,
 * refusal and audit verdict.
 *
 * ## Off path
 *
 * Without @c network.ring_fix.enable the run state (@c NetworkCarryState::ring_fix) is null and
 * every call site tests it first, so no branch below is reachable and the output is
 * byte-identical.
 *
 * The rule's numbers are constants of this file, frozen by the card that pre-registered the
 * mechanism before any graded run; they are not keys.
 */

#include "network/state/Entities.hpp"   // net::ArcTable
#include "network/diag/BoundedTally.hpp"   // BoundedTally (the names behind the run-end counts)

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct GapResumption;   // network/slip/RawObservables.hpp (ringFixGatherKeepEvidence)

// ---- The frozen rule --------------------------------------------------------------------------

/// The gate's bound on the formal sigma of the ionosphere-free N1 (cycles).
inline constexpr double kRingFixMaxSigmaCyc = 0.10;
/// The gate's bound on the fraction of the ionosphere-free N1 (cycles).
inline constexpr double kRingFixMaxFracCyc = 0.20;
/// The damage gate's event window (s): no decision on a link with an event inside it.
inline constexpr double kRingFixWindowS = 600.0;
/// A pause longer than this between two epochs at which a link is a chord with a float restarts
/// its continuity count (s).
inline constexpr double kRingFixGapS = 45.0;
/// Epochs at which a candidate must pass the gate with the same integers before it is decided.
inline constexpr int kRingFixPersistEpochs = 10;
/// The hold row's sigma on each band's ambiguity column (m): the oracle pin's row.
inline constexpr double kRingFixHoldSigmaM = 1.0e-4;
/// The monitor's bound on the held link's post-fit ionosphere-free phase residual (m).
inline constexpr double kRingFixMonitorBoundM = 0.03;
/// Consecutive epochs above the monitor's bound that release a held link.
inline constexpr int kRingFixMonitorEpochs = 4;
/// A released link's carried variance is re-opened to at least this (m^2): a link held from its first
/// stash has the hold's own variance as its birth variance, which a drop would restore.
inline constexpr double kRingFixReleaseFloorM2 = 1.0;
/// Change O: at most this many links are decided per epoch, the most certain first.
inline constexpr int kRingFixMaxDecisionsPerEpoch = 3;
/// Rule "largest" of the settled flags (network.ring_fix.settled_rule): the fewest satellites a flagged set holds.
inline constexpr int kRingFixSettledMinSats = 4;
/// Rule "largest": another set replaces the incumbent once it holds this many more satellites.
inline constexpr int kRingFixSettledHysteresis = 2;
/// The re-attachment of untied satellites (network.ring_fix.reattach): how long a satellite of a family's tree stays
/// unsettled and without a tie before its tree edge may move (s; card NRF section 102.2).
inline constexpr double kRingFixReattachDwellS = 1800.0;
/// The re-attachment: how long after a move its satellite is not moved again (s), longer than its links' window and
/// persistence after the re-host (section 102.2).
inline constexpr double kRingFixReattachHoldoffS = 3600.0;
/// The re-attachment: the smallest common offset of the candidates that moves a satellite, ionosphere-free (cycles);
/// below it and below the wide lane's the links are converging onto the satellite's level (section 102.2).
inline constexpr double kRingFixReattachMinOffsetIfCyc = 0.10;
/// The re-attachment: the smallest common offset of the candidates that moves a satellite, wide lane (cycles).
inline constexpr double kRingFixReattachMinOffsetWlCyc = 0.15;
/// The re-attachment: the fewest agreeing candidate links of the flagged piece.
inline constexpr int kRingFixReattachMinLinks = 3;
/// The re-attachment: the candidates' largest circular standard deviation of the ionosphere-free fraction (cycles).
inline constexpr double kRingFixReattachMaxSdIfCyc = 0.08;
/// The re-attachment: the candidates' largest circular standard deviation of the wide-lane fraction (cycles).
inline constexpr double kRingFixReattachMaxSdWlCyc = 0.10;
/// The re-attachment: how long after a move its satellite is watched for its flag (s).
inline constexpr double kRingFixReattachWatchS = 1800.0;
/// The cascade (network.ring_fix.cascade): the largest fraction of the reference-less-third float difference decided.
inline constexpr double kRingFixCascadeMaxFracCyc = 0.10;
/// Change L: the bound on the log of the likelihood ratio between the nearest and the next integer,
/// ln(1e6), for N1 and for the wide lane.
inline constexpr double kRingFixMinLogOdds = 13.815510557964274;

/// The order of the decisions (@c network.ring_fix.order).
enum class RingFixOrder : std::uint8_t {
    Ring,     ///< Ring by ring: a link waits until its inner vertex's links towards the root are held.
    Certain,  ///< Change O: every chord with a float is a candidate; the most certain decided first, a few per epoch.
};

/// The gate of the decisions (@c network.ring_fix.gate).
enum class RingFixGateRule : std::uint8_t {
    Sigma,  ///< The frozen gate: the formal sigma and the fraction of the ionosphere-free N1.
    Odds,   ///< Change L: the log-odds of N1 and of the wide lane's own float, and the fraction of N1.
};

/// The decision rule of a run (@c network.ring_fix.rule).
enum class RingFixRule : std::uint8_t {
    Gated,  ///< The frozen rule: the gate, the damage gate and the persistence count.
    Plain,  ///< The negative control: every candidate with a float is rounded and held at once.
    Log,    ///< Log only: the frozen rule is evaluated and traced, nothing is held (the products are unchanged).
};

/// Why a held link was released.
enum class RingFixRelease : std::uint8_t {
    Slip,     ///< Either band in the epoch's free-epoch latch (any slip detector or re-entry response).
    Refloat,  ///< Either band's carried entry dropped or erased by another mechanism (RAIM, gap cliff, screen).
    Gap,      ///< The link absent, or no chord of its pair family, for longer than the leveling horizon.
    Chain,    ///< The link became a tree link, lies on a cut release's moved side, or its component's root changed.
    Monitor,  ///< The post-fit ionosphere-free phase residual above its bound at consecutive epochs.
};
/// Number of release causes.
inline constexpr std::size_t kRingFixReleaseCount = 5;

/// Why a link that is not held was not decided at an epoch (the first test it failed).
enum class RingFixRefuse : std::uint8_t {
    NotCandidate,  ///< Its ring's turn has not come: a link of its inner vertex towards the root is not held.
    NoFloat,       ///< No usable two-band float or covariance this epoch.
    Window,        ///< An event inside the window (re-level, latch, gap, tree link, release) or a missing carry.
    Sigma,         ///< The formal sigma of the ionosphere-free N1 above its bound.
    Fraction,      ///< The fraction of the ionosphere-free N1 above its bound.
    Lattice,       ///< The producer's lattice of fixed cycles implies another integer on a band.
    Persist,       ///< Passed this epoch, the persistence count not yet reached.
    Odds,          ///< Change L: the log-odds of N1 or of the wide lane below the bound.
    Cap,           ///< Change O: passed every test, passed over by the per-epoch cap (the count is kept).
    WlFraction,    ///< Passed every other test (persistence included); the wide lane's fraction above network.ring_fix.wl_frac_max.
    CodeEvent,     ///< Its satellite's code reference moved or its code datum re-seeded within network.ring_fix.code_event_window_s.
};
/// Number of refusal causes.
inline constexpr std::size_t kRingFixRefuseCount = 11;

/**
 * @brief The name of a decision rule ("gated", "plain", "log").
 * @param r The rule.
 * @return Its name as a static string; "?" for a value outside the enum.
 */
const char* ringFixRuleName(RingFixRule r);

/**
 * @brief The name of a release cause ("slip", "refloat", "gap", "chain", "monitor").
 * @param c The cause.
 * @return Its name as a static string; "?" for a value outside the enum.
 */
const char* ringFixReleaseName(RingFixRelease c);

/**
 * @brief The name of a refusal cause ("not_candidate", "no_float", "window", "sigma",
 *        "fraction", "lattice", "persist", "odds", "cap", "wl_fraction").
 * @param c The cause.
 * @return Its name as a static string; "?" for a value outside the enum.
 */
const char* ringFixRefuseName(RingFixRefuse c);

/// Why a hold-table hit in PASS B was not folded as a hold row (card CNN row 4; the first test it failed).
enum class RingFixRowSkip : std::uint8_t {
    TreeEdge,        ///< The arc is a tree edge of the chain: its 1e10 pin is the frame, a second row would fight it.
    IndexPastState,  ///< The arc's column index lies past this epoch's linearization point x_lin.
    NoLocalColumn,   ///< The column is not one of the station's columns this epoch (h2local), or its x_lin is non-finite.
    NonFiniteRow,    ///< The row's right-hand side is non-finite.
};
/// Number of hold-row skip causes.
inline constexpr std::size_t kRingFixRowSkipCount = 4;

/**
 * @brief The name of a hold-row skip cause, as the run-end line prints it.
 * @param c The cause.
 * @return "on a tree edge", "index past the state", "not a column of the epoch" or "a non-finite row"; "?" outside
 *         the enum.
 */
const char* ringFixRowSkipName(RingFixRowSkip c);

// ---- The gate ---------------------------------------------------------------------------------

/// The gate's reading of one two-band float.
struct RingFixGate {
    bool      valid{false};    ///< The inputs were finite and the covariance usable.
    long long wl{0};           ///< The rounded wide lane, round(f1 - f2) (cycles).
    long long n1{0};           ///< Band 1's integer, round of the ionosphere-free row with the wide lane rounded.
    long long n2{0};           ///< Band 2's integer, n1 - wl.
    double    n_if{0.0};       ///< The ionosphere-free row with the wide lane rounded (cycles of band 1).
    double    sigma_if{0.0};   ///< Its formal sigma (cycles).
    double    frac_if{0.0};    ///< |n_if - n1| (cycles).
    double    sigma_wl{0.0};   ///< The wide lane's formal sigma (cycles).
    double    frac_wl{0.0};    ///< |(f1 - f2) - wl| (cycles).
    bool      pass{false};     ///< sigma_if and frac_if within the frozen bounds.
    double    log_odds_if{0.0};  ///< ln of the likelihood ratio of N1's nearest and next integer, (1 - 2 frac_if) / (2 sigma_if^2).
    double    log_odds_wl{0.0};  ///< The same for the wide lane's own float, (1 - 2 frac_wl) / (2 sigma_wl^2).
    bool      pass_odds{false};  ///< Change L: frac_if within its bound and both log-odds at least kRingFixMinLogOdds.
};

/**
 * @brief The gate on one link's two-band float: the wide lane rounded first, then band 1's
 *        integer from the ionosphere-free row with the wide lane rounded.
 *
 * With r = lambda2 / lambda1 (= F1 / F2), the row is n = (r f1 - f2 - z) / (r - 1) with
 * z = round(f1 - f2); n1 = round(n), n2 = n1 - z.  Its formal variance is
 * a^2 q11 + 2 a b q12 + b^2 q22 with a = r / (r - 1), b = -1 / (r - 1).  The link passes when the
 * sigma is at most @ref kRingFixMaxSigmaCyc and |n - n1| at most @ref kRingFixMaxFracCyc.
 *
 * @param f1      Band-1 float (cycles).
 * @param f2      Band-2 float (cycles).
 * @param q11     Band-1 variance (cycles^2).
 * @param q12     Band-1, band-2 covariance (cycles^2).
 * @param q22     Band-2 variance (cycles^2).
 * @param lambda1 Band-1 wavelength (m).
 * @param lambda2 Band-2 wavelength (m), longer than band 1's.
 * @param sigma_scale A factor on both formal sigmas in the two log-odds (network.ring_fix.sigma_scale;
 *        1, the default, uses the formal sigmas).  @c sigma_if, @c sigma_wl and @c pass keep the
 *        formal sigmas.
 * @return The reading; @c valid false for non-finite inputs, a negative variance of the row or
 *         wavelengths that do not order.
 */
RingFixGate ringFixGate(double f1, double f2, double q11, double q12, double q22,
                        double lambda1, double lambda2, double sigma_scale = 1.0);

/**
 * @brief Whether a satellite's code event still closes its links to decisions
 *        (network.ring_fix.code_event_window_s).
 * @param code_event_s The satellite's last code-reference move or code-datum re-seed (s; negative: none).
 * @param window_s     The window (s; 0: none).
 * @param now_s        This epoch (s).
 * @return true while the event is inside the window.
 */
bool ringFixCodeEventOpen(double code_event_s, double window_s, double now_s);

/**
 * @brief The satellites of one held family that are settled: reached from their tree's datum receiver through
 *        held links and tree edges that entered the tree as held links (network.ring_fix.settled_flags).
 *
 * The links form a graph over receivers and satellites; a satellite is settled when it lies in one component
 * with its own datum receiver.  The components are the arc graph's disjoint sets (@ref arcgraph::Dsu).
 *
 * @param links       The family's certified links, (station index, satellite): its held links and its tree
 *                    edges that entered as held links.
 * @param root_of_sat Every satellite of the family's tree -> its tree's datum receiver (station index).
 * @return The settled satellites (a subset of the keys of @p root_of_sat).
 */
std::set<std::string> ringFixSettledSats(const std::vector<std::pair<int, std::string>>& links,
                                         const std::map<std::string, int>& root_of_sat);

/// One candidate link of an untied satellite for the re-attachment (network.ring_fix.reattach).
struct RingFixReattachCand {
    int    station{-1};   ///< The link's station (roster index).
    double frac_if{0.0};  ///< Its ionosphere-free fraction in the chain frame, n_if - n1 (cycles of band 1), in [-0.5, 0.5].
    double frac_wl{0.0};  ///< Its wide-lane fraction, (f1 - f2) - round(f1 - f2) (cycles), in [-0.5, 0.5].
};

/// The re-attachment's reading of one untied satellite's candidates (@ref ringFixReattachChoice).
struct RingFixReattachPick {
    bool   ok{false};      ///< Enough candidates agree on a common offset large enough: a move is chosen.
    bool   converging{false};  ///< They agree but their common offset is below both minimums: no move.
    int    index{-1};      ///< The chosen candidate, an index into the candidates (-1 unless @ref ok).
    int    n{0};           ///< The candidates read.
    double med_if{0.0};    ///< Their circular median of the ionosphere-free fraction (cycles).
    double med_wl{0.0};    ///< Their circular median of the wide-lane fraction (cycles).
    double sd_if{0.0};     ///< Their circular standard deviation of the ionosphere-free fraction (cycles).
    double sd_wl{0.0};     ///< Their circular standard deviation of the wide-lane fraction (cycles).
};

/**
 * @brief The re-attachment's choice for one untied satellite (network.ring_fix.reattach): whether its candidate links
 *        agree, and which of them carries the satellite.
 *
 * The fractions are angles on the circle of one cycle.  For each direction the circular mean m and the circular
 * standard deviation sqrt(-2 ln R) / (2 pi) are formed (R the mean resultant length); the median is the median of
 * the fractions unwrapped about m, plus m.  The candidates agree when there are at least
 * @ref kRingFixReattachMinLinks of them and both spreads are within @ref kRingFixReattachMaxSdIfCyc and
 * @ref kRingFixReattachMaxSdWlCyc.  Agreeing candidates move the satellite only when their common offset is an
 * offset, not a convergence: |median| at least @ref kRingFixReattachMinOffsetIfCyc ionosphere-free or
 * @ref kRingFixReattachMinOffsetWlCyc wide lane (else @c converging).  The chosen candidate is the one with the
 * smallest |d_if| / kRingFixReattachMaxSdIfCyc + |d_wl| / kRingFixReattachMaxSdWlCyc, d its wrapped distance to the
 * medians, ties by the lower index.  The median, so that one receiver's code does not set the wide lane for all.
 *
 * @param cands The candidates (their order breaks ties).
 * @return The reading; @c ok false with the spreads and medians filled when the candidates are too few, disagree or
 *         converge.
 */
RingFixReattachPick ringFixReattachChoice(const std::vector<RingFixReattachCand>& cands);

/// The choice of rule "largest" of the settled flags at one epoch (@ref ringFixLargestSettledSet).
struct RingFixLargestSet {
    std::set<std::string> sats;    ///< The flagged satellites (empty: none flagged).
    bool      switched{false};     ///< Another component replaced a non-empty incumbent: a change of identity.
    long long side_changes{0};     ///< At a change of identity, the satellites in one of the two sets only.
};

/// How a link came to be held (ringFixHoldLink): decided by the gates, held at the truth in the oracle phase, or
/// inherited from the previous epoch's tree edge.  The kinds differ in their stamps, their re-base flag and their
/// counters, nowhere else.
enum class RingFixHoldKind : std::uint8_t { Decided, Oracle, Inherited };

/// How a held link stops being held (ringFixUnholdLink): a release of the release step, a release across an unknown
/// cut, or a link taken into the chain's tree (not a release: the chain pins it).  The kinds differ in what they reset
/// beyond the common fields; each path keeps the resets it had.
enum class RingFixUnholdKind : std::uint8_t { Release, CutRelease, ToTree };

/// One cascade of network.ring_fix.cascade: a held pair and the third signal it carries to.
struct RingFixCascade {
    char        letter{0};   ///< The constellation's RINEX letter.
    std::string ck;          ///< Its constellation key, as the pairs name it ("G").
    std::string s1, s2;      ///< The held pair's two signals.
    std::string s3;          ///< The third signal.
    std::string ref;         ///< The pair's signal the third is decided against (@ref ringFixCascadeReference).
    std::string name;        ///< "<c>:<s1>/<s2>><s3>".
};

/// The cascade's decision reading of one link (@ref ringFixCascadeGate).
struct RingFixCascadeGate {
    bool      valid{false};    ///< The inputs were finite and the variance not negative.
    long long d{0};            ///< The reference less the third, rounded (cycles).
    double    frac{0.0};       ///< |difference - d| (cycles).
    double    sigma{0.0};      ///< The difference's formal sigma (cycles), unscaled.
    double    log_odds{0.0};   ///< ln of the likelihood ratio of d and the next integer at the scaled sigma.
    bool      pass{false};     ///< frac within kRingFixCascadeMaxFracCyc and the odds at kRingFixMinLogOdds or more.
};

/**
 * @brief The cascade's gate: the float difference of the reference and the third signal, f_ref - f_3, whose rounding
 *        is the difference of their integers in the chain's frame (each band's potentials are its root's plus whole
 *        numbers).  The reference's float sits at its held integer, so the difference carries the third band's
 *        post-hold float and its extra-wide-lane (or same-frequency) precision.
 * @param f_ref       The reference's float (cycles).
 * @param f3          The third signal's float (cycles).
 * @param q11         The reference's conditional variance (cycles^2).
 * @param q12         Their conditional covariance (cycles^2).
 * @param q22         The third's conditional variance (cycles^2).
 * @param sigma_scale The factor on the formal sigma of the odds (network.ring_fix.sigma_scale).
 * @return The reading; @c pass decides.
 */
RingFixCascadeGate ringFixCascadeGate(double f_ref, double f3, double q11, double q12, double q22, double sigma_scale);

/**
 * @brief The pair's signal a cascade decides its third signal against: the one on the third's band (a same-frequency
 *        sibling, GPS L1W from L1C), else the one nearest in frequency (GPS L5 from L2, Galileo E5b from E5a).
 * @param sat A satellite of the constellation (its letter selects the frequencies).
 * @param s1  The pair's first signal.
 * @param s2  The pair's second signal.
 * @param s3  The third signal.
 * @return @p s1 or @p s2; empty when a wavelength is unknown.
 */
std::string ringFixCascadeReference(const std::string& sat, const std::string& s1, const std::string& s2,
                                    const std::string& s3);

/**
 * @brief The lead rank of one arc of a cascade's third band grown on its pair's forest (network.ring_fix.cascade_tree,
 *        arcgraph::ForestRules::lead_rank): the arcs on which the pair fixes both integers come first, so that the
 *        third band's tree paths close over arcs whose slant ionosphere the pair's integers fix.
 * @param pair_tree_edge  The arc is a tree edge of both bands of the pair at this election.
 * @param pair_held       The pair holds the arc's integers (a held link, its monitor not tripped, not freed).
 * @param third_incumbent The arc was a tree edge of the third band at the last election.
 * @param third_slipped   The arc slipped on the third band this epoch.
 * @param eligible_host   The arc is an eligible host on the third band (its ambiguity has converged).
 * @return 1 for a tree edge of the pair, 2 for a held link that is an incumbent of the third band, 3 for another held
 *         link; 0 (no lead) for any other arc, for an arc slipped on the third band, and for an ineligible host that is
 *         no incumbent (a fresh arc is not pinned ahead of the walk's own rules).
 */
int ringFixCascadeLeadRank(bool pair_tree_edge, bool pair_held, bool third_incumbent, bool third_slipped,
                           bool eligible_host);

struct RingFixCascadeLink;

/**
 * @brief The third-signal integer a chain re-host of the third band onto a cascade link may keep
 *        (network.ring_fix.cascade_rehost_keep), as a re-host onto a pair's held link keeps the pair's integer.
 * @param link        The cascade link the new tree edge lies on.
 * @param ref_freed   The link's reference signal is in this epoch's free-epoch latch.
 * @param third_freed The link's third signal is in this epoch's free-epoch latch.
 * @return The held integer when the link is held, its monitor not tripped and neither signal freed; none otherwise.
 */
std::optional<long long> ringFixCascadeKeepInteger(const RingFixCascadeLink& link, bool ref_freed, bool third_freed);

/// One link of a cascade: the third signal of a link the pair holds.
struct RingFixCascadeLink {
    bool      held{false};          ///< Held now.
    long long n3{0};                ///< The held third-signal integer (cycles).
    long long n_ref{0};             ///< The reference's held integer at the decision.
    double    lambda3{0.0};         ///< The third signal's wavelength (m).
    int       streak{0};            ///< Consecutive epochs that passed with the same integer.
    long long streak_n3{0};         ///< The streak's integer.
    double    decided_s{-1.0};      ///< When it was decided (continuous GPS seconds; -1: never).
    double    stamp3{0.0};          ///< The third signal's carried re-level stamp at the decision.
    double    absent_since_s{-1.0}; ///< Held and absent since (-1: present).
    int       monitor_bad{0};       ///< Consecutive epochs with the third signal's residual above the monitor's bound.
    bool      monitor_trip{false};  ///< The monitor asks for a release at the next epoch.
    double    released_s{-1.0};     ///< Its last release (-1: never).
};

/// A cascade's run counters.
struct RingFixCascadeCounters {
    long long decided{0};              ///< Decisions.
    long long held_link_epochs{0};     ///< Link-epochs held in the solve.
    long long to_tree{0};              ///< Held links that became tree edges of the third band.
    long long refused_fraction{0};     ///< Link-epochs refused for the fraction.
    long long refused_odds{0};         ///< Link-epochs refused for the odds.
    long long refused_persist{0};      ///< Link-epochs that passed and wait for the persistence.
    long long refused_no_float{0};     ///< Link-epochs without a float.
    long long truth_right{0};          ///< Simulation: held link-epochs right against truth in the epoch's frame.
    long long truth_wrong{0};          ///< Simulation: held link-epochs wrong.
    long long decided_truth_wrong{0};  ///< Simulation: decisions wrong.
    std::map<std::string, long long> released;   ///< Releases by cause (pair, slip, cut, monitor, gap).
    // The third band's tree on the pair's forest (network.ring_fix.cascade_tree): its tree edge-epochs by how the walk
    // took them.
    long long tree_edges{0};           ///< Tree edge-epochs of the third band.
    long long tree_pair{0};            ///< ...taken as a tree edge of the pair (lead rank 1).
    long long tree_held{0};            ///< ...taken as a link the pair holds (lead ranks 2 and 3).
    long long tree_other{0};           ///< ...taken otherwise (an incumbent or a new edge of the walk).
    // The log mode (network.ring_fix.cascade_log).
    long long would_hold{0};           ///< Link-epochs that passed the gate for the persistence, traced, not held.
    long long would_truth_wrong{0};    ///< Simulation: of them, wrong against truth in the epoch's frame.
    // Re-hosts of the third band onto held cascade links (network.ring_fix.cascade_rehost_keep).
    long long rehost_kept{0};          ///< Kept at the held integer (the new host's carry on it), a step of 0.
    long long rehost_off_integer{0};   ///< Released: the new host's carry 1e-4 cycle or more off the held integer.
    long long rehost_not_kept{0};      ///< Released for another cause (no carry at the new host, an unknown draft step).
};

/**
 * @brief The wait on the settled flag's raise (network.ring_fix.settled_wait_emissions): a satellite in this epoch's
 *        set is flagged only from its (wait + 1)-th consecutive emission in the set.
 * @param in_set The family's set this epoch (the rule's choice).
 * @param run    [in/out] Satellite -> its consecutive emissions in the set; a satellite out of the set leaves it.
 * @param wait   Emissions in the set before the raise (0: the set itself).
 * @return The flagged satellites.
 */
std::set<std::string> ringFixSettledWait(const std::set<std::string>& in_set, std::map<std::string, int>& run,
                                         int wait);

/**
 * @brief Rule "largest" of the settled flags: the largest set of a family's satellites mutually tied through held
 *        links and certified tree edges, whatever receiver roots it, with a minimum and a hysteresis.
 *
 * The links form a graph over receivers and satellites, and the family's tree satellites fall into its components.
 * The incumbent (last epoch's flagged set) is followed into the component that holds most of it; that component is
 * flagged, with this epoch's membership, while it holds at least @p min_sats satellites and no other component holds
 * @p hysteresis more.  Otherwise the largest component is flagged when it holds at least @p min_sats, and none is
 * flagged below.  Ties go to the component whose lexically smallest satellite comes first.
 *
 * @param links      (station index, satellite) of every held link and certified tree edge of the family.
 * @param tree_sats  The family's satellites in its first band's tree.
 * @param incumbent  Last epoch's flagged set (empty at the start).
 * @param min_sats   The fewest satellites a flagged set holds.
 * @param hysteresis The lead another component needs to replace the incumbent.
 * @return The flagged set, whether it changed identity, and the satellites that changed side then.
 */
RingFixLargestSet ringFixLargestSettledSet(const std::vector<std::pair<int, std::string>>& links,
                                           const std::set<std::string>& tree_sats,
                                           const std::set<std::string>& incumbent, int min_sats, int hysteresis);

// ---- Rings and candidates ---------------------------------------------------------------------

/// A link's identity: the station (roster index), the satellite and its pair family's signals.
struct RingFixLinkId {
    int         station{-1};  ///< Roster index.
    std::string sat;          ///< Satellite ("G05").
    std::string sig1;         ///< Band-1 phase signal ("L1C").
    std::string sig2;         ///< Band-2 phase signal ("L2W").
    /// @brief Strict weak order (station, satellite, band 1, band 2).
    /// @param o The other identity.
    /// @return True when this identity sorts first.
    bool operator<(const RingFixLinkId& o) const {
        return std::tie(station, sat, sig1, sig2) < std::tie(o.station, o.sat, o.sig1, o.sig2);
    }
};

/**
 * @brief A link as a run-end line names it (card CNN rows 2 and 4): the station, the satellite, the signals and the
 *        ring at the epoch it was counted.  The station is a roster index, named at the run end.
 */
struct RingFixNamedLink {
    int         station{-1};  ///< Roster index.
    std::string sat;          ///< Satellite ("G09").
    std::string signals;      ///< The link's pair ("L1C/L2W"); for a hold row, its signal and pair ("L1C of L1C/L2W").
    int         ring{-1};     ///< Its ring when counted (-1: not in the rings).
    /** @brief Equality of every field.  @param o The other link.  @return True when equal. */
    bool operator==(const RingFixNamedLink& o) const {
        return station == o.station && sat == o.sat && signals == o.signals && ring == o.ring;
    }
};

/**
 * @brief A link as the run-end line prints it: "BOGT00COL-G09 L1C/L2W ring 2".
 * @param l The link.
 * @param station_names The roster's station names (a station outside them prints as "#<index>").
 * @return The text.
 */
std::string ringFixNamedLinkText(const RingFixNamedLink& l, const std::vector<std::string>& station_names);

/// One band's tree and chords, as the rings read them.  Vertices are keys: "R:<station index>"
/// for a receiver, "S:<satellite>" for a satellite (the chain record's keys).
struct RingFixTree {
    /// Tree edges: (child, parent, the child's depth below its component's root).
    std::vector<std::tuple<std::string, std::string, int>> edges;
    /// Chords: (receiver key, satellite key).
    std::vector<std::pair<std::string, std::string>> chords;
};

/// A chord's place in the rings.
struct RingFixRingInfo {
    int         ring{-1};          ///< The larger depth of its two vertices (-1: a vertex not in the tree).
    std::string inner;             ///< The vertex of smaller depth.
    bool        candidate{false};  ///< Every link of the inner vertex towards the root is held.
};

/**
 * @brief The rings of one band's chords and which of them are candidates.
 *
 * A vertex's depth is read from the tree edges (a child's depth is given, its parent's is one
 * less).  A chord's ring is the larger depth of its two vertices and its inner vertex the one of
 * smaller depth.  The chord is a candidate when every link of its inner vertex to a vertex of
 * smaller depth than the inner vertex is held: a tree link always is, a chord when it is in
 * @p held.  A ring-2 chord's inner vertex (a satellite at depth 1) reaches the root through its
 * tree link alone, so ring 2 is always a candidate.
 *
 * @param tree One band's tree edges and chords.
 * @param held The chords held now, as (receiver key, satellite key).
 * @return Every chord of @p tree with its ring, inner vertex and candidacy.
 */
std::map<std::pair<std::string, std::string>, RingFixRingInfo>
ringFixRings(const RingFixTree& tree, const std::set<std::pair<std::string, std::string>>& held);

// ---- The run state ------------------------------------------------------------------------------

/// One link's state across epochs.
struct RingFixLink {
    double    lambda1{0.0};          ///< Band-1 wavelength (m).
    double    lambda2{0.0};          ///< Band-2 wavelength (m).
    double    chord_since_s{-1.0};   ///< Start of the current run of epochs at which it is a chord with a float (-1: none).
    double    last_chord_s{-1.0};    ///< Last epoch at which it was a chord with a float (-1: never).
    int       streak{0};             ///< Consecutive candidate epochs that passed with the same integers.
    long long streak_n1{0};          ///< The streak's band-1 integer.
    long long streak_n2{0};          ///< The streak's band-2 integer.
    bool      held{false};           ///< Held now.
    long long n1{0};                 ///< The held band-1 integer (cycles).
    long long n2{0};                 ///< The held band-2 integer (cycles).
    int       ring{-1};              ///< The ring at the decision.
    double    decided_s{-1.0};       ///< When it was decided (continuous GPS seconds; -1: never).
    double    stamp1{0.0};           ///< Band 1's carried re-level stamp when it was decided.
    double    stamp2{0.0};           ///< Band 2's carried re-level stamp when it was decided.
    std::string root;                ///< Its component's root receiver when it was decided.
    double    absent_since_s{-1.0};  ///< Held and absent since (-1: present).
    int       monitor_bad{0};        ///< Consecutive epochs above the monitor's bound.
    bool      monitor_trip{false};   ///< The monitor asks for a release at the next epoch.
    double    released_s{-1.0};      ///< Its last release (-1: never).
    bool      wrong_now{false};      ///< The audit found its held integers wrong at the last epoch.
    bool      rebase_stamps{false};  ///< A cut release erased its carried entries: take the next stamps as the baseline.
    bool      inherited{false};      ///< Held as an old host of a cut (not decided by the rule).
    bool      oracle{false};         ///< Held by the oracle phase.
    bool      log_decided{false};    ///< Log rule: the rule would have decided it (at least once).
    long long log_n1{0};             ///< Log rule: the first would-be band-1 integer.
    long long log_n2{0};             ///< Log rule: the first would-be band-2 integer.
    // The near-null guard (network.ring_fix.nn_guard).
    int       nn_n{0};               ///< Code-residual epochs in the moving average since the hold or the guard's last move.
    double    nn_gf_m{0.0};          ///< Moving average of the geometry-free combination of its two post-fit code residuals (m).
    double    nn_if_m{0.0};          ///< Moving average of their ionosphere-free combination (m).
    bool      nn_moved{false};       ///< The guard moved its pair during this hold.
};

/// One link as the decision reads it after the solve.
struct RingFixDecideObs {
    RingFixLinkId id;                  ///< The link.
    double    lambda1{0.0};            ///< Band-1 wavelength (m).
    double    lambda2{0.0};            ///< Band-2 wavelength (m).
    bool      have_float{false};       ///< A usable two-band float and covariance this epoch.
    double    f1{0.0};                 ///< Band-1 float (cycles).
    double    f2{0.0};                 ///< Band-2 float (cycles).
    double    q11{0.0};                ///< Band-1 variance (cycles^2).
    double    q12{0.0};                ///< Covariance (cycles^2).
    double    q22{0.0};                ///< Band-2 variance (cycles^2).
    int       ring{-1};                ///< Its ring this epoch.
    bool      candidate{false};        ///< Its ring's turn has come.
    std::string root;                  ///< Its component's root receiver this epoch.
    bool      have_lat1{false};        ///< The lattice implies band 1's integer.
    bool      have_lat2{false};        ///< The lattice implies band 2's integer.
    long long lat1{0};                 ///< That integer (band 1).
    long long lat2{0};                 ///< That integer (band 2).
    bool      carry_ok{false};         ///< Both bands' carried entries exist.
    double    relevel1_s{0.0};         ///< Band 1's carried re-level stamp (0: unstamped).
    double    relevel2_s{0.0};         ///< Band 2's carried re-level stamp (0: unstamped).
    bool      latched{false};          ///< Either band in this epoch's free-epoch latch.
    double    code_event_s{-1.0};      ///< Its satellite's last code-reference move or code-datum re-seed (s; negative: none).
    bool      have_resid{false};       ///< Held links: the post-fit ionosphere-free phase residual was formed.
    double    resid_if_m{0.0};         ///< That residual (m).
    bool      have_code{false};        ///< Held links under network.ring_fix.nn_guard: both bands' post-fit code residuals were formed.
    double    code_gf_m{0.0};          ///< Their geometry-free combination, band 1 minus band 2 (m).
    double    code_if_m{0.0};          ///< Their ionosphere-free combination (m).
    bool      have_truth{false};       ///< Simulation only: the link's true integers in this epoch's frame are known.
    long long truth1{0};               ///< Its true band-1 integer.
    long long truth2{0};               ///< Its true band-2 integer.
};

/// One link as the release reads it before PASS B.
struct RingFixHoldObs {
    bool        present{false};   ///< A chord of both bands' trees this epoch.
    bool        tree_now{false};  ///< A tree link in either band this epoch.
    bool        in_cut{false};    ///< A vertex on the moved side of one of this epoch's cut releases.
    std::string root;             ///< Its component's root receiver this epoch ("" when not found).
    bool        latched{false};   ///< Either band in this epoch's free-epoch latch.
    bool        carry_ok{false};  ///< Both bands' carried entries exist.
    double      stamp1{0.0};      ///< Band 1's carried re-level stamp now.
    double      stamp2{0.0};      ///< Band 2's carried re-level stamp now.
};

/// What a step did to one link.
struct RingFixEvent {
    enum class Kind : std::uint8_t {
        Hold,     ///< Decided: held from the next epoch.
        Release,  ///< Released, with @ref release.
        Refuse,   ///< Not decided this epoch, with @ref refuse.
    };
    Kind           kind{Kind::Refuse};              ///< What happened.
    RingFixLinkId  id;                              ///< The link.
    RingFixRelease release{RingFixRelease::Slip};   ///< Release: the cause.
    RingFixRefuse  refuse{RingFixRefuse::NotCandidate};  ///< Refuse: the cause.
    RingFixGate    gate;                            ///< The gate's reading (Hold, and Refuse when formed).
    bool           inherited{false};                ///< Hold: an old host of a cut, held at its moved target.
    bool           oracle{false};                   ///< Hold: decided by the oracle phase.
    bool           log_only{false};                 ///< Hold under the log rule: a would-be decision, nothing held.
};

/// The run counters (the run-end line).
struct RingFixCounters {
    long long epochs_decided{0};          ///< Epochs the decision ran.
    long long decided{0};                 ///< Links decided.
    long long decided_by_ring[8]{};       ///< Of them, by ring (index 7: ring 7 or more).
    long long held_link_epochs{0};        ///< Held links present at an epoch's decision.
    long long released[kRingFixReleaseCount]{};  ///< Releases by cause.
    long long refused[kRingFixRefuseCount]{};    ///< Link-epochs refused, by cause.
    long long rows_offered{0};            ///< Hold-table hits in PASS B: rows_injected + rows_gauge_skipped + the three without a column.
    long long rows_injected{0};           ///< Hold rows folded in PASS B.
    long long rows_gauge_skipped{0};      ///< Table hits on a gauge-pinned (tree) arc, never folded.
    long long rows_no_column_index{0};    ///< Table hits without a column: the column index past this epoch's x_lin.
    long long rows_no_column_local{0};    ///< Table hits without a column: not a column of the station this epoch, or a non-finite x_lin.
    long long rows_no_column_eta{0};      ///< Table hits without a column: the row's right-hand side non-finite.
    long long carry_reopened{0};          ///< Carried entries the release re-opened (drop or erase).
    long long audit_right{0};             ///< Held link-epochs whose integers the audit found right.
    long long audit_wrong{0};             ///< Held link-epochs whose integers the audit found wrong.
    long long audit_unlisted{0};          ///< Held link-epochs the audit file does not list.
    long long decided_wrong{0};           ///< Decisions the audit found wrong at the decision.
    long long decided_right{0};           ///< Decisions the audit found right at the decision.
    double    first_hold_s[8]{-1, -1, -1, -1, -1, -1, -1, -1};  ///< First decision by ring (continuous GPS seconds; -1: none).
    // Stage N2.
    long long cuts{0};                    ///< Cut releases seen (per band).
    long long cuts_known{0};              ///< Of them, with a known step.
    long long cuts_known_zero{0};         ///< Of them, with a step of 0 (the frame kept).
    long long reexpressed{0};             ///< Held links re-expressed by a known step.
    long long cut_released{0};            ///< Held links released for crossing a cut with an unknown step (in released[Chain] too).
    long long to_tree{0};                 ///< Held links that became tree edges (left the held set, the chain pins them).
    long long inherited{0};               ///< Old hosts held at their moved target.
    long long root_kept{0};               ///< Held links kept through a pinned root handover.
    long long stamps_rebased{0};          ///< Stamp rebases after a cut release's erase.
    long long oracle_decided{0};          ///< Links held by the oracle phase.
    long long oracle_corrected{0};        ///< Held integers the oracle phase set again (they differed from truth).
    long long truth_right{0};             ///< Held link-epochs the truth audit found right.
    long long truth_wrong{0};             ///< Held link-epochs the truth audit found wrong.
    long long truth_none{0};              ///< Held link-epochs without a true integer (no cycle, no truth row).
    long long decided_truth_wrong{0};     ///< Decisions the truth audit found wrong at the decision.
    long long truth_file_checked{0};      ///< Chord-band-epochs where the truth and the audit file both give an integer.
    long long truth_file_mismatch{0};     ///< Of them, where they differ.
    // Stage N2b.
    long long cuts_held_first{0};         ///< Cuts whose moved side re-attached through a held link the walk took first.
    long long cuts_held_first_zero{0};    ///< Of them, with a known step of 0.
    long long rehost_kept{0};             ///< Chain re-hosts onto a held link kept, not released (rehost_keep_carry): the host's carry on the held integer.
    long long rehost_off_integer{0};      ///< Re-hosts onto a held link released: the host's carry 1e-4 cycle or more off the held integer.
    long long rehost_not_kept{0};         ///< Re-hosts onto a held link released for another cause (no carry, an unknown draft step).
    long long settled_signal_epochs{0};   ///< Satellite-signal-epochs of held families flagged settled (settled_flags).
    long long unsettled_signal_epochs{0}; ///< Satellite-signal-epochs of held families not flagged settled.
    long long settled_flips{0};           ///< Changes of a satellite signal's settled flag between consecutive epochs.
    long long settled_identity_changes{0}; ///< Rule "largest": changes of a family's flagged set to another component.
    long long settled_side_changes{0};    ///< Rule "largest": satellites that changed side at those changes.
    long long settled_wait_held{0};       ///< The wait: satellite-signal-epochs in the set held back before the raise.
    long long reattached{0};              ///< Re-attachments through a held link (one per released edge the walk took first).
    long long held_first_kept{0};         ///< Held links crossing those cuts, kept at their integers (the entry links not counted).
    long long cut_released_usable{0};     ///< Held links released across an unknown step while usable (not latched, monitor not tripped).
    long long cuts_nested{0};             ///< Cuts whose whole moved side holds a release of its own.
    long long release_floored{0};         ///< Carried entries the release floor re-opened (the vehicle's drop alone stayed tighter).
    long long log_truth_right{0};         ///< Log rule: would-be decisions at the true integers (simulation only).
    long long log_truth_wrong{0};         ///< Log rule: would-be decisions off the true integers (simulation only).
    long long oracle_waited{0};           ///< Oracle holds put off to the next epoch (the link released or latched this epoch).
    long long log_links{0};               ///< Log rule: links the rule would have decided.
    long long log_link_epochs{0};         ///< Log rule: link-epochs at which it would have decided.
    long long log_changed{0};             ///< Log rule: would-be decisions with integers other than the link's first.
    // Stages N2c and O1.
    long long excluded_link_epochs{0};    ///< Chord link-epochs of pair families outside network.ring_fix.pairs (never decided).
    long long mask_epochs{0};             ///< Simulation only: epochs at which the slip mask removed the masked arc's flags.
    long long mask_keys_removed{0};       ///< Of them, the latch, funnel and bridge-pend entries removed.
    long long mask_carries_restored{0};   ///< Of them, carried entries the epoch's drains had changed, restored.
    // The re-attachment of untied satellites (network.ring_fix.reattach).
    long long untied_requests{0};         ///< Moves requested (one per satellite and family).
    long long untied_applied{0};          ///< Band-moves the election applied: the walk took the chosen arc.
    long long untied_missed{0};           ///< Band-moves not applied: the arc absent, or the satellite reached another way.
    long long untied_few{0};              ///< Satellite-epochs past the dwell with too few candidates (no move).
    long long untied_spread{0};           ///< Satellite-epochs past the dwell whose candidates disagree (no move).
    long long untied_converging{0};       ///< Satellite-epochs past the dwell whose candidates agree on a small offset (no move).
    long long untied_held_off{0};         ///< Satellite-epochs past the dwell within a moved satellite's hold-off (no move).
    long long untied_flagged_after{0};    ///< Moved satellites flagged within kRingFixReattachWatchS of the move.
    long long untied_unflagged_after{0};  ///< Moved satellites not flagged within it.
    // The near-null guard (network.ring_fix.nn_guard).
    long long nn_link_checks{0};          ///< Held chord-epochs checked (moving average full, satellite and receiver groups formed).
    long long nn_flagged{0};              ///< Of them, a deviation that fits a near-null step with a small ionosphere-free deviation.
    long long nn_refused_if{0};           ///< Of them, a step-sized geometry-free deviation refused for its ionosphere-free one.
    long long nn_moves{0};                ///< Pairs moved by -k (3, 4).
    long long nn_moves_one{0};            ///< Of them, by one step.
    long long nn_releases{0};             ///< Chords released for a second flag after a move during the same hold.
    long long nn_reopened{0};             ///< Carried entries re-opened after a move.
    // The re-validation of held entries to the tree (network.ring_fix.entry_revalidate).
    long long entry_checked{0};           ///< Cuts (per band) whose entry link was usable (held, not latched, not tripped).
    long long entry_known{0};             ///< Of them, the entry's held integer taken as a known step.
    long long entry_refused_unchecked{0}; ///< Refused: the link came back across a gap and its held pair was not measured.
    long long entry_refused_shifted{0};   ///< Refused: the gap re-entry test found the held pair shifted.
    long long entry_refused_signature{0}; ///< Refused: the near-null signature sits at a whole step.
    // The entry check of a re-host kept on a held link (entry_revalidate with rehost_keep_carry), per band.
    long long keep_checked{0};            ///< Kept re-hosts onto a usable held link whose entry was checked.
    long long keep_known{0};              ///< Of them, the held integer taken as a known step (kept at a step of 0).
    long long keep_refused_unchecked{0};  ///< Refused: back across a gap, the held pair not measured across it.
    long long keep_refused_shifted{0};    ///< Refused: the gap re-entry test found the held pair shifted.
    long long keep_refused_signature{0};  ///< Refused: the near-null signature sits at a whole step.
    // The names behind the counts (card CNN rows 2 and 4).  Filled serially (the decision, the station tally in
    // station order); read by the run-end lines only.
    std::map<std::string, long long> pair_link_epochs;   ///< Chord link-epochs considered, per listed pair "E:L1C/L5Q" (network.ring_fix.pairs set only).
    BoundedTally<std::string> excluded_families;         ///< Chord link-epochs not considered, by family "E:L1X/L5X" (the first ten).
    BoundedTally<RingFixNamedLink> rows_skipped_links[kRingFixRowSkipCount];   ///< Hold-table hits not folded, by cause (index RingFixRowSkip), the first ten links.
    BoundedTally<RingFixNamedLink> refused_links[kRingFixRefuseCount];        ///< Link-epochs refused, by cause (index RingFixRefuse), the first ten links.
};

/// A move of the re-attachment waiting for the next election (network.ring_fix.reattach).
struct RingFixUntiedMove {
    int         station{-1};   ///< The chosen station (roster index).
    std::string family;        ///< Its family, "<constellation key>:<signal 1>/<signal 2>".
};

/// What one election did with a move of the re-attachment, per band (network.ring_fix.reattach).
struct RingFixUntiedOutcome {
    std::string family;        ///< The family, "<constellation key>:<signal 1>/<signal 2>".
    std::string signal;        ///< The band's signal.
    std::string sat;           ///< The satellite.
    int         station{-1};   ///< The chosen station (roster index).
    int         taken{-1};     ///< The station of the edge the walk took for the satellite (-1: none).
    bool        applied{false};   ///< The walk took the chosen arc.
};

struct RingFixTruth;   // below: the fixture's truth (simulation only)

/// One held chord's tally of the near-null monitor: the epochs and episodes its signature sat at a whole step.
struct RingFixNnMonitorLink {
    long long epochs{0};      ///< Checked epochs at a step signature.
    long long episodes{0};    ///< Runs of such epochs (an episode opens at a step epoch after a non-step one).
    bool      on{false};      ///< The last checked epoch was at a step signature.
    int       k{0};           ///< The last step's whole number of steps.
    double    sum_gf_m{0.0};  ///< Sum of the centred geometry-free deviations over the step epochs (m).
    double    sum_if_m{0.0};  ///< Sum of the centred ionosphere-free deviations over the step epochs (m).
    double    first_tow{-1.0};///< Time of week of the first step epoch (s); -1 before any.
    double    last_tow{-1.0}; ///< Time of week of the last step epoch (s).
};

/// The near-null monitor's state: global counters and the per-chord tallies.
struct RingFixNnMonitor {
    long long checks{0};      ///< Held chord-epochs with a signature verdict.
    long long steps{0};       ///< Of them, at a whole step (k != 0) with the ionosphere-free deviation within bound.
    std::map<RingFixLinkId, RingFixNnMonitorLink> links;   ///< Per chord.
};

/**
 * @brief What the ring fix knows of a cut's entry link when it decides whether the link's held integer is a known step.
 *
 * The entry link is the edge that carries the cut's moved side now.  A held link that enters the tree is a re-coupling
 * (standing invariant 7): its integer becomes the moved side's frame step only once it is re-validated
 * (network.ring_fix.entry_revalidate, @ref ringFixEntryVerdict).
 */
struct RingFixEntryEvidence {
    bool   usable{false};         ///< Held, its monitor not tripped, neither signal in this epoch's free-epoch latch.
    bool   resumed{false};        ///< Its arcs came back across a gap this epoch, or the cut is a returning satellite's.
    bool   gap_measured{false};   ///< The gap re-entry test measured the link's own held pair across the gap.
    bool   gap_shifted{false};    ///< ... and found it shifted (|dMW| or |dGF| over the gate).
    bool   signature{false};      ///< The near-null signature has a verdict for the link (Galileo E1/E5a, enough epochs).
    int    signature_k{0};        ///< Its whole number of steps: round(centred geometry-free deviation / step).
    double signature_if_m{0.0};   ///< Its centred ionosphere-free deviation (m).
};

/// The ring fix's verdict on a cut's entry link.
enum class RingFixEntryVerdict : std::uint8_t {
    NotHeld,       ///< Not a usable held link: the step is unknown, as before.
    Known,         ///< The held integer is the known step.
    GapUnchecked,  ///< Refused: the link came back across a gap and its held pair was not measured across it.
    GapShifted,    ///< Refused: the gap re-entry test found the held pair shifted.
    Signature,     ///< Refused: the near-null signature sits at a whole step with a small ionosphere-free deviation.
    RowUnknown,    ///< The asking mechanism's rule leaves the step unknown whatever the link (the chain's returning row).
};

/**
 * @brief Run-scope state of the ring fix.  Null on every run without @c network.ring_fix.enable.
 *
 * Non-copyable (a copy would duplicate the held set and misreport the edges); movable.
 */
struct NetworkRingFix {
    /** @brief Default constructor: no links, nothing held. */
    NetworkRingFix() = default;
    /** @brief Deleted: a copy would duplicate the held set and misreport the edges. */
    NetworkRingFix(const NetworkRingFix&) = delete;
    /** @brief Deleted (see the copy constructor).
     *  @return Not applicable: the operator is deleted. */
    NetworkRingFix& operator=(const NetworkRingFix&) = delete;
    /** @brief Move constructor: the state moves whole (one owner at a time). */
    NetworkRingFix(NetworkRingFix&&) = default;
    /** @brief Move assignment: the state moves whole (one owner at a time).
     *  @return This state. */
    NetworkRingFix& operator=(NetworkRingFix&&) = default;

    RingFixRule rule{RingFixRule::Gated};   ///< The decision rule.
    RingFixOrder order{RingFixOrder::Ring};          ///< The order of the decisions.
    RingFixGateRule gate_rule{RingFixGateRule::Sigma};  ///< The gate of the decisions.
    double      max_gap_s{900.0};           ///< The gap horizon of a held link (the leveling carry's, phase_leveling_max_gap_s).
    std::string audit_file;                 ///< The audit's frame-target file ("" = no audit).
    /// (station name, satellite, signal) -> the frame integer of the audit file.
    std::map<std::tuple<std::string, std::string, std::string>, long long> audit;
    /// Every link seen as a chord of a pair family, with its state.
    std::map<RingFixLinkId, RingFixLink> links;
    /// (station index, satellite) -> the last epoch it was a tree link in either band.
    std::map<std::pair<int, std::string>, double> last_tree_s;
    /// This epoch's hold rows, built before PASS B: (station index, satellite, signal) -> the held
    /// value lambda times the integer (m).  Read only inside the parallel PASS B produce.
    std::map<std::tuple<int, std::string, std::string>, double> table;
    RingFixCounters counters;               ///< Run counters.
    // Stage N2.
    std::unique_ptr<RingFixTruth> truth;    ///< The fixture's truth (network.ring_fix.truth_root); null without it.
    double      oracle_until_s{0.0};        ///< The oracle phase's length from the first epoch (s; 0 = none).
    bool        held_first{false};          ///< The chain's walk re-attaches through held links first (network.ring_fix.held_first).
    bool        held_first_by_life{false};  ///< The held-first walk tries the held arcs by remaining life (network.ring_fix.held_first_by_life).
    /// The pair families considered (network.ring_fix.pairs), as (constellation key, signal 1, signal 2); empty: all.
    std::set<std::tuple<std::string, std::string, std::string>> pairs;
    double      wl_frac_max{0.0};           ///< The decision's bound on the wide lane's fraction (cycles; 0: none).
    double      sigma_scale{1.0};           ///< The odds gate's factor on the formal sigmas (network.ring_fix.sigma_scale).
    double      code_event_window_s{0.0};   ///< No decision on a satellite's links this long after its code event (s; 0: none).
    bool        rehost_keep_carry{false};   ///< A chain re-host onto a held link keeps the moved side's carries (network.ring_fix.rehost_keep_carry).
    int         max_decisions_per_epoch{kRingFixMaxDecisionsPerEpoch};   ///< Decisions per epoch under the order certain (network.ring_fix.max_decisions_per_epoch; 0: no cap).
    bool        settled_flags_on{false};    ///< The stream carries the settled flags (network.ring_fix.settled_flags).
    /// Tree edges that entered the tree as held links, (station, satellite, first signal of the pair), kept while
    /// the edge stays in the tree (settled_flags only).
    std::set<std::tuple<int, std::string, std::string>> certified_edges;
    /// This epoch's settled flag of every satellite signal of a held family present in its tree, (satellite,
    /// signal) -> settled (settled_flags only).
    std::map<std::pair<std::string, std::string>, bool> settled;
    bool        settled_largest{false};     ///< The settled flags follow rule "largest" (network.ring_fix.settled_rule).
    /// Rule "largest": each family's flagged set at the last epoch, "<letter>:<signal 1>/<signal 2>" -> satellites.
    std::map<std::string, std::set<std::string>> settled_incumbent;
    int         settled_wait{0};            ///< The wait on the raise, emissions (network.ring_fix.settled_wait_emissions).
    /// The wait: each family's satellites and their consecutive emissions in the set.
    std::map<std::string, std::map<std::string, int>> settled_run;
    /// The cascades (network.ring_fix.cascade); empty: none.
    std::vector<RingFixCascade> cascades;
    /// Each cascade link, keyed as (station, satellite, reference, third signal).
    std::map<RingFixLinkId, RingFixCascadeLink> cascade_links;
    /// Each cascade's run counters, by its name.
    std::map<std::string, RingFixCascadeCounters> cascade_counters;
    bool        flags_per_family{false};    ///< The settled flags are kept per family (armed with a cascade).
    bool        cascade_tree{false};        ///< A cascade's third band grows its tree on its pair's forest (network.ring_fix.cascade_tree).
    bool        cascade_log{false};         ///< The cascades decide and trace, never hold (network.ring_fix.cascade_log).
    bool        cascade_rehost_keep{false}; ///< A re-host of a third band onto a held cascade link keeps its integer (network.ring_fix.cascade_rehost_keep).
    /**
     * @brief The cascade a cascade link belongs to.
     * @param id The link (station, satellite, reference, third signal).
     * @return The cascade; null when none.
     */
    const RingFixCascade* cascadeOf(const RingFixLinkId& id) const {
        for (const RingFixCascade& c : cascades)
            if (!id.sat.empty() && c.letter == id.sat[0] && c.ref == id.sig1 && c.s3 == id.sig2) return &c;
        return nullptr;
    }
    bool        form_configured_pairs{false};   ///< The chord table forms every configured family (network.ring_fix.form_configured_pairs).
    /// The configured families by constellation letter, for the chord table (form_configured_pairs only).
    std::map<char, std::vector<std::pair<std::string, std::string>>> configured_pairs;
    // Simulation only: the slip mask (network.ring_fix.mask_slip_*).
    int         mask_station{-1};           ///< The masked arc's station (roster index; -1: no mask).
    std::string mask_sat;                   ///< The masked arc's satellite.
    double      mask_tow{-1.0};             ///< The first masked epoch (GPS time of week, s).
    int         mask_n_epochs{1};           ///< The number of masked epochs.
    /// @brief Whether a link's pair family is considered (network.ring_fix.pairs; all when the list is empty).
    /// @param sys The constellation key ("G", "E"). @param sig1 Band-1 signal. @param sig2 Band-2 signal.
    /// @return True when the family is in the list or the list is empty.
    bool pairConsidered(const std::string& sys, const std::string& sig1, const std::string& sig2) const {
        return pairs.empty() || pairs.count({sys, sig1, sig2}) != 0;
    }
    double      t0_s{-1.0};                 ///< The first epoch the ring fix saw (continuous GPS seconds).
    /// The previous epoch's tree edges with their targets: (station, satellite, signal) -> target (cycles).
    std::map<std::tuple<int, std::string, std::string>, double> prev_targets;
    // The re-attachment of untied satellites (network.ring_fix.reattach).
    bool        reattach{false};            ///< The re-attachment is armed (network.ring_fix.reattach).
    /// (family, satellite) -> when the satellite became unsettled and untied (continuous GPS seconds).
    std::map<std::pair<std::string, std::string>, double> untied_since_s;
    /// The moves for the next election, (signal, satellite) -> the move (both signals of the family carry it).
    std::map<std::pair<std::string, std::string>, RingFixUntiedMove> untied_moves;
    /// (family, satellite) -> the epoch of its last applied move, watched for its flag.
    std::map<std::pair<std::string, std::string>, double> untied_moved_s;
    /// (family, satellite) -> the epoch of its last requested move, for the hold-off (kRingFixReattachHoldoffS).
    std::map<std::pair<std::string, std::string>, double> untied_last_move_s;
    /// This epoch's election outcomes of the moves, for the decision seam's events (cleared there).
    std::vector<RingFixUntiedOutcome> untied_outcomes;
    /// This epoch's entry evidence of every held link, for the keep-carry path's check (network.ring_fix.entry_revalidate
    /// with rehost_keep_carry): gathered before the election by @ref ringFixGatherKeepEvidence from the slip layer's gap
    /// resumptions and the near-null signature of the ring fix's state at the epoch's start (the cut reads this
    /// epoch's); replaced each epoch, empty otherwise.  The election sets each link's usability from this epoch's latch.
    std::map<RingFixLinkId, RingFixEntryEvidence> keep_evidence;
    /// This epoch's refused keeps (the link and the verdict), for the decision seam's events (cleared there).
    std::vector<std::pair<RingFixLinkId, RingFixEntryVerdict>> keep_refusals;
    // The near-null guard (network.ring_fix.nn_guard).
    bool        nn_guard{false};            ///< The guard is armed (network.ring_fix.nn_guard).
    int         nn_min_epochs{60};          ///< The moving average's length and the epochs before a chord is checked.
    double      nn_tol_gf_m{0.12};          ///< The fit tolerance of the geometry-free deviation to k near-null steps (m).
    double      nn_max_if_m{0.30};          ///< The bound on the ionosphere-free deviation (m).
    std::vector<RingFixLinkId> nn_reopen;   ///< Chords moved after the last solve: their carried entries re-open before PASS B.
    bool        entry_revalidate{false};    ///< Held entries to the tree are re-validated (network.ring_fix.entry_revalidate).
    bool        nn_monitor{false};          ///< The near-null monitor is armed (network.ring_fix.nn_monitor).
    bool        nn_guard_release{false};    ///< The guard releases a flagged chord at its first flag, never moves it (network.ring_fix.nn_guard_release).
    RingFixNnMonitor nn_mon;                ///< Its state (measure-only).
};

// ---- The near-null guard (network.ring_fix.nn_guard) -------------------------------------------

/// One held chord as the near-null guard reads it.
struct RingFixNearNullIn {
    RingFixLinkId id;          ///< The chord.
    double        step_m{0.0}; ///< Its pair's near-null step on the code's geometry-free combination, 4 lambda2 - 3 lambda1 (m).
    double        gf_m{0.0};   ///< Moving average of its geometry-free post-fit code residual (m).
    double        if_m{0.0};   ///< Moving average of its ionosphere-free post-fit code residual (m).
};

/// The guard's verdict on one chord.
struct RingFixNearNullVerdict {
    RingFixLinkId id;               ///< The chord.
    double        dev_gf_m{0.0};    ///< Its geometry-free deviation, centred on its satellite's family, then its receiver (m).
    double        dev_if_m{0.0};    ///< Its ionosphere-free deviation, centred the same way (m).
    int           k{0};             ///< Near-null steps the held pair sits above the code's level, round(dev_gf / step).
    bool          act{false};       ///< k != 0, the deviation fits k steps and the ionosphere-free deviation is small.
    bool          refused_if{false};///< k != 0 and the deviation fits, refused for the ionosphere-free deviation.
};

/**
 * @brief The near-null guard's verdicts on the held chords of an epoch (pure).
 *
 * A Galileo E1/E5a chord held at the pair k (3, 4) above the truth carries the geometry-free phase error 3 lambda1 -
 * 4 lambda2; its free slant ionosphere absorbs it, and its code rows then show a geometry-free residual of k (4 lambda2
 * - 3 lambda1), about 0.448 m per step, with the ionosphere-free residual untouched, while a chord's own code bias shows
 * in both.  Each chord's moving averages are centred twice: on the median over the chords of its satellite and pair
 * family (at least 4), then on the median of its receiver's chords' remaining deviations (at least 3); a chord in a
 * smaller group gets no verdict.  k = round(dev_gf / step); @c act when k != 0, |dev_gf - k step| <= tol_gf_m and
 * |dev_if| <= max_if_m.
 *
 * @param in       The held chords whose moving averages are full.
 * @param tol_gf_m The fit tolerance of the geometry-free deviation to k steps (m).
 * @param max_if_m The bound on the ionosphere-free deviation (m).
 * @return One verdict per chord that has both groups.
 */
std::vector<RingFixNearNullVerdict> ringFixNearNullVerdicts(const std::vector<RingFixNearNullIn>& in, double tol_gf_m,
                                                            double max_if_m);

// ---- The near-null monitor (network.ring_fix.nn_monitor; measure-only) ------------------------

/// One transition of a chord's monitor state at an epoch: an episode opening or closing.
struct RingFixNnMonitorEdge {
    RingFixLinkId id;          ///< The chord.
    bool          opened{false};  ///< True: an episode opens; false: it closes.
    int           k{0};        ///< The step's whole number of steps (opening).
    double        dev_gf_m{0.0};  ///< The centred geometry-free deviation (m).
    double        dev_if_m{0.0};  ///< The centred ionosphere-free deviation (m).
};

/**
 * @brief One epoch of the near-null monitor: tallies each verdict and returns the episodes that open or close.
 *
 * A verdict is at a step when its whole number of steps is not 0 and its ionosphere-free deviation is within
 * @p max_if_m (the guard's signature without the fit tolerance: section 108.2's measure-only form).  A chord with no
 * verdict this epoch keeps its state.  Measure-only: nothing the solve or the stream reads.
 * @param m        The monitor's state; updated.
 * @param v        This epoch's verdicts (@ref ringFixNearNullVerdicts).
 * @param tow      This epoch's time of week (s).
 * @param max_if_m The bound on the ionosphere-free deviation (m).
 * @return The episodes that opened or closed at this epoch, in verdict order.
 */
std::vector<RingFixNnMonitorEdge> ringFixNnMonitorStep(RingFixNnMonitor& m, const std::vector<RingFixNearNullVerdict>& v,
                                                       double tow, double max_if_m);

// ---- The steps (pure: unit-tested on hand-built epochs) ---------------------------------------

/**
 * @brief The window test of the decision (@ref ringFixDecideStep), shared with the re-attachment's candidates.
 *
 * Holds when both carried entries exist, neither band is latched, their re-level stamps (when stamped) are at least
 * @ref kRingFixWindowS old, the link has been a chord with a float for at least the window, was no tree link within
 * it and was not released within it.
 *
 * @param L           The link's state.
 * @param o           The link's reading this epoch.
 * @param last_tree_s The last epoch it was a tree link in either band (continuous GPS seconds).
 * @param now_s       This epoch (continuous GPS seconds).
 * @return true when the window holds.
 */
bool ringFixWindowOk(const RingFixLink& L, const RingFixDecideObs& o, double last_tree_s, double now_s);

/**
 * @brief Hold a link at integers (n1, n2): the fields every hold resets, and the decision counters.
 * @param rf      The ring fix (its counters).
 * @param L       The link.
 * @param n1, n2  The held integers.
 * @param ring    The link's ring.
 * @param root    The datum receiver of the link's component.
 * @param now_s   The epoch.
 * @param kind    Decided and Oracle set the stamps from @p stamp1 and @p stamp2; Inherited keeps the link's own.
 * @param rebase_stamps  The re-base flag the path sets (a decision false, the oracle phase where a carry is missing,
 *                an inherited hold true).
 * @param stamp1, stamp2  The carries' relevel stamps (read for Decided and Oracle only).
 */
void ringFixHoldLink(NetworkRingFix& rf, RingFixLink& L, long long n1, long long n2, int ring, const std::string& root,
                     double now_s, RingFixHoldKind kind, bool rebase_stamps, double stamp1, double stamp2);

/**
 * @brief Stop holding a link: the fields every un-hold resets, and those its kind resets beyond them.  No counter.
 * @param L      The link.
 * @param now_s  The epoch.
 * @param kind   Release (also the release time, the absence clock and the audit's flag), CutRelease (the release
 *               time, the absence clock and the re-base flag), ToTree (the re-base flag).
 */
void ringFixUnholdLink(RingFixLink& L, double now_s, RingFixUnholdKind kind);

/**
 * @brief The decision of one epoch, after the solve.
 *
 * For each observed link: its continuity is updated; a held link updates its monitor (the
 * residual above @ref kRingFixMonitorBoundM at @ref kRingFixMonitorEpochs consecutive epochs sets
 * @c monitor_trip, read by the next release step); a link that is not held is refused (the first
 * test it fails, in the order not candidate, no float, window, sigma, fraction, lattice, persist)
 * or decided.  Under @ref RingFixRule::Plain a candidate with a float is decided at once.  The
 * window holds when both carried entries exist, their re-level stamps (when stamped) are at least
 * @ref kRingFixWindowS old, neither band is latched, the link has been a chord with a float for
 * at least the window, was no tree link within it and was not released within it.  A link
 * passes the persistence test at the @ref kRingFixPersistEpochs th consecutive epoch that passed
 * with the same integers; any refusal restarts the count.
 *
 * Under @ref RingFixGateRule::Odds the sigma test is replaced by the odds test (after the fraction
 * test): both log-odds at least @ref kRingFixMinLogOdds.  Under @ref RingFixOrder::Certain every link
 * with a float is a candidate, and the links that pass every test are decided after the loop, the
 * highest log-odds of N1 first (ties by link), at most @c rf.max_decisions_per_epoch (default
 * @ref kRingFixMaxDecisionsPerEpoch; 0: no cap); the others
 * are refused with the cause cap and keep their persistence count.
 *
 * During the oracle phase (simulation only: @c rf.oracle_until_s after @c rf.t0_s) every link with
 * true integers is held at them at once, and a held link whose integers differ is set to them; the
 * rule does not run.
 *
 * @param rf     The run state (links and counters written).
 * @param links  This epoch's chords of pair families with their readings.
 * @param now_s  The epoch (continuous GPS seconds).
 * @return One event per decision and per refusal, in the order of @p links.
 */
std::vector<RingFixEvent> ringFixDecideStep(NetworkRingFix& rf,
                                            const std::vector<RingFixDecideObs>& links,
                                            double now_s);

/**
 * @brief The release of one epoch, before PASS B.
 *
 * For each held link the first cause that applies, in the order chain (a tree link now, on a cut
 * release's moved side, or its component's root changed), slip (latched), refloat (a carried
 * entry missing or re-levelled since the decision), monitor (tripped), gap (absent for longer than
 * @c rf.max_gap_s).  A held link absent within the horizon keeps its hold and takes no row.  A
 * released link is no longer held, its streak and monitor restart and its release time is
 * recorded; the caller re-opens its carried entries.
 *
 * @param rf    The run state (links and counters written).
 * @param obs   The held links' readings this epoch; a held link missing from the map is absent.
 * @param now_s The epoch (continuous GPS seconds).
 * @return One event per release.
 */
std::vector<RingFixEvent> ringFixReleaseStep(NetworkRingFix& rf,
                                             const std::map<RingFixLinkId, RingFixHoldObs>& obs,
                                             double now_s);

/**
 * @brief Read an audit frame-target file into the run state.
 *
 * Header contract: the line begins `station,sat,signal,frame_int` (the frame-target files of
 * scripts/irdu_frame_targets.py; further columns are ignored).
 *
 * @param path File to read.
 * @return (station name, satellite, signal) -> frame integer.
 * @throws std::runtime_error on a missing file, a malformed header or row, or a duplicate arc.
 */
std::map<std::tuple<std::string, std::string, std::string>, long long>
ringFixReadAuditFile(const std::string& path);

/// The audit's verdict on one held link at one epoch.
enum class RingFixAudit : std::uint8_t {
    Right,     ///< Both bands' integers equal the file's.
    Wrong,     ///< A band's integer differs from the file's.
    Unlisted,  ///< The file lists neither band.
};

/**
 * @brief The audit of one held link.
 * @param audit        The audit file's integers.
 * @param station_name The link's station name.
 * @param id           The link.
 * @param n1           Its held band-1 integer.
 * @param n2           Its held band-2 integer.
 * @return Right when every listed band matches and at least one is listed, Wrong when a listed
 *         band differs, Unlisted when neither band is listed.
 */
RingFixAudit ringFixAuditLink(
    const std::map<std::tuple<std::string, std::string, std::string>, long long>& audit,
    const std::string& station_name, const RingFixLinkId& id, long long n1, long long n2);

// ---- Stage N2: held integers through the chain's events ------------------------------------------

/**
 * @brief One cut release of the epoch on one band, as the held integers read it.
 *
 * The chain re-attached the moved side M (the released vertex and its subtree) through a released
 * edge; every vertex of M moved by one integer frame step on the band.  The step is known only when
 * the released edge was a held link before the event (@ref ringFixCutStep).
 */
struct RingFixCut {
    std::string           sys;             ///< The band's constellation key ("G", "E"; empty: any), the release's group being "<key>|<signal>".
    std::string           signal;          ///< The band's phase signal.
    std::set<int>         stations;        ///< Receivers of M (roster index).
    std::set<std::string> sats;            ///< Satellites of M.
    bool                  known{false};    ///< The step is known.
    long long             delta{0};        ///< M's frame step on the band (cycles), when known.
    std::string           row;             ///< The releasing row's name ("rehost", "returning", "slip", "merge").
    int                   entry_station{-1};  ///< The released edge's station (the edge that carries M now).
    std::string           entry_sat;          ///< The released edge's satellite.
    bool                  held_first{false};  ///< The walk took the released edge first because its integer was held.
};

/**
 * @brief The frame step of a cut's moved side from its released edge.
 *
 * With the released edge e' held at N_h before the event and pinned at k' now: delta = N_h - k' when
 * the moved vertex is e''s satellite, k' - N_h when it is e''s receiver.
 *
 * @param held_before   The released edge was held before the event (and not latched, not tripped).
 * @param n_held        Its held integer on the band.
 * @param target_cyc    Its target this epoch (cycles), which must be an integer.
 * @param sat_is_child  The moved vertex is the released edge's satellite.
 * @param[out] delta    The step, written when known.
 * @return True when the step is known.
 */
bool ringFixCutStep(bool held_before, long long n_held, double target_cyc, bool sat_is_child,
                    long long& delta);

/**
 * @brief Whether a cut's entry link's held integer is a known step: the one owner of that decision.
 *
 * The chain's returning row declares a returning satellite's step unknown (its first-sighting target); the cut step
 * (@ref ringFixCutStep) makes it known from a held entry link.  With @p revalidate the held entry must also pass:
 * a link that came back across a gap needs the gap re-entry test's clean measurement of its own held pair (section
 * 107.6: a slip on the held pair, unseen because the slip layer watched other bands, became a satellite's level),
 * and a Galileo E1/E5a link whose near-null signature sits at a whole step is refused (the steps section 103 found
 * promoted into the tree).  Without @p revalidate a usable link is known, as before.
 *
 * @param e           The entry link's evidence.
 * @param revalidate  network.ring_fix.entry_revalidate.
 * @param max_if_m    The bound on the signature's ionosphere-free deviation (network.ring_fix.nn_guard_max_if_m): a
 *                    larger one is the link's own code bias, not a step.
 * @return The verdict; only @c Known makes the step known.
 */
RingFixEntryVerdict ringFixEntryVerdict(const RingFixEntryEvidence& e, bool revalidate, double max_if_m);

/// @brief The verdict's name for events and logs. @param v The verdict. @return "known", "gap_unchecked", ...
const char* ringFixEntryVerdictName(RingFixEntryVerdict v);

/// Who asks whether a held entry's step is known (@ref chainEntryStep).
enum class ChainEntryKind : std::uint8_t {
    CutEntry,      ///< The ring fix's cut: the entry link that carries a released moved side (@ref ringFixCutStep).
    ReturningRow,  ///< The chain's returning row: a vertex that rejoins its tree after an absence.
    KeepCarry,     ///< The chain's rehost row onto a held link (network.ring_fix.rehost_keep_carry).
};

/**
 * @brief Whether a held entry's step is known: the one owner of that decision for the three mechanisms that ask it.
 *
 * A held link that enters the chain's tree carries its integer as a known step only by this decision (an entry is a
 * re-coupling, standing invariant 7).  Each mechanism keeps its own rule, side by side here:
 *  - @c CutEntry: @ref ringFixEntryVerdict (a usable link; with @p revalidate, the gap re-entry test and the near-null
 *    signature as well);
 *  - @c ReturningRow: never known (@c RowUnknown): the returning vertex takes its first-sighting target and the walk
 *    re-references its moved side, while the ring fix's cut re-expresses its held links across it (a different
 *    object, decided by @c CutEntry);
 *  - @c KeepCarry: the cut's rule, on evidence gathered before the election (@ref ringFixGatherKeepEvidence: the gap
 *    resumptions of PASS A, the near-null signature of the epoch's start); off, a usable link is known.
 *
 * @param kind        Who asks.
 * @param e           The entry link's evidence.
 * @param revalidate  network.ring_fix.entry_revalidate (read by @c CutEntry and @c KeepCarry).
 * @param max_if_m    The near-null signature's ionosphere-free bound (network.ring_fix.nn_guard_max_if_m).
 * @return The verdict; only @c Known makes the step known.
 */
RingFixEntryVerdict chainEntryStep(ChainEntryKind kind, const RingFixEntryEvidence& e, bool revalidate,
                                   double max_if_m);

/**
 * @brief Gather this epoch's entry evidence of every held link for the keep-carry path's check.
 *
 * Runs before the election (network.ring_fix.entry_revalidate with rehost_keep_carry; otherwise it only empties
 * @c keep_evidence).  A link's evidence: whether its arc came back across a gap this epoch, and whether the gap
 * re-entry test measured and found shifted its own held pair (the slip layer's gap resumptions, final after PASS A);
 * its near-null signature, from the ring fix's state at the epoch's start (the last epoch's code residuals).  The
 * election takes each link's usability from this epoch's latch.
 *
 * @param rf           The ring fix's state; writes @c keep_evidence.
 * @param resumptions  This epoch's gap resumptions (the slip-QC channel's).
 */
void ringFixGatherKeepEvidence(NetworkRingFix& rf, const std::vector<GapResumption>& resumptions);

// The chain election's writes to the ring fix's state: the election reads the held links and changes the ring fix's
// state only through these functions.

/// How a chain re-host onto a held link ended (network.ring_fix.rehost_keep_carry, and
/// network.ring_fix.cascade_rehost_keep for a cascade's third band).
enum class RingFixRehostKeep : std::uint8_t {
    Kept,        ///< The new host's carry sits on the held integer and the draft's step is known: kept at a step of 0.
    OffInteger,  ///< Released: the new host's carry is off the held integer.
    NotKept,     ///< Released for another cause: no carry at the new host, or an unknown draft step.
};

/**
 * @brief Count a chain re-host onto a held link in the run counters.
 * @param rf       The ring fix's state; writes @c counters, or the cascade's @c cascade_counters entry.
 * @param cascade  The cascade whose third band was re-hosted onto one of its held links; null for a pair's link.
 * @param outcome  How the re-host ended.
 */
void ringFixCountRehost(NetworkRingFix& rf, const RingFixCascade* cascade, RingFixRehostKeep outcome);

/**
 * @brief Count a kept re-host that the entry check examined (network.ring_fix.entry_revalidate with
 *        network.ring_fix.rehost_keep_carry), and record a refused one for the decision seam's events.
 * @param rf       The ring fix's state; writes @c counters and @c keep_refusals.
 * @param id       The held link the re-host would keep.
 * @param verdict  The held entry's step under keep-carry's rule (@ref chainEntryStep).
 */
void ringFixCountKeepCheck(NetworkRingFix& rf, const RingFixLinkId& id, RingFixEntryVerdict verdict);

/**
 * @brief Record the election's outcome of an untied satellite's move and retire the move (network.ring_fix.reattach):
 *        counted as applied or missed, kept for the decision seam's events, erased from the moves.
 * @param rf       The ring fix's state; writes @c counters, @c untied_outcomes and @c untied_moves.
 * @param outcome  The outcome; its signal and satellite key the move.
 */
void ringFixRecordUntiedOutcome(NetworkRingFix& rf, const RingFixUntiedOutcome& outcome);

/**
 * @brief Add one election's third-band tree edges of a cascade to its run counters, by how the walk took them.
 * @param rf       The ring fix's state; writes the cascade's @c cascade_counters entry (created when absent).
 * @param cascade  The cascade.
 * @param pair     Edges taken as pair tree edges (lead rank 1, not a re-attachment's lead arc).
 * @param held     Edges taken as held links (lead rank 2 or more).
 * @param other    Every other edge, a re-attachment's lead arc included.
 */
void ringFixCountCascadeTree(NetworkRingFix& rf, const RingFixCascade& cascade, long long pair, long long held,
                             long long other);

/**
 * @brief Write the ring fix's memory to the snapshot's chain memory section (network.state_snapshot_chain_capture):
 *        its first epoch, its links with every field, the last tree times, the certified edges, the settled sets and
 *        their waits, the cascade links, the previous targets, the untied satellites' times and pending moves, and the
 *        guard's pending re-opens, one line each (tags @c R*).  The run counters and the near-null monitor's
 *        measure-only state are not written.
 * @param rf  The ring fix's state.
 * @param os  The section being written.
 */
void ringFixWriteMemory(const NetworkRingFix& rf, std::ostream& os);

/**
 * @brief Read one line of the ring fix's memory back (network.resume_chain_restore).
 * @param rf  The ring fix's state; written.
 * @param f   The line's comma-separated fields, the tag first.
 * @return True when the line is one of the ring fix's and was read; false for another tag or a malformed line.
 */
bool ringFixReadMemoryLine(NetworkRingFix& rf, const std::vector<std::string>& f);

/// What the cuts of an epoch did to the held links.
struct RingFixCutOutcome {
    std::vector<std::pair<RingFixLinkId, std::string>> released;  ///< Links released (crossing a cut with an unknown step), with the cut's row.
    /// Links re-expressed: (link, step on band 1, step on band 2, the rows).
    std::vector<std::tuple<RingFixLinkId, long long, long long, std::string>> reexpressed;
    long long touched{0};   ///< Held links with an end in a moved side (their carries were erased: stamps rebased).
    /// Per cut, in the order given: the held links that cross it and keep their integers.
    std::vector<std::vector<RingFixLinkId>> kept_by_cut;
};

/**
 * @brief Apply an epoch's cuts to the held links.
 *
 * A held link with exactly one end in a cut's moved side on one of its bands is re-expressed by the
 * cut's step (minus when its satellite is in M, plus when its receiver is) when the step is known,
 * and released (cause chain) when it is not; one with both ends in M, or none, keeps its integer.
 * Every held link with an end in a moved side has its carried stamps rebased at the next release step.
 * A cut's moved side is the released vertex's whole subtree, so a link that leaves a nested cut and its
 * outer one crosses both and takes both steps.
 *
 * @param rf    The run state (links and counters written).
 * @param cuts  This epoch's cuts.
 * @param now_s The epoch (continuous GPS seconds).
 * @return What the cuts did.
 */
RingFixCutOutcome ringFixApplyCuts(NetworkRingFix& rf, const std::vector<RingFixCut>& cuts, double now_s);

/**
 * @brief The band of a held link that a cut is on.
 *
 * A band is a constellation and a phase signal: GPS L1 and Galileo E1 share the code L1C, and a cut
 * on one says nothing about the other's frame.
 *
 * @param c  The cut (an empty @c sys matches any constellation).
 * @param id The link.
 * @return 1 or 2 for the link's band 1 or band 2, 0 when the cut is on neither.
 */
int ringFixCutBand(const RingFixCut& c, const RingFixLinkId& id);

/// A tree edge of the previous epoch that is a chord of its pair family now.
struct RingFixInheritCand {
    RingFixLinkId id;               ///< The link.
    long long     k1{0};            ///< Its target on band 1 at the previous epoch (cycles).
    long long     k2{0};            ///< Its target on band 2 at the previous epoch (cycles).
    double        lambda1{0.0};     ///< Band-1 wavelength (m).
    double        lambda2{0.0};     ///< Band-2 wavelength (m).
    bool          latched{false};   ///< Either band in this epoch's free-epoch latch.
    int           ring{-1};         ///< Its ring now.
    std::string   root;             ///< Its component's root now.
};

/**
 * @brief Hold the old hosts: each candidate not held and not latched is held at its previous targets
 *        moved by the steps of the cuts it crosses, when every such step is known.
 * @param rf    The run state.
 * @param cands The candidates.
 * @param cuts  This epoch's cuts.
 * @param now_s The epoch.
 * @return One Hold event (flag @c inherited) per link held.
 */
std::vector<RingFixEvent> ringFixInherit(NetworkRingFix& rf, const std::vector<RingFixInheritCand>& cands,
                                         const std::vector<RingFixCut>& cuts, double now_s);

// ---- Truth (simulation only) ----------------------------------------------------------------------

/**
 * @brief The fixture's truth as the ring fix reads it: each arc's integer and slips, each receiver's
 *        wind-up per satellite, and the producer's wind-up branch of each arc.
 *
 * The producer's integer of an arc is the fixture's slip-accumulated integer plus the branch
 * round_half_up(wu(t0)), wu the fixture's accumulated wind-up at the arc's first epoch t0, a new arc
 * after a gap above @ref kRingFixArcGapS (the rule of scripts/simnet_producer_ar_grade.py).
 */
struct RingFixTruth {
    /// One arc's integer and its jumps (continuous GPS seconds, cycles), ascending.
    struct Arc {
        long long n_int{0};                              ///< The pre-slip integer.
        std::vector<std::pair<double, long long>> slips;  ///< (time, jump).
    };
    /// One wind-up branch of a (receiver, satellite): its first epoch, its last epoch, its integer.
    struct Branch {
        double    start_tow{0.0};  ///< First epoch (time of week, s).
        double    last_tow{0.0};   ///< Last epoch seen (time of week, s).
        long long k{0};            ///< The branch integer.
    };
    std::map<std::tuple<int, std::string, std::string>, Arc> arcs;          ///< (station, sat, signal) -> the arc.
    std::map<std::pair<int, std::string>, std::map<double, double>> windup; ///< (station, sat) -> tow -> cycles.
    std::map<std::pair<int, std::string>, std::vector<Branch>> branches;   ///< (station, sat) -> its branches.
    long long arcs_without_windup{0};   ///< Branches started without a wind-up row (k = 0).
};

/// A gap above this ends a (receiver, satellite) arc for the wind-up branch (s).
inline constexpr double kRingFixArcGapS = 300.0;

/**
 * @brief round_half_up of a wind-up in cycles: floor(w + 0.5).
 * @param w The accumulated wind-up (cycles).
 * @return The branch integer.
 */
long long ringFixWindupBranch(double w);

/**
 * @brief Record that a (receiver, satellite) is seen at an epoch: a new branch at its first epoch or
 *        after a gap above @ref kRingFixArcGapS, with the wind-up at that epoch.
 * @param T       The truth.
 * @param station Roster index.
 * @param sat     Satellite.
 * @param tow     Time of week (s).
 */
void ringFixTruthObserve(RingFixTruth& T, int station, const std::string& sat, double tow);

/**
 * @brief The producer's true integer of an arc at an epoch.
 * @param T       The truth.
 * @param station Roster index.
 * @param sat     Satellite.
 * @param signal  Phase signal.
 * @param now_s   Continuous GPS seconds (the slips' clock; a slip at now_s counts).
 * @param tow     Time of week (s), for the branch.
 * @param[out] n  The integer.
 * @return False when the arc has no truth row.
 */
bool ringFixTruthN(const RingFixTruth& T, int station, const std::string& sat, const std::string& signal,
                   double now_s, double tow, long long& n);

/**
 * @brief The true integer of a chord in the frame of an epoch: the sum over its fundamental cycle of
 *        coefficient times the true integer, less the sum over the tree terms of coefficient times target.
 * @param T            The truth.
 * @param terms        The cycle's terms: (station, satellite, signal, coefficient), the chord included (+1).
 * @param pin_sum_cyc  Sum over the tree terms of coefficient times target (cycles).
 * @param pins_integer Every target on the path is an integer.
 * @param now_s        Continuous GPS seconds.
 * @param tow          Time of week (s).
 * @param[out] n       The integer.
 * @return False when a target is not an integer, a term has no truth, or the sum is not an integer.
 */
bool ringFixTruthFrameInteger(const RingFixTruth& T,
                              const std::vector<std::tuple<int, std::string, std::string, int>>& terms,
                              double pin_sum_cyc, bool pins_integer, double now_s, double tow, long long& n);

/**
 * @brief Read one receiver's truth_windup.csv.
 *
 * Header contract: the line begins `week,tow_s,satellite,windup_cycles`.
 *
 * @param path File to read.
 * @return satellite -> tow -> accumulated wind-up (cycles).
 * @throws std::runtime_error on a missing file or a malformed header or row.
 */
std::map<std::string, std::map<double, double>> ringFixParseWindupCsv(const std::string& path);

// ---- The network seams ----------------------------------------------------------------------

struct NetworkRun;          // network/driver/Run.hpp
struct NetworkEpoch;        // network/driver/Epoch.hpp
struct NetworkRunEndView;   // network/runend/RunEnd.hpp
struct NetworkRunContext;   // network/state/RunContext.hpp
struct StationAssembly;     // network/driver/RunTypes.hpp
struct StationCarryFrame;   // network/assembly/StationBuild.hpp
struct NetworkSolutionEpoch;   // products/NetworkProductEmitter.hpp
struct RelevelEpochState;   // network/driver/RunTypes.hpp
struct NetStation;          // network/driver/RunTypes.hpp

// The helpers the seams and their stages share (network/ar/RingFixSeams.cpp, the stages of
// network/ar/RingFixHold.hpp and network/ar/RingFixDecide.hpp).

/**
 * @brief The chain record's group key of a link's band.
 * @param sat The satellite ("E05"); its first letter names the constellation.
 * @param sig The band's phase signal ("L1C").
 * @return "<letter>|<signal>" ("E|L1C"), the key the record's edges, chords and roots carry.
 */
std::string ringFixGroupOf(const std::string& sat, const std::string& sig);

/**
 * @brief The arcs in this epoch's free-epoch latch, as (station, satellite, signal).
 * @param relevel This epoch's latch (the PASS A slip responses and the float vehicle's drops).
 * @return Every latched arc whose ambiguity key round-trips to an arc.
 */
std::set<net::ArcTuple> ringFixLatchedArcs(const RelevelEpochState& relevel);

/**
 * @brief The near-null signature's inputs: every held Galileo E1/E5a chord whose moving averages of
 *        its code residuals span @c rf.nn_min_epochs, with its step 4 lambda2 - 3 lambda1.
 *
 * The guard (network.ring_fix.nn_guard), the monitor (network.ring_fix.nn_monitor) and the entry
 * re-validation (network.ring_fix.entry_revalidate) read the verdicts of the same inputs.
 *
 * @param rf The ring fix (its links and their moving averages).
 * @return One input per qualifying held chord, in link order.
 */
std::vector<RingFixNearNullIn> ringFixNearNullInputs(const NetworkRingFix& rf);

/**
 * @brief A station's roster name for an event row.
 * @param stations The roster.
 * @param i        The station's roster index.
 * @return Its name, or "" when @p i is out of range.
 */
std::string ringFixStationName(const std::vector<NetStation>& stations, int i);

/**
 * @brief Setup builder: builds the run state when @c network.ring_fix.enable is set and reads
 *        the audit file.  Called once from runNetwork.
 * @param run The run: writes run.ctx.carry.ring_fix; reads run.nc, run.config and run.log.
 * @throws std::runtime_error when the audit file cannot be read.
 */
void networkRingFixArm(NetworkRun& run);

/**
 * @brief Before PASS B: the releases of this epoch and the hold rows PASS B folds.
 *
 * Serial, after the phase-datum election (the chain record) and PASS A's slip latch, and before
 * the float vehicle's drop pass and PASS B.  Reads each held link's state from the chain record,
 * the latch and the carried entries, runs @ref ringFixReleaseStep, re-opens the released links'
 * carried entries (the vehicle's drop under float_kf, an erase on the leveling vehicle; skipped
 * for a slip, which the drop pass answers, and for a refloat, already answered), writes the
 * release events and rebuilds the hold table.  Gate: the run state exists.  The body is an
 * ordered list of stages (network/ar/RingFixHold.hpp, which states their order and why).
 *
 * @param run The run. Written: run.ctx (the ring fix's state, the carried entries of released
 *            links), run.traces (events). Read: run.config, run.setup.stations, run.log.
 * @param ep  The epoch: ep.se.time, ep.clock.pl_now_s, ep.events.phase_datum.chain,
 *            ep.events.slip.relevel.
 */
void networkRingFixHoldEpoch(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The hold rows of one station: one diagonal prior row per held band column.
 *
 * Step `ring_hold` of kStationCarrySteps (network/assembly/StationBuild.hpp); its kinds:
 * PriorAmbiguityRingHold.  It returns at once unless `legacy_local_inject` holds and the hold
 * table is non-empty.  Records: the rfix_* counters of StationAssembly.
 *
 * @param carry The station's carry frame (the build, the priors, the block columns).
 */
void networkRingFixPriors(StationCarryFrame& carry);

/**
 * @brief Folds the stations' hold-row accounting into the run counters.  Serial, after the
 *        parallel produce.  Does nothing without the run state.
 * @param ctx     The run context: the counters are written.
 * @param asm_res The stations' assembled blocks, read.
 */
void networkRingFixStationTally(NetworkRunContext& ctx,
                                const std::vector<StationAssembly>& asm_res);

/**
 * @brief After the solve: the monitor, the audit and the decisions of this epoch.
 *
 * Serial, after the producer's search and the lattice step, before the solution's preparation.
 * Forms each pair-family chord's two-band float and conditional covariance (the chord table's
 * quantities, @c chainChordFloats), its ring (@ref ringFixRings), its lattice integers and its
 * carried entries' stamps; each held link's post-fit ionosphere-free phase residual; runs
 * @ref ringFixDecideStep; audits every held link; writes the hold events and the trace.
 * Gate: the run state exists and the chain record is armed.  The body is an ordered list of
 * stages (network/ar/RingFixDecide.hpp, which states their order and why).
 *
 * @param run The run. Written: run.ctx (the ring fix's state), run.traces (events, the trace).
 *            Read: run.engines.est, run.setup.stations, run.config.
 * @param ep  The epoch after the solve: ep.solve (x_s, plan), ep.assembly (asm_res,
 *            est_index_of), ep.events (the chain record, the latch), ep.se.time, ep.clock.
 */
void networkRingFixDecide(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief The near-null monitor (network.ring_fix.nn_monitor): an epoch observer, measure-only.
 *
 * After the decision, the centred code signature of every held Galileo E1/E5a chord (the guard's moving averages and
 * verdicts, nothing moved) is tallied by @ref ringFixNnMonitorStep; an RINGFIX nn_monitor_on / nn_monitor_off event at
 * each episode's edges; a run-end table of the chords with the most step epochs.  Its row in kNetworkEpochObservers
 * (slot RosterTail).  Gate: the ring fix's state exists and the monitor is armed.
 *
 * @param run The run. Written: the ring fix's monitor state, run.traces (events). Read: the held chords' averages.
 * @param ep  The epoch (its time).
 */
void networkRingFixNearNullMonitor(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Stamps the settled flags on an epoch's phase-bias records (network.ring_fix.settled_flags).
 *
 * On a record of a held family (a pair of @p rf's pairs, matched by constellation and signal)
 * signal_integer is the satellite signal's settled flag of this epoch (false when the satellite has
 * none) and wide_lane_integer is 1; on every other record both are 0.  Called last in the solution's
 * preparation, after every record exists: the wire fill copies a record of the same band and channel
 * family into a missing modelled signal, and the copy carries the flags of its own signal, not those
 * of its source.
 *
 * @param sol The epoch's solution; every phase-bias record is stamped in place.
 * @param rf  The ring fix after this epoch's decisions (its pairs and its settled flags).
 */
void ringFixStampSettledFlags(NetworkSolutionEpoch& sol, const NetworkRingFix& rf);

/**
 * @brief Run-end report: the ring fix's decisions, holds, releases, refusals and audit.
 *
 * Gate: the run state exists (nothing is printed otherwise).
 *
 * @param v The run at its end.
 * @return 0 (the report does not set the exit code).
 */
int networkRingFixRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
