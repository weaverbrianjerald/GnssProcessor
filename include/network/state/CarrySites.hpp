#pragma once
/**
 * @file CarrySites.hpp
 * @brief The carried references of the network's border parameters, and one traversal
 *        over them (card PRM phase 2, docs/refound/PRM_parameter_registry_2026-09-29.md
 *        section 5).
 *
 * A border parameter (a satellite clock, phase bias, code bias or SH coefficient) is
 * referenced by more of the carried state than its own entry in @c prev_carry: every
 * station-local carry holds a regression on it (@ref CoupledShared, with the value the
 * regression was taken at), the clock monitor and the hold keep the last value as the
 * baseline of a step test, an open pin bridge holds the value the bridge pins, the
 * stash records the pinned pivot's value, and an ambiguity's own carried value is
 * expressed in the frame of its satellite's phase bias.  A mechanism that moves the
 * parameter's frame (an ephemeris cut-over, a clock-pivot handover, a phase-bias datum
 * shift, a declared anchor handover) must move every one of those
 * references by the same amount, or the next epoch reads the parameter in two frames.
 *
 * Until this card each such mechanism enumerated the containers by hand, and each list
 * was a different subset (card CLV section 6.4 item 6: the ephemeris re-level moved two
 * of the ten local carries; card LGM's phase-colour carry, added after card CLV, is in
 * no list).  This header declares the containers once (@ref CarrySite, one row per
 * container and reference role, with its key form, what it holds, how a shift applies
 * and whether the warm-resume snapshot restores it), one walk over them
 * (@ref forEachCarriedReference), and, per mechanism, the sites it moves and the sites
 * it does not move with the reason (@ref carryMechanismSite).  A mechanism calls the
 * walk with its own site set and a visitor that applies its shift; the walk counts what
 * each visitor moved per site, and the run-end table states it.
 *
 * Phase 2 changes no number: every mechanism keeps the set of references it moved
 * before, written as its site set plus declared exemptions.  The exemptions are the
 * gaps the hand lists left, now named, and a site added to the carried state is added
 * here or the unit test that scans the carry state's declaration fails.  Phase 3b step 2
 * closes the owed gaps behind one default-off knob, @c network.carry_rebase_complete:
 * with it, each mechanism's walk also moves the sites its row exempts as owed
 * (@ref carryMechanismOwedSites); the by-design exemptions and the phase hold's held
 * value stay where they are.
 *
 * Containers that hold a border-frame quantity and are deliberately not sites (each
 * either rebuilt every epoch, cleared at a datum change, re-based by its own mechanism,
 * or holding no value): the per-epoch scratch (@c cxu_epoch, @c readmit_seed_check,
 * @c sts_c0, @c sts_dense, @c oracle_pin), the smoothed hold's clones
 * (@c smh_clone, @c smh_block and their pending maps: their own re-base,
 * @c pbHoldCloneRebase, every epoch), card LAH's per-constellation state
 * (@c pb_hold_frz_M, @c pb_hold_frz_prev_clk), the clock-level monitor's previous
 * epoch (@c clm_prev_clk, cleared at a datum change), the anchor's one-epoch record
 * (@c clk_anchor), the phase-datum elections' own targets and ledgers (@c phase_basis,
 * @c eq34_prev_target, the handover ledger, rebuilt by their elections), the one- to
 * two-epoch local copies (@c relevel_pend), the receiver-side rate memory
 * (@c rx_clk_rate), and the SH field's deviation carry (no mechanism moves an SH
 * coefficient's frame).
 */

#include "network/driver/RunTypes.hpp"   // SharedParamKind, CarryPrior, CoupledShared, LocalCarry, ClockOsc

#include <cstdint>
#include <functional>
#include <string>

