# Third-party code

Code here is not covered by Bulwark's clean-room rules (it is not engine code
written for Bulwark), but every library must be under a licence that keeps
Bulwark ISC-compatible: ISC, MIT, BSD, zlib or similar. No GPL-only code.

| Library | Version | Licence | Used for |
| --- | --- | --- | --- |
| PanoramaEngine | upstream commit `f3c780e84159a64d0975234420f09933cd8a82c8` | MIT (`PanoramaEngine/LICENSE`) | UI views, see `docs/spec/ui.md` |
| QuickJS (inside PanoramaEngine/Thirdparty) | 0.15.0 | MIT | JavaScript for UI views and entity scripts |
| Lua (`lua-5.4.7`) | 5.4.7, tag `v5.4.7` of github.com/lua/lua (commit `1ab3208a`) | MIT (`lua-5.4.7/LICENSE`) | Lua scripting, see `docs/spec/scripting.md` |
| nanosvg (`nanosvg/`) | commit `239e102ec2c691f2902e20ace2ed36ee4a35cfe6` of github.com/memononen/nanosvg | zlib (`nanosvg/LICENSE.txt`) | SVG images (UI art), `src/render/r_svg.c` |
| FreeType (inside PanoramaEngine/Thirdparty) | bundled | FreeType License (FTL), chosen from its FTL/GPLv2 dual licence | font rasterising |

PanoramaEngine was vendored without its examples, tests and renderer adapters.
Its claim to be an independent implementation of Panorama has not been
checked against Valve's code.

Lua is the interpreter's own sources only: the standalone `lua.c`, the
amalgamation `onelua.c` and the test harness `ltests.*` were left out, and
nothing was changed.
