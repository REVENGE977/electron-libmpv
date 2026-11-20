#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "mpv_loader.h"

namespace electron_mpv {

constexpr int kSurfacePoolSize = 3;

enum class FrameHandleType {
    None = 0,
    WinNtHandle,
    MacIOSurface,
    LinuxNativePixmap,
    SoftwareRgba,
};

struct FrameDescriptor {
    FrameHandleType type = FrameHandleType::None;
    int slot_id = -1;
    int width = 0;
    int height = 0;
    std::string pixel_format = "bgra";  // "bgra" | "rgba" | "rgbaf16"
    bool hdr = false;
    int64_t timestamp_us = 0;

    // Platform handles
    uintptr_t nt_handle = 0;
    uintptr_t io_surface = 0;

    // Linux NativePixmap (single-plane RGBA/BGRA/RGBAF16 DMA-BUF)
    int dmabuf_fd = -1;
    uint32_t stride = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t modifier = 0;
    bool supports_zero_copy_webgpu = false;

    // Software fallback pixels
    std::vector<uint8_t> sw_pixels;
    size_t sw_stride = 0;
};

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    virtual const char* name() const = 0;
    virtual bool supportsSharedTexture() const = 0;
    virtual bool supportsHdr() const = 0;

    // Called on the dedicated render thread.
    virtual bool init(const MpvApi& api, mpv_handle* mpv, mpv_render_context** out_ctx, std::string& out_error) = 0;

    // Called on the dedicated render thread when size or HDR mode changes.
    virtual bool ensureSurface(int width, int height, bool hdr, std::string& out_error) = 0;

    // Called on the dedicated render thread to render the next frame into `slot`.
    virtual bool renderFrame(
        const MpvApi& api,
        mpv_render_context* ctx,
        int slot,
        FrameDescriptor& out_frame,
        std::string& out_error
    ) = 0;

    // Called on the dedicated render thread before destroying mpv.
    virtual void shutdown(const MpvApi& api, mpv_render_context* ctx) = 0;

    // Slot pool tracking (thread-safe: acquire on render thread, release from JS/main thread).
    int acquireFreeSlot() {
        for (int attempt = 0; attempt < kSurfacePoolSize; ++attempt) {
            int idx = (next_slot_ + attempt) % kSurfacePoolSize;
            bool expected = false;
            if (slot_in_use_[idx].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                next_slot_ = (idx + 1) % kSurfacePoolSize;
                return idx;
            }
        }
        return -1;
    }

    void releaseSlot(int slot) {
        if (slot >= 0 && slot < kSurfacePoolSize) {
            slot_in_use_[slot].store(false, std::memory_order_release);
        }
    }

    void resetSlots() {
        for (int i = 0; i < kSurfacePoolSize; ++i) {
            slot_in_use_[i].store(false, std::memory_order_release);
        }
        next_slot_ = 0;
    }

    bool anySlotInUse() const {
        for (int i = 0; i < kSurfacePoolSize; ++i) {
            if (slot_in_use_[i].load(std::memory_order_acquire)) return true;
        }
        return false;
    }

protected:
    std::array<std::atomic<bool>, kSurfacePoolSize> slot_in_use_{ {false, false, false} };
    int next_slot_ = 0;
};

}  // namespace electron_mpv
