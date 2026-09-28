"""Backend-agnostic animation: a sequence of :class:`Figure` frames -> GIF.

A viewer that wants to animate over epochs (the ionosphere pierce-point movie,
say) builds one :class:`Figure` per frame and returns them wrapped in an
:class:`Animation` on its :class:`~gnssviz.viewers.base.ViewerResult`.  Rendering
stays the backend's job: :func:`render_gif` draws each frame through the same
:class:`~gnssviz.backends.base.Backend` the static figures use, then assembles a
looping GIF with Pillow (already a matplotlib dependency, so no new requirement).

The :class:`Figure` contract is untouched: an animation is *N* ordinary figures,
not a new figure kind, so every existing trace/axes/projection works inside a
frame for free.  The report layer base64-embeds the GIF as an ``<img>`` (browsers
auto-loop animated GIFs), exactly like the static PNGs.
"""

from __future__ import annotations

import io
from dataclasses import dataclass, field
from typing import List

from .figures import Figure


@dataclass
class Animation:
    """An ordered set of :class:`Figure` frames rendered to a looping GIF.

    ``fps`` sets the playback rate (frames per second).  ``summary`` is surfaced
    in the HTML report like a viewer summary.
    """
    name: str = "animation"
    title: str = ""
    frames: List[Figure] = field(default_factory=list)
    fps: float = 4.0
    summary: str = ""

    @property
    def n_frames(self) -> int:
        return len(self.frames)


def render_gif(anim: Animation, backend, *, dpi: int = 90, loop: int = 0) -> bytes:
    """Render an :class:`Animation` to GIF bytes via ``backend``.

    Each frame is drawn with a *fixed* (non-tight) bounding box so every frame
    shares pixel dimensions; as a safety net frames are padded onto a common
    white canvas before assembly, so any residual size jitter cannot corrupt the
    GIF.  ``loop=0`` loops forever.
    """
    from PIL import Image

    if not anim.frames:
        raise ValueError(f"animation '{anim.name}' has no frames")

    imgs: List = []
    for fr in anim.frames:
        rendered = backend.render(fr)
        try:
            png = backend.to_png_bytes(rendered, dpi=dpi, tight=False)
        finally:
            backend.close(rendered)
        imgs.append(Image.open(io.BytesIO(png)).convert("RGB"))

    w = max(im.width for im in imgs)
    h = max(im.height for im in imgs)
    frames = []
    for im in imgs:
        if im.size != (w, h):
            bg = Image.new("RGB", (w, h), (255, 255, 255))
            bg.paste(im, (0, 0))
            im = bg
        frames.append(im)

    duration_ms = max(1, int(round(1000.0 / max(anim.fps, 0.1))))
    buf = io.BytesIO()
    frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:],
                   duration=duration_ms, loop=loop, disposal=2)
    return buf.getvalue()


def write_gif(path: str, anim: Animation, backend, *, dpi: int = 90,
              loop: int = 0) -> str:
    """Render ``anim`` and write the GIF to ``path``; returns ``path``."""
    data = render_gif(anim, backend, dpi=dpi, loop=loop)
    with open(path, "wb") as fh:
        fh.write(data)
    return path
