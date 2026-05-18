// Minimal RT64-backed RendererContext for Body Harvest.
//
// Adapted from Zelda64Recompiled/src/main/rt64_render_context.cpp, stripped
// of texture-pack hooks, mod hooks, debugger UI, etc. Goal for Phase 4a: get
// RT64::Application::setup() to succeed against our Win32 window, so
// ultramodern's renderer thread reports SetupResult::Success and the
// recompiled game can advance past libultra init.
//
// send_dl and update_screen actually call into RT64; the renderer will draw
// black or whatever the game submits. That's enough for libultra's frame
// semantics to tick over.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

// Windows headers must be included before RT64's dxcapi.h so IUnknown /
// IStream (COM interfaces) are defined. WIN32_LEAN_AND_MEAN strips combaseapi
// from windows.h, so pull objbase.h in explicitly afterwards.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

#include "ultramodern/renderer_context.hpp"

// RT64 headers. RT64 doesn't have a clean public-API split, so we reach into
// src/ directly via the include paths set up in CMakeLists.
#define HLSL_CPU
#include "hle/rt64_application.h"

// Globals exposed in bh_app.hpp so main.cpp's watchdog can read them.
std::atomic<uint64_t> s_send_dl_count{0};
std::atomic<uint64_t> s_update_screen_count{0};

// Live pointer to the RT64::Application owned by the active
// BHRendererContext. Set in the context's constructor after
// setup() succeeds, cleared on destruction. Read by other
// translation units (e.g. bh_cheats.cpp's Enhancements tab) to
// reach RT64's runtime config. The Application's update*Config
// methods are internally thread-safe per RT64's inspector design.
std::atomic<RT64::Application*> g_rt64_app{nullptr};

namespace {

// RT64 needs pointers to N64 register state. ultramodern owns the VI regs;
// the rest aren't really tracked by ultramodern, so we keep our own zeroed
// shadows. RT64 reads these; it doesn't drive interrupts off them.
struct DummyN64Regs {
    unsigned int MI_INTR_REG = 0;
    unsigned int DPC_START_REG = 0;
    unsigned int DPC_END_REG = 0;
    unsigned int DPC_CURRENT_REG = 0;
    unsigned int DPC_STATUS_REG = 0;
    unsigned int DPC_CLOCK_REG = 0;
    unsigned int DPC_BUFBUSY_REG = 0;
    unsigned int DPC_PIPEBUSY_REG = 0;
    unsigned int DPC_TMEM_REG = 0;
};

DummyN64Regs g_dummy_regs;

// RT64 needs DMEM/IMEM (RSP scratch). 4KB each.
uint8_t g_dmem[0x1000];
uint8_t g_imem[0x1000];

// 0x40 dummy ROM header — RT64 references this for game-identification
// heuristics but we don't drive any of them.
uint8_t g_dummy_rom_header[0x40] = {0};

void dummy_check_interrupts() {}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult r) {
    switch (r) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        default:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
    }
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        default:
            return ultramodern::renderer::GraphicsApi::Auto;
    }
}

