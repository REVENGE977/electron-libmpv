#pragma once

#include <chrono>
#include <cstring>
#include <vector>

#include "gpu_backend.h"

namespace electron_mpv {

class SoftwareRenderBackend final : public RenderBackend {
public:
    const char* name() const override { return "software"; }
    bool supportsSharedTexture() const override { return false; }
    bool supportsHdr() const override { return false; }

    bool init(const MpvApi& api, mpv_handle* mpv, mpv_render_context** out_ctx, std::string& out_error) override {
        char api_type[] = MPV_RENDER_API_TYPE_SW;
        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_API_TYPE, api_type },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        int err = api.render_context_create(out_ctx, mpv, params);
        if (err < 0) {
            out_error = std::string("mpv_render_context_create (sw) failed: ") + api.error_string(err);
            return false;
        }
        return true;
    }

    bool ensureSurface(int width, int height, bool /*hdr*/, bool& out_recreated, std::string& /*out_error*/) override {
        width = width < 16 ? 16 : width;
        height = height < 16 ? 16 : height;
        if (width == width_ && height == height_) {
            out_recreated = false;
            return true;
        }

        width_ = width;
        height_ = height;
        // 64-byte aligned stride for SIMD efficiency (see mpv/render.h).
        stride_ = ((static_cast<size_t>(width_) * 4 + 63) / 64) * 64;
        resetSlots();
        out_recreated = true;
        return true;
    }

    bool renderFrame(
        const MpvApi& api,
        mpv_render_context* ctx,
        int slot,
        FrameDescriptor& out_frame,
        std::string& out_error
    ) override {
        if (width_ <= 0 || height_ <= 0) {
            out_error = "Invalid software surface dimensions";
            return false;
        }

        out_frame.type = FrameHandleType::SoftwareRgba;
        out_frame.slot_id = slot;
        out_frame.width = width_;
        out_frame.height = height_;
        out_frame.pixel_format = "rgba";
        out_frame.hdr = false;
        out_frame.sw_stride = stride_;
        out_frame.sw_pixels.resize(stride_ * static_cast<size_t>(height_));

        int size[2] = { width_, height_ };
        char fmt[] = "rgb0";
        size_t stride = stride_;
        void* ptr = out_frame.sw_pixels.data();

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_SW_SIZE, size },
            { MPV_RENDER_PARAM_SW_FORMAT, fmt },
            { MPV_RENDER_PARAM_SW_STRIDE, &stride },
            { MPV_RENDER_PARAM_SW_POINTER, ptr },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        int err = api.render_context_render(ctx, params);
        if (err < 0) {
            out_error = std::string("mpv_render_context_render (sw) failed: ") + api.error_string(err);
            return false;
        }

        api.render_context_report_swap(ctx);

        auto now = std::chrono::steady_clock::now().time_since_epoch();
        out_frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return true;
    }

    void shutdown(const MpvApi& api, mpv_render_context* ctx) override {
        if (ctx) {
            api.render_context_free(ctx);
        }
    }

private:
    int width_ = 0;
    int height_ = 0;
    size_t stride_ = 0;
};

}  // namespace electron_mpv
