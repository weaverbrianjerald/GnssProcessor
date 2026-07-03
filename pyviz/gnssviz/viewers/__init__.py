"""Viewer registry.

Each viewer module exposes a module-level ``viewer`` (:class:`.base.Viewer`).
``REGISTRY`` is the ordered list the driver dispatches over -- the Python
analogue of the ``REG`` table in ``view_all.m``.  Add a new viewer by importing
its ``viewer`` here and appending it to ``REGISTRY``.
"""

from __future__ import annotations

from .base import Viewer, ViewerResult

# State / position
from .state import viewer as _state
from .wls import viewer as _wls
from .baseline import viewer as _baseline
# Measurement domain
from .measurement_residuals import viewer as _measurement_residuals
from .residual_strip import viewer as _residual_strip
from .plot_satellite_signals import viewer as _plot_satellite_signals
from .dops import viewer as _dops
from .satellite_status import viewer as _satellite_status
from .satellite_strip import viewer as _satellite_strip
# State domain (per satellite)
from .ambiguities import viewer as _ambiguities
from .ambiguity_locktime import viewer as _ambiguity_locktime
from .ionosphere import viewer as _ionosphere
from .iono_strip import viewer as _iono_strip
from .sat_biases import viewer as _sat_biases
# Cycle-slip diagnostics
from .mwwl_trace import viewer as _mwwl_trace
from .turbo_edit_trace import viewer as _turbo_edit_trace
from .slip_gate_strip import viewer as _slip_gate_strip
from .slip_conditions import viewer as _slip_conditions
from .graphic_trace import viewer as _graphic_trace
from .gf_trace import viewer as _gf_trace
from .if_slip_trace import viewer as _if_slip_trace
# Network-level (PPP-RTK NETWORK producer)
from .network_map import viewer as _network_map
from .network_iono_map import viewer as _network_iono_map
from .network_health import viewer as _network_health
from .network_products import viewer as _network_products
from .network_closed_loop import viewer as _network_closed_loop
# Network log-viz L-queue (QC timelines, V5 instruments, product/consumer inspection)
from .network_epoch_status import viewer as _network_epoch_status
from .network_events import viewer as _network_events
from .network_flag_rates import viewer as _network_flag_rates
from .network_clock_nis import viewer as _network_clock_nis
from .network_dd_fractional import viewer as _network_dd_fractional
from .network_anchor_info import viewer as _network_anchor_info
from .network_zwd import viewer as _network_zwd
from .network_product_file import viewer as _network_product_file
from .ssr_consumer import viewer as _ssr_consumer
from .network_discontinuities import viewer as _network_discontinuities
from .network_station_strip import viewer as _network_station_strip
from .network_manifest import viewer as _network_manifest

REGISTRY = [
    _state, _wls, _baseline,
    _measurement_residuals, _residual_strip, _plot_satellite_signals,
    _dops, _satellite_status, _satellite_strip,
    _ambiguities, _ambiguity_locktime, _ionosphere, _iono_strip, _sat_biases,
    _mwwl_trace, _turbo_edit_trace, _slip_gate_strip, _slip_conditions,
    _graphic_trace, _gf_trace, _if_slip_trace,
    _network_map, _network_health, _network_products, _network_iono_map,
    _network_closed_loop,
    # L-queue additions
    _network_epoch_status, _network_events, _network_flag_rates, _network_clock_nis,
    _network_dd_fractional, _network_anchor_info, _network_zwd,
    _network_product_file, _ssr_consumer, _network_discontinuities,
    _network_station_strip, _network_manifest,
]

__all__ = ["Viewer", "ViewerResult", "REGISTRY"]