namespace GnssProcessor {

struct NetworkCarryState;   // network/state/RunContext.hpp, included in the .cpp only
namespace net { struct NetworkEntities; }   // network/state/Entities.hpp (card NOM step N4)

/**
 * @brief One container (and reference role) of @c NetworkCarryState that holds a
 *        reference of a border parameter.
 *
 * The roles: a carried border value (with its variance where it has one), a regression
 * term on a border column (@ref CoupledShared: @c s_prev with @c p_prev), a baseline (a
 * previous solved or emitted value a step test or a datum reference reads), a held value,
 * a pin target, and a station-local carried value expressed in a border parameter's frame
 * (@ref LocalCarry::value_abs of an ambiguity, a receiver clock, a receiver phase or code
 * bias).  A local carry map with both a value in a border frame and regression terms is
 * two sites.
 */
enum class CarrySite : int {
    PrevCarry = 0,         ///< @c prev_carry: the carried border value and variance (live and parked entries)
    CxuPivot,              ///< @c cxu_pivot: the stash's pinned clock per constellation and its value
    ClockOsc,              ///< @c clock_osc: the two-state clock oscillator's carried offset (x0)
    PrevClk,               ///< @c prev_clk: the clock monitor's jump baseline (first)
    CbExcPrevClk,          ///< @c cb_exc_prev_clk: the code-bias excursion test's previous solved clock
    PbHoldPrevClock,       ///< @c pb_hold_prev_clock_m: the emitted clock at the hold's previous emission
    PbLevelPrev,           ///< @c pb_level_prev: the last solved phase bias ("SAT_SIGNAL")
    PinBridge,             ///< @c pin_bridge: an open pin bridge's held phase bias ("SAT_SIGNAL")
    PbHold,                ///< @c pb_hold: the phase hold's held value ("SAT|SIGNAL", emit frame)
    CodeBiasPrev,          ///< @c code_bias_prev_: the time-linked code-bias datum reference
    AmbiguityValue,        ///< @c local_carry value_abs: the ambiguity, in its phase bias's frame
    AmbiguityCoupling,     ///< @c local_carry coupling: the ambiguity's regression on the border
    ZwdCoupling,           ///< @c zwd_carry coupling
    IonoCoupling,          ///< @c iono_carry coupling (empty by construction)
    IonoBlockCoupling,     ///< @c iono_blk_carry coupling
    HoldAmbiguityValue,    ///< @c gl_hold_amb value_abs: the gross screen's held ambiguity
    HoldAmbiguityCoupling, ///< @c gl_hold_amb coupling (cleared at the declaration)
    HoldIonoCoupling,      ///< @c gl_hold_iono coupling (cleared at the declaration)
    HoldIonoBlockCoupling, ///< @c gl_hold_iono_blk coupling (cleared at the declaration)
    RxClockValue,          ///< @c rx_clk_carry value_abs: the receiver clock, in the clock datum's frame
    RxClockCoupling,       ///< @c rx_clk_carry coupling
    RxPhaseBiasValue,      ///< @c rpb_carry value_abs: the eq34 chain's receiver phase bias
    RxPhaseBiasCoupling,   ///< @c rpb_carry coupling (empty by construction)
    RxCodeBiasValue,       ///< @c rx_cb_carry value_abs: the receiver code bias, in the code-bias datum's frame
    RxCodeBiasCoupling,    ///< @c rx_cb_carry coupling
    PhaseColourCoupling,   ///< @c pc_carry coupling (card LGM)
};

/// Number of @ref CarrySite values (array sizing).
inline constexpr int kCarrySites = 26;

/// The reference role of a site (see @ref CarrySite).
enum class CarryRefRole : int {
    Value = 0,    ///< a carried border value (and its variance)
    Regression,   ///< a regression term on a border column (s_prev, with p_prev as the variance)
    Baseline,     ///< a previous solved or emitted value a step test or datum reference reads
    Held,         ///< a held value
    PinTarget,    ///< a pin target
    LocalValue,   ///< a station-local carried value expressed in a border parameter's frame
};

/**
 * @brief The declaration of one site: the row of the card's section 5 table.
 *
 * @c record names the warm-resume snapshot record that restores the site (null when the
 * site is cold-started on a resume), and @c snapshot_note the capture knob or the reason
 * it is cold-started (the decomposition plan's two-place rule; the registry records the
 * membership as it is, card PRM section 9).
 */
struct CarrySiteSpec {
    CarrySite    site;
    const char*  name;            ///< short name (the run-end table, the manifest)
    const char*  container;       ///< the @c NetworkCarryState member
    const char*  key;             ///< the container's key form
    CarryRefRole role;
    const char*  holds;           ///< what the reference is
    const char*  shift;           ///< how a shift of the parameter applies to it
    const char*  record;          ///< the snapshot record that restores it, or null
    const char*  snapshot_note;   ///< the capture condition, or why it is cold-started
};

/// The declaration of @p s.
const CarrySiteSpec& carrySiteSpec(CarrySite s);

/// Short name of a reference role.
const char* carryRefRoleName(CarryRefRole r);

/// Bit of @p s in a site mask.
inline constexpr std::uint32_t carrySiteBit(CarrySite s) {
    return std::uint32_t{1} << static_cast<int>(s);
}

/// The mask of every site.
inline constexpr std::uint32_t kAllCarrySites = (std::uint32_t{1} << kCarrySites) - 1u;

/**
 * @brief The mechanisms that move a border parameter's frame through the carry.
 *
 * Each is a caller of @ref forEachCarriedReference with its own site set
 * (@ref carryMechanismSites).  @c HandoverBeforeRows is the clock-pivot handover's
 * S-transform when it runs before the coupling rows are built: the same transform over a
 * wider site set (declared here with the kernel; the seam that runs it before PASS B is
 * card PRM phase 2's third step).
 */
enum class CarryMechanism : int {
    EphRelevel = 0,       ///< the ephemeris cut-over re-level (@c networkEphCutoverRelevel): one clock by -Δ_eph
    Handover,             ///< the clock-pivot handover S-transform after PASS B (@c applyClockStransform)
    HandoverBeforeRows,   ///< the same transform before PASS B, over every clock reference of the constellation
    Eq34Shift,            ///< the eq34 chain datum's D4 carry shift (@c eq34ShiftCarry): phase biases and receiver phase biases (also for the seed-round re-host's edges)
    DeclaredHandover,     ///< the float vehicle's declared anchor handover (@c networkAmbFloatDeclaredHandover)
};

/// Number of @ref CarryMechanism values (array sizing).
inline constexpr int kCarryMechanisms = 5;

/// Short name of a mechanism (the run-end table, the manifest).
const char* carryMechanismName(CarryMechanism m);

/**
 * @brief What a mechanism does with one site: moves it, does not move it for a stated
 *        reason (an exemption), or does not apply (the site holds no reference of the
 *        parameter kinds or the role the mechanism's direction moves; @c reason null).
 */
struct CarryMechanismSite {
    bool        moves{false};
    const char* reason{nullptr};   ///< the exemption's reason; null when it moves or does not apply
};

/// What @p m does with @p s.
CarryMechanismSite carryMechanismSite(CarryMechanism m, CarrySite s);

/// The mask of the sites @p m moves.
std::uint32_t carryMechanismSites(CarryMechanism m);

/**
 * @brief The mask of the sites @p m exempts as owed: the gaps of the hand-written lists
 *        (card PRM sections 16.2 and 16.7), which @c network.carry_rebase_complete moves.
 *
 * A site is owed when its exemption's reason is one of the owed reasons (a gap of the hand
 * list card PRM phase 2 replaced, card CLV section 6.4 item 6).  Never in the set: a site
 * the mechanism already moves, a site outside its direction (no reason), a by-design
 * exemption (the D4 shift's bridged root, the handover's reasons, the receiver clock under
 * the handover included), and @c PbHold for every mechanism: the phase hold's held value
 * is an emitted value in the emit frame, and moving it needs its own design, so it stays
 * exempt with that reason.  The two handover mechanisms have no owed site (their walks take
 * no knob).
 * @param m the mechanism.
 * @return the owed site mask (disjoint from @ref carryMechanismSites).
 */
std::uint32_t carryMechanismOwedSites(CarryMechanism m);

/**
 * @brief The site mask a walk of @p m covers on a run: @ref carryMechanismSites, with the
 *        owed sites when @p complete (@c network.carry_rebase_complete, card PRM phase 3b).
 * @param m        the mechanism.
 * @param complete whether the owed sites are moved.
 * @return @c carryMechanismSites(m) | (complete ? carryMechanismOwedSites(m) : 0).
 */
std::uint32_t carryMechanismSiteMask(CarryMechanism m, bool complete);

/**
 * @brief Whether @p m moves @p s on a run, with the owed sites when @p complete.
 * @param m        the mechanism.
 * @param s        the site.
 * @param complete @c network.carry_rebase_complete.
 * @return whether @p s is in @ref carryMechanismSiteMask.
 */
bool carryMechanismMoves(CarryMechanism m, CarrySite s, bool complete);

/**
 * @brief One carried reference, as the traversal hands it to a visitor.
 *
 * @c kind, @c sat and @c signal identify the border parameter the reference belongs to
 * (for a @c LocalValue, the parameter whose frame the local value is expressed in; a
 * receiver-side local value has an empty @c sat and the map's own key in @c signal).
 * @c value is the double a shift moves; @c var the variance a re-expression moves with it
 * (the entry's variance on @c PrevCarry, the term's @c p_prev on a coupling site), null
 * where the site has none.  The typed pointers give a visitor the owning object when a
 * shift rule needs more than the double (an oscillator's @c init flag, a term's
 * coefficient, a prior's park stamp).
 */
struct CarriedRef {
    CarrySite          site;
    SharedParamKind    kind;
    const std::string& sat;
    const std::string& signal;
    int                station;   ///< the station index of a local carry site; -1 otherwise
    double*            value;
    double*            var;
    bool               parked;    ///< a parked @c prev_carry entry (READMIT, @c t_park_s > 0)
    CarryPrior*        prior;     ///< @c PrevCarry: the entry
    CoupledShared*     term;      ///< a coupling site: the term
    LocalCarry*        local;     ///< a local carry site: the owning entry
    ClockOsc*          osc;       ///< @c ClockOsc: the oscillator
};

/// A visitor: apply the mechanism's rule to one reference; return whether it moved it.
using CarryRefVisitor = std::function<bool(const CarriedRef&)>;

/// Per-site tallies of one walk.
struct CarrySiteCounts {
    long long visited[kCarrySites]{};   ///< references handed to the visitor
    long long moved[kCarrySites]{};     ///< references the visitor reported moved
    long long moved_border[4]{};        ///< the carried border values moved (@c PrevCarry and the clock oscillator's @c ClockOsc), per @c SharedParamKind (card PRM phase 3a)
};

/**
 * @brief The run-scope moved tallies: per [site][mechanism], and the @c PrevCarry moves per
 *        [@c SharedParamKind][mechanism] (the Stransform responses of the border kinds,
 *        card PRM phase 3a).
 */
struct CarryMovedTally {
    long long site[kCarrySites][kCarryMechanisms]{};
    long long border[4][kCarryMechanisms]{};
};

/**
 * @brief Walk every carried reference of the sites in @p sites, in a fixed order, and
 *        hand each to @p visit.
 *
 * The order is the order of @ref CarrySite, and within a site the container's own order
 * (vector order for @c prev_carry, key order for a map, term order within a local
 * carry's coupling).  A shift of a frame moves each referenced double once, so the order
 * cannot change a value; a visitor that accumulates side outputs keeps them in ordered or
 * de-duplicating containers.  The walk itself writes
 * nothing, creates no container entry and reads no value.
 *
 * @param carry  the run's carried state.
 * @param entities the run's entity tables (the station carries, card NOM step N4a).
 * @param sites  the site mask (@ref carrySiteBit, @ref carryMechanismSites).
 * @param visit  the visitor.
 * @param counts optional out: per-site visited and moved tallies (accumulated, not reset).
 * @return the number of references the visitor reported moved.
 */
long long forEachCarriedReference(NetworkCarryState& carry, net::NetworkEntities& entities,
                                  std::uint32_t sites,
                                  const CarryRefVisitor& visit,
                                  CarrySiteCounts* counts = nullptr);

/// Add one walk's moved tallies to the run-scope table.
inline void carryTallyMoved(CarryMovedTally& moved, CarryMechanism m, const CarrySiteCounts& c) {
    for (int i = 0; i < kCarrySites; ++i) moved.site[i][static_cast<int>(m)] += c.moved[i];
    for (int k = 0; k < 4; ++k) moved.border[k][static_cast<int>(m)] += c.moved_border[k];
}

/**
 * @brief The number of references each site holds (a walk with a counting visitor).
 * @param carry the run's carried state (not modified).
 * @param entities the run's entity tables (the station carries, card NOM step N4a).
 * @param held  out: references per site.
 */
void carrySiteHeld(const NetworkCarryState& carry, const net::NetworkEntities& entities,
                   long long (&held)[kCarrySites]);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the carried references per site (PRM 2).
 *
 * The carried references of the border per site and mechanism (card PRM phase 2), with
 * the `param_sites` manifest record.
 *
 * Gate: prints always (a block).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkCarrySitesRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
