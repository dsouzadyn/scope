#pragma once

#include <imgui.h>

#include <cstdint>

namespace scope {

/// Platform-neutral GPU texture uploader for live 2D pixel buffer probes.
///
/// Implemented in `main.mm` using native macOS `MTLTexture` (`MTLPixelFormatRGBA8Unorm`).
class TextureBridge {
  public:
    virtual ~TextureBridge() = default;
    /// Uploads `width * height * 4` tightly-packed RGBA8 bytes and returns an `ImTextureID`.
    virtual ImTextureID upload_rgba(int width, int height, const uint8_t* rgba_bytes) = 0;
};

} // namespace scope
