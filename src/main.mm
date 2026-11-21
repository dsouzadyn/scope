#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_metal.h>

#include "app/App.hpp"
#include "ui/TextureBridge.hpp"
#include "ui/Theme.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

class MetalTextureBridge final : public scope::TextureBridge {
public:
    explicit MetalTextureBridge(id<MTLDevice> device) : device_(device) {}
    ~MetalTextureBridge() override {
        texture_ = nil;
    }

    ImTextureID upload_rgba(int width, int height, const uint8_t* rgba_bytes) override {
        if (!device_ || width <= 0 || height <= 0 || !rgba_bytes) return 0;

        if (!texture_ || width_ != width || height_ != height) {
            MTLTextureDescriptor* desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                   width:static_cast<NSUInteger>(width)
                                                                  height:static_cast<NSUInteger>(height)
                                                               mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModeShared;
            texture_ = [device_ newTextureWithDescriptor:desc];
            width_ = width;
            height_ = height;
        }

        MTLRegion region = MTLRegionMake2D(0, 0, static_cast<NSUInteger>(width),
                                           static_cast<NSUInteger>(height));
        [texture_ replaceRegion:region
                    mipmapLevel:0
                      withBytes:rgba_bytes
                    bytesPerRow:static_cast<NSUInteger>(width * 4)];

        return (ImTextureID)(__bridge void*)texture_;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLTexture> texture_ = nil;
    int width_ = 0;
    int height_ = 0;
};

void glfw_error_callback(int error, const char* desc) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, desc);
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        glfwSetErrorCallback(glfw_error_callback);
        if (!glfwInit()) {
            return 1;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);

        GLFWwindow* window = glfwCreateWindow(1540, 960, "SCOPE // C++ PRECISION DEBUGGER", nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            return 1;
        }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> command_queue = [device newCommandQueue];

        NSWindow* nswin = glfwGetCocoaWindow(window);
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.contentsScale = nswin.backingScaleFactor;
        nswin.contentView.layer = layer;
        nswin.contentView.wantsLayer = YES;
        nswin.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        float xscale = 1.0f, yscale = 1.0f;
        glfwGetWindowContentScale(window, &xscale, &yscale);

        scope::Theme::load_fonts(xscale);
        scope::Theme::apply(scope::ThemeMode::AnodizedLab);

        ImGui_ImplGlfw_InitForOther(window, true);
        ImGui_ImplMetal_Init(device);

        MetalTextureBridge tex_bridge(device);
        std::string app_dir = std::filesystem::weakly_canonical(argv[0]).parent_path().string();
        std::string initial_target = (argc > 1) ? std::string(argv[1]) : "";
        scope::App app(&tex_bridge, app_dir, initial_target);

        MTLRenderPassDescriptor* render_pass_desc = [MTLRenderPassDescriptor new];

        while (!glfwWindowShouldClose(window)) {
            @autoreleasepool {
                glfwPollEvents();

                int width = 0, height = 0;
                glfwGetFramebufferSize(window, &width, &height);
                if (width == 0 || height == 0) continue;

                layer.contentsScale = nswin.backingScaleFactor;
                layer.drawableSize = CGSizeMake(width, height);
                id<CAMetalDrawable> drawable = [layer nextDrawable];
                if (!drawable) continue;

                const auto& pal = scope::Theme::palette();
                ImVec4 clear_c = ImGui::ColorConvertU32ToFloat4(pal.bg_void);
                render_pass_desc.colorAttachments[0].clearColor =
                    MTLClearColorMake(clear_c.x, clear_c.y, clear_c.z, 1.0);
                render_pass_desc.colorAttachments[0].texture = drawable.texture;
                render_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
                render_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;

                id<MTLCommandBuffer> cmd_buf = [command_queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder =
                    [cmd_buf renderCommandEncoderWithDescriptor:render_pass_desc];

                ImGui_ImplMetal_NewFrame(render_pass_desc);
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                app.render_frame();

                ImGui::Render();
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd_buf, encoder);

                [encoder endEncoding];
                [cmd_buf presentDrawable:drawable];
                [cmd_buf commit];
            }
        }

        ImGui_ImplMetal_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
    return 0;
}
