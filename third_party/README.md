# Third-party code

Code here is not covered by Bulwark's clean-room rules (it is not engine code
written for Bulwark), but every library must be under a licence that keeps
Bulwark ISC-compatible: ISC, MIT, BSD, zlib or similar. No GPL-only code.

| Library | Version | Licence | Used for |
| --- | --- | --- | --- |
| PanoramaEngine | upstream commit `f3c780e84159a64d0975234420f09933cd8a82c8` | MIT (`PanoramaEngine/LICENSE`) | UI views, see `docs/spec/ui.md` |
| QuickJS (inside PanoramaEngine/Thirdparty) | 0.15.0 | MIT | JavaScript for UI views |
| FreeType (inside PanoramaEngine/Thirdparty) | bundled | FreeType License (FTL), chosen from its FTL/GPLv2 dual licence | font rasterising |

PanoramaEngine was vendored without its examples, tests and renderer adapters.
Its claim to be an independent implementation of Panorama has not been
checked against Valve's code.
