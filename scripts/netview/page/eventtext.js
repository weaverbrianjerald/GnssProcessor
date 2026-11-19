'use strict';
/*
 * Network dashboard and live monitor: plain words for the events of network_events.csv (tier/event -> text) and for the
 * causes of a release of held whole numbers.  Shared by the dashboard (network.js, events.js) and the live page.
 */

const EVENT_TEXT = {
  'RINGFIX/hold': 'whole numbers decided (held from the next epoch)',
  'RINGFIX/release': 'released',
  'RINGFIX/to_tree': 'became a tree link',
  'RINGFIX/reattach': 'the tree re-formed through this held link',
  'CHAIN/rehost': 'tree re-formed: a node moved to a new parent',
  'CHAIN/cut_release': 'tree cut: the moved side lets go of its links',
  'CHAIN/returning': 'a node returned to the tree',
  'CHAIN/split': 'the tree split in two',
  'CHAIN/merge': 'two trees merged',
  'CHAIN/slip_deseat': 'a tree link slipped and the node moved',
  'CHAIN/kept': 'tree link kept through an event',
  'CHAIN/declared_keys': 'tree bookkeeping after a move',
  'PHASE_DATUM/eq34_reattach': 'receiver re-attached to a new tree parent',
  'PHASE_DATUM/eq34_reref': 'phase reference re-realized',
  'T0_gap/arc_resume': 'tracking resumed after a gap',
  'T0_gap/sig_resume': 'signal resumed after a gap',
  'T0_gap/reentry_relevel': 'float re-levelled at a return',
  'SIGNAL/reentry': 'satellite back after a short gap',
  'SIGNAL/band_lost': 'a signal lost',
  'SIGNAL/band_regain': 'a signal regained',
  'READMIT/readmit': 'satellite readmitted after a long gap',
  'T0_mw_detect/lli_slip': 'slip: loss of lock reported by the receiver',
  'T0_mw_detect/mw_wl_slip': 'slip: wide-lane (Melbourne-Wubbena) jump',
  'T1_gf_jump/gf_jump': 'slip: geometry-free jump',
  'T1_if_slip/if_slip': 'slip: ionosphere-free time-difference test',
  'T0_windup/wu_restart': 'wind-up restarted',
  'RAIM/residual_alarm': 'alarm: post-fit residual too large',
  'T4_prior/prior_arc': 'alarm: a carried float re-opened',
  'T4_attrib/attrib_arc': 'alarm: a float re-opened by attribution',
  'GROSSLOS/gross_los_declare': 'alarm: gross error on this line of sight',
  'GROSSLOS/gross_los_clear': 'gross error cleared',
  'CLK_ANCHOR/cta_decouple': 'alarm: satellite clock left its anchor',
  'CLK_ANCHOR/cta_recouple': 'satellite clock back on its anchor',
  'CB_DATUM/reseed_disc_landed': 'code reference re-seeded',
  'CB_DATUM/family_birth_prior': 'code reference: new code signal of a satellite',
  'CLK_BASIS/reselect': 'the clock moved to another code reference',
  'ZWD/slip_latch': 'wet delay carry reset by a slip',
  'ZWD/recouple': 'wet delay carry resumed',
  'BLOCKBAARDA/block_outlier': 'outlier test of a receiver-satellite block',
  'MDB/mdb_exceedance': 'smallest detectable error above its bound',
  'CONGRUENCE/coordinate_offset': 'coordinate offset test',
};
const RELEASE_TEXT = {
  slip: 'a slip on either signal', refloat: 'its float re-opened by another check', gap: 'the link absent too long',
  chain: 'the tree changed around it', monitor: 'its residual stayed above 3 cm',
};
