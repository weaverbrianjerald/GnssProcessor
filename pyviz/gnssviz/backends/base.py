"""The rendering backend contract.

A backend turns a :class:`gnssviz.figures.Figure` (pure data) into a rendered
artifact.  The matplotlib backend (static figures, PNG-for-report, today) and a
future plotly backend (interactive / real-time) implement this same interface,
which is the whole point of the backend-agnostic split: viewers never import a
plotting library, they only build :class:`Figure` objects.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any

from ..figures import Figure


class Backend(ABC):
    """Renders :class:`Figure` specs.

    A "rendered object" is whatever the backend produces (a matplotlib
    ``Figure``, a plotly ``Figure``, ...); callers move it around opaquely and
    pass it back to :meth:`save` / :meth:`to_png_bytes`.
    """

    name: str = "abstract"

    #: Whether this backend's report fragments need the plotly.js runtime once
    #: per document (interactive backends set this True).
    needs_js: bool = False

    @abstractmethod
    def render(self, fig: Figure) -> Any:
        """Build and return the backend-native figure object for ``fig``."""

    @abstractmethod
    def to_png_bytes(self, rendered: Any, dpi: int = 110) -> bytes:
        """Serialise a rendered figure to PNG bytes (for HTML/report embedding)."""

    @abstractmethod
    def save(self, rendered: Any, path: str, dpi: int = 110) -> None:
        """Write a rendered figure to ``path`` (format inferred from extension)."""

    def to_html(self, rendered: Any, dpi: int = 110) -> str:
        """Return an embeddable HTML fragment for a rendered figure.

        Default: a base64 PNG ``<img>`` (static backends).  Interactive backends
        override this to emit a live ``<div>`` (and set :attr:`needs_js`).
        """
        import base64
        b64 = base64.b64encode(self.to_png_bytes(rendered, dpi=dpi)).decode("ascii")
        return f'<div class="fig"><img src="data:image/png;base64,{b64}"></div>'

    def live_payload(self, rendered: Any, dpi: int = 110) -> dict:
        """A JSON-able payload for *in-place* live update (real-time dashboard).

        Default: a PNG data-URI (``{"kind": "img", "src": ...}``) -- the live
        client swaps the image.  Interactive backends override to return the
        native figure description (e.g. plotly ``{"kind": "plotly", "data",
        "layout"}``) so the client can update without reloading the page.
        """
        import base64
        b64 = base64.b64encode(self.to_png_bytes(rendered, dpi=dpi)).decode("ascii")
        return {"kind": "img", "src": f"data:image/png;base64,{b64}"}

    def show(self, rendered: Any) -> None:  # pragma: no cover - interactive
        """Display a rendered figure (no-op for non-interactive backends)."""
        raise NotImplementedError(f"{self.name} backend does not support show()")

    def close(self, rendered: Any) -> None:
        """Release any resources held by a rendered figure."""