class BHRendererContext final : public ultramodern::renderer::RendererContext {
public:
    BHRendererContext(uint8_t* rdram,
                      ultramodern::renderer::WindowHandle window_handle,
                      bool developer_mode) {
        RT64::Application::Core appCore{};
#if defined(_WIN32)
        appCore.window = window_handle.window;
#endif
        appCore.checkInterrupts = dummy_check_interrupts;
        appCore.HEADER = g_dummy_rom_header;
        appCore.RDRAM  = rdram;
        appCore.DMEM   = g_dmem;
        appCore.IMEM   = g_imem;

        appCore.MI_INTR_REG       = &g_dummy_regs.MI_INTR_REG;
        appCore.DPC_START_REG     = &g_dummy_regs.DPC_START_REG;
        appCore.DPC_END_REG       = &g_dummy_regs.DPC_END_REG;
        appCore.DPC_CURRENT_REG   = &g_dummy_regs.DPC_CURRENT_REG;
        appCore.DPC_STATUS_REG    = &g_dummy_regs.DPC_STATUS_REG;
        appCore.DPC_CLOCK_REG     = &g_dummy_regs.DPC_CLOCK_REG;
        appCore.DPC_BUFBUSY_REG   = &g_dummy_regs.DPC_BUFBUSY_REG;
        appCore.DPC_PIPEBUSY_REG  = &g_dummy_regs.DPC_PIPEBUSY_REG;
        appCore.DPC_TMEM_REG      = &g_dummy_regs.DPC_TMEM_REG;

        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        appCore.VI_STATUS_REG        = &vi->VI_STATUS_REG;
        appCore.VI_ORIGIN_REG        = &vi->VI_ORIGIN_REG;
        appCore.VI_WIDTH_REG         = &vi->VI_WIDTH_REG;
        appCore.VI_INTR_REG          = &vi->VI_INTR_REG;
        appCore.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        appCore.VI_TIMING_REG        = &vi->VI_TIMING_REG;
        appCore.VI_V_SYNC_REG        = &vi->VI_V_SYNC_REG;
        appCore.VI_H_SYNC_REG        = &vi->VI_H_SYNC_REG;
        appCore.VI_LEAP_REG          = &vi->VI_LEAP_REG;
        appCore.VI_H_START_REG       = &vi->VI_H_START_REG;
        appCore.VI_V_START_REG       = &vi->VI_V_START_REG;
        appCore.VI_V_BURST_REG       = &vi->VI_V_BURST_REG;
        appCore.VI_X_SCALE_REG       = &vi->VI_X_SCALE_REG;
        appCore.VI_Y_SCALE_REG       = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration appConfig;
        appConfig.useConfigurationFile = false;

        app = std::make_unique<RT64::Application>(appCore, appConfig);
        app->userConfig.developerMode = developer_mode;
        app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale  = true;

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_setup_result(app->setup(thread_id));
        chosen_api = map_graphics_api(app->chosenGraphicsAPI);

        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            app.reset();
        } else {
            // Phase 15b: expose the Application pointer so the
            // Enhancements tab can push live config changes
            // (aspect ratio, refresh rate, etc.).
            g_rt64_app.store(app.get(), std::memory_order_release);
        }
    }

    ~BHRendererContext() override {
        g_rt64_app.store(nullptr, std::memory_order_release);
    }

    bool valid() override { return static_cast<bool>(app); }

    bool update_config(const ultramodern::renderer::GraphicsConfig&,
                       const ultramodern::renderer::GraphicsConfig&) override {
        // Phase 4a: no dynamic config changes yet.
        return false;
    }

    void enable_instant_present() override {
        // Newer RT64 dropped the enableInstantPresent API. Default present
        // behavior is fine for Phase 4a — we just need frames to advance.
    }

    void send_dl(const OSTask* task) override {
        s_send_dl_count.fetch_add(1, std::memory_order_relaxed);
        if (!app) return;

        // Body Harvest occasionally submits gfx tasks with data_ptr
        // values past the 8-MiB RDRAM end (seen during the intro
        // cutscene + main-menu skip transitions: e.g. 0x076A0000).
        // RT64's processDisplayLists does `memory[dlStartAddress]`
        // with no bounds check and AVs reading beyond the RDRAM mmap.
        // Drop these tasks (and log the first few) instead of crashing.
        constexpr uint32_t kRdramSize = 0x00800000;
        const uint32_t data_phys  = task->t.data_ptr  & 0x3FFFFFF;
        const uint32_t ucode_phys = task->t.ucode      & 0x3FFFFFF;
        const uint32_t udata_phys = task->t.ucode_data & 0x3FFFFFF;
        if (data_phys  >= kRdramSize ||
            ucode_phys >= kRdramSize ||
            udata_phys >= kRdramSize ||
            (data_phys + task->t.data_size) > kRdramSize)
        {
            static std::atomic<uint64_t> warned{0};
            if (warned.fetch_add(1, std::memory_order_relaxed) < 8) {
                std::fprintf(stderr,
                    "[gfx] send_dl: OOB gfx task — data_ptr=0x%X data_size=0x%X "
                    "ucode=0x%X ucode_data=0x%X — skipped\n",
                    data_phys, task->t.data_size, ucode_phys, udata_phys);
            }
            return;
        }
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(ucode_phys, udata_phys, true);
        app->processDisplayLists(app->core.RDRAM, data_phys,
                                 task->t.data_size, true);
    }

    void update_screen() override {
        s_update_screen_count.fetch_add(1, std::memory_order_relaxed);
        if (!app) return;
        app->updateScreen();
    }

    void shutdown() override {
        if (app) app->end();
    }

    uint32_t get_display_framerate() const override {
        // RT64 no longer exposes a public refresh-rate getter. ultramodern
        // uses this to scale audio + timing — 60 is correct for an NTSC
        // game like Body Harvest until we wire monitor query in Phase 6.
        return 60;
    }

    float get_resolution_scale() const override {
        // No upscaling for Phase 4a; render at native (1x).
        return 1.0f;
    }

private:
    std::unique_ptr<RT64::Application> app;
};

} // namespace

namespace bh {

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram,
                      ultramodern::renderer::WindowHandle window_handle,
                      bool developer_mode) {
    return std::make_unique<BHRendererContext>(rdram, window_handle, developer_mode);
}

// Phase 15b — Enhancements tab helpers.
//
// These let the F1 menu (bh_cheats.cpp) push live config changes
// into RT64 without it needing to know about the Application class.
// Both set the appropriate fields on app->userConfig then call
// app->updateUserConfig(discardFBs) — RT64's documented runtime-
// reconfigure entry point.

