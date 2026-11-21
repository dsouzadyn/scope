#pragma once

#include <imgui.h>

#include <cstdint>

namespace scope {

// Minimal GPU texture bridge for uploading 2D pixel probes straight into Metal textures.
class TextureBridge {
public:
    virtual ~TextureBridge() = default;
    virtual ImTextureID upload_rgba(int width, int height, const uint8_t* rgba_bytes) = 0;
};

} // namespace scope
