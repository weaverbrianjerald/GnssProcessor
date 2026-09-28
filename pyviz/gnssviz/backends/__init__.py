"""Rendering backends.

``Backend`` is the abstract contract. ``MatplotlibBackend`` implements it for
static figures and report PNGs; ``PlotlyBackend`` implements the same interface
for interactive and real-time output.
"""

from __future__ import annotations

from .base import Backend


def get_backend(name: str = "matplotlib") -> Backend:
    """Factory: return a backend instance by name."""
    name = name.lower()
    if name in ("matplotlib", "mpl", "static"):
        from .matplotlib_backend import MatplotlibBackend
        return MatplotlibBackend()
    if name in ("plotly", "interactive"):
        from .plotly_backend import PlotlyBackend
        return PlotlyBackend()
    raise ValueError(f"unknown backend: {name!r} (available: matplotlib, plotly)")


__all__ = ["Backend", "get_backend"]