namespace renderer {

// Apply an aspect-ratio choice. discardFBs=true so RT64 recreates
// its framebuffer at the new aspect (changes resolution geometry).
void set_aspect_ratio(int mode_index) {
    RT64::Application* app = g_rt64_app.load(std::memory_order_acquire);
    if (app == nullptr) return;

    using AR = RT64::UserConfiguration::AspectRatio;
    switch (mode_index) {
    case 0:  // 4:3 (Original / native)
        app->userConfig.aspectRatio = AR::Original;
        break;
    case 1:  // 16:9
        app->userConfig.aspectRatio = AR::Manual;
        app->userConfig.aspectTarget = 16.0 / 9.0;
        break;
    case 2:  // 16:10
        app->userConfig.aspectRatio = AR::Manual;
        app->userConfig.aspectTarget = 16.0 / 10.0;
        break;
    case 3:  // 21:9 ultrawide
        app->userConfig.aspectRatio = AR::Manual;
        app->userConfig.aspectTarget = 21.0 / 9.0;
        break;
    case 4:  // 32:9 super-ultrawide
        app->userConfig.aspectRatio = AR::Manual;
        app->userConfig.aspectTarget = 32.0 / 9.0;
        break;
    case 5:  // Stretch to window
        app->userConfig.aspectRatio = AR::Expand;
        break;
    default:
        return;
    }
    app->updateUserConfig(true);
    std::fprintf(stderr, "[enh] aspect ratio -> mode %d (target=%.3f)\n",
                 mode_index, app->userConfig.aspectTarget);
}

// Apply a fog distance scale. 1.0 = original BH fog, larger
// numbers push fog further out. RT64's G_MW_FOG handler reads
// enhancementConfig.fog.scaleFactor on every fog-set command and
// divides BH's multiplier accordingly.
void set_fog_scale(float scale) {
    RT64::Application* app = g_rt64_app.load(std::memory_order_acquire);
    if (app == nullptr) return;

    // Clamp to a sane positive range. 0 or negative would brick fog.
    if (scale < 0.1f)  scale = 0.1f;
    if (scale > 16.0f) scale = 16.0f;

    app->enhancementConfig.fog.scaleFactor = scale;
    app->updateEnhancementConfig();
    std::fprintf(stderr, "[enh] fog scale -> %.2fx\n", double(scale));
}

// Apply a 2D upscaling mode. ScaledOnly often helps HUD elements
// in wide aspect ratios because RT64 re-anchors / re-scales 2D
// draws to fit the new viewport instead of leaving them at their
// original 4:3-coordinate positions. "Original" preserves the
// vanilla N64 look (pixel-perfect 2D at native scale).
void set_upscale_2d(int mode_index) {
    RT64::Application* app = g_rt64_app.load(std::memory_order_acquire);
    if (app == nullptr) return;

    using U2D = RT64::UserConfiguration::Upscale2D;
    switch (mode_index) {
    case 0: app->userConfig.upscale2D = U2D::Original;   break;
    case 1: app->userConfig.upscale2D = U2D::ScaledOnly; break;
    case 2: app->userConfig.upscale2D = U2D::All;        break;
    default: return;
    }
    app->updateUserConfig(false);
    std::fprintf(stderr, "[enh] upscale 2D -> mode %d\n", mode_index);
}

// Apply a refresh-rate choice. discardFBs=false — refresh rate is
// presentation timing, doesn't change framebuffer dimensions.
void set_refresh_rate(int mode_index) {
    RT64::Application* app = g_rt64_app.load(std::memory_order_acquire);
    if (app == nullptr) return;

    using RR = RT64::UserConfiguration::RefreshRate;
    int target = 60;
    switch (mode_index) {
    case 0:  // Native (whatever the game runs at — 30 for BH)
        app->userConfig.refreshRate = RR::Original;
        break;
    case 1:  // 60
        app->userConfig.refreshRate = RR::Manual;
        app->userConfig.refreshRateTarget = target = 60;
        break;
    case 2:  // 75
        app->userConfig.refreshRate = RR::Manual;
        app->userConfig.refreshRateTarget = target = 75;
        break;
    case 3:  // 120
        app->userConfig.refreshRate = RR::Manual;
        app->userConfig.refreshRateTarget = target = 120;
        break;
    case 4:  // 144
        app->userConfig.refreshRate = RR::Manual;
        app->userConfig.refreshRateTarget = target = 144;
        break;
    case 5:  // Match display refresh
        app->userConfig.refreshRate = RR::Display;
        break;
    default:
        return;
    }
    app->updateUserConfig(false);
    std::fprintf(stderr, "[enh] refresh rate -> mode %d (target=%dHz)\n",
                 mode_index, target);
}

} // namespace renderer

} // namespace bh
