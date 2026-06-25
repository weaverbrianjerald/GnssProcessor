# IAU SOFA (vendored)

This directory contains the **IAU SOFA** (Standards of Fundamental Astronomy)
ANSI-C library, vendored verbatim into the GnssProcessor tree so the build is
self-contained and portable (no dependency on an external reference checkout).

- **Release:** 2023-10-11 (revision 2023 April 16)
- **Origin:** http://www.iausofa.org/ — the C ("ansi") distribution.
- **Contents:** all 240 library `*.c` files plus `sofa.h` and `sofam.h`.
  The upstream test harness (`t_sofa_c.c`) and the `UnitTest/` directory are
  intentionally **not** vendored (they contain `main()` and are not part of the
  library).
- **Modifications:** none. The sources are byte-for-byte upstream.

## License

SOFA is distributed under the **SOFA Software License** (a permissive,
BSD-style license with a name-change requirement for derived works). The full
license text is reproduced in the notes at the end of every source file and in
`sofa.h`. Per the license, this vendored copy is unmodified and retains all
original copyright and license notices.

## Build integration

Built as a separate static library `sofa` (see the top-level `CMakeLists.txt`),
with compiler warnings suppressed (third-party C). `gnss_core` links it
`PUBLIC`. It is consumed by `Planetary::celestialToTerrestrial`, the CIO-based
IAU 2000A/2006 celestial-to-terrestrial frame transform gated behind the
`estimation.enable_cio_frame` config flag.

## Upgrading

Replace the `*.c`, `sofa.h`, and `sofam.h` files with a newer upstream release
(again excluding `t_sofa_c.c` / `UnitTest/`). The `file(GLOB ...)` in
`CMakeLists.txt` picks up the new file set automatically on the next configure.
