#include "ui/panorama/panorama_render_backend.hpp"

namespace panorama
{
namespace
{
// Selection belongs to the calling UI strand. A host scopes its renderer on
// worker threads instead of replacing another window's main-thread backend.
thread_local PanoramaRenderBackend* g_panorama_render_backend = nullptr;
}

PanoramaRenderBackend* panorama_render_backend()
{
    return g_panorama_render_backend;
}

void set_panorama_render_backend(PanoramaRenderBackend* backend)
{
    g_panorama_render_backend = backend;
}
}
