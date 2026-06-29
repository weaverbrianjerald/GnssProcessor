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

REGISTRY = [
    _state, _wls, _baseline,
    _measurement_residuals, _residual_strip, _plot_satellite_signals,
    _dops, _satellite_status, _satellite_strip,
    _ambiguities, _ambiguity_locktime, _ionosphere, _iono_strip, _sat_biases,
    _mwwl_trace, _turbo_edit_trace, _slip_gate_strip, _slip_conditions,
    _graphic_trace, _gf_trace, _if_slip_trace,
    _network_map, _network_health, _network_products, _network_iono_map,
    _network_closed_loop,
]

__all__ = ["Viewer", "ViewerResult", "REGISTRY"]
