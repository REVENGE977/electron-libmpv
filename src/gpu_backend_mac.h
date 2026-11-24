#pragma once

#ifdef __APPLE__

#define GL_SILENCE_DEPRECATION
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "gpu_backend.h"

namespace electron_mpv {

constexpr uint32_t kPixelFmt32BGRA = 0x42475241U;     // 'BGRA'
constexpr uint32_t kPixelFmt64RGBAHalf = 0x52476841U; // 'RGhA'

class MacIOSurfaceBackend final : public RenderBackend {
public:
    const char* name() const override { return "cgl-iosurface"; }
    bool supportsSharedTexture() const override { return true; }
    bool supportsHdr() const override { return hdr_supported_; }

    bool init(const MpvApi& api, mpv_handle* mpv, mpv_render_context** out_ctx, std::string& out_error) override {
        gl_framework_ = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
        if (!gl_framework_) {
            out_error = "Failed to dlopen OpenGL.framework";
            return false;
        }

        CGLPixelFormatAttribute attrs[] = {
            kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
            kCGLPFAAccelerated,
            kCGLPFAAllowOfflineRenderers,
            static_cast<CGLPixelFormatAttribute>(0)
        };
        CGLPixelFormatObj pix = nullptr;
        GLint npix = 0;
        CGLError cgl_err = CGLChoosePixelFormat(attrs, &pix, &npix);
        if (cgl_err != kCGLNoError || !pix) {
            // Fallback without strict accelerated flag (e.g. VM).
            CGLPixelFormatAttribute fallback_attrs[] = {
                kCGLPFAOpenGLProfile, static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
                static_cast<CGLPixelFormatAttribute>(0)
            };
            cgl_err = CGLChoosePixelFormat(fallback_attrs, &pix, &npix);
        }
        if (cgl_err != kCGLNoError || !pix) {
            out_error = std::string("CGLChoosePixelFormat failed: ") + CGLErrorString(cgl_err);
            cleanupContext();
            return false;
        }

        cgl_err = CGLCreateContext(pix, nullptr, &cgl_ctx_);
        CGLDestroyPixelFormat(pix);
        if (cgl_err != kCGLNoError || !cgl_ctx_) {
            out_error = std::string("CGLCreateContext failed: ") + CGLErrorString(cgl_err);
            cleanupContext();
            return false;
        }

        if (CGLSetCurrentContext(cgl_ctx_) != kCGLNoError) {
            out_error = "CGLSetCurrentContext failed";
            cleanupContext();
            return false;
        }

        mpv_opengl_init_params gl_init = {
            &MacIOSurfaceBackend::getProcAddress,
            this
        };
        char api_type[] = MPV_RENDER_API_TYPE_OPENGL;

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_API_TYPE, api_type },
            { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        int err = api.render_context_create(out_ctx, mpv, params);
        if (err < 0) {
            out_error = std::string("mpv_render_context_create (macOS CGL) failed: ") + api.error_string(err);
            cleanupContext();
            return false;
        }

        hdr_supported_ = probeHdrSupport();
        return true;
    }

    bool ensureSurface(int width, int height, bool hdr, bool& out_recreated, std::string& out_error) override {
        width = width < 16 ? 16 : width;
        height = height < 16 ? 16 : height;
        bool use_hdr = hdr && hdr_supported_;

        if (width == width_ && height == height_ && use_hdr == hdr_active_ && surfaces_ready_) {
            out_recreated = false;
            return true;
        }

        if (cgl_ctx_ && CGLGetCurrentContext() != cgl_ctx_) {
            CGLSetCurrentContext(cgl_ctx_);
        }

        destroySurfaces();
        resetSlots();

        width_ = width;
        height_ = height;
        hdr_active_ = use_hdr;

        for (int i = 0; i < kSurfacePoolSize; ++i) {
            if (!createSlot(slots_[i], width_, height_, hdr_active_, out_error)) {
                if (hdr_active_) {
                    destroySurfaces();
                    hdr_supported_ = false;
                    hdr_active_ = false;
                    return ensureSurface(width, height, false, out_recreated, out_error);
                }
                destroySurfaces();
                out_recreated = false;
                return false;
            }
        }

        surfaces_ready_ = true;
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
        if (!surfaces_ready_ || slot < 0 || slot >= kSurfacePoolSize) {
            out_error = "macOS IOSurface slots not ready";
            return false;
        }

        if (cgl_ctx_ && CGLGetCurrentContext() != cgl_ctx_) {
            CGLSetCurrentContext(cgl_ctx_);
        }

        const SurfaceSlot& s = slots_[slot];
        mpv_opengl_fbo fbo = {
            static_cast<int>(s.fbo),
            width_,
            height_,
            hdr_active_ ? GL_RGBA16F : GL_RGBA8
        };
        int flip_y = 0;
        int depth = hdr_active_ ? 16 : 8;

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_OPENGL_FBO, &fbo },
            { MPV_RENDER_PARAM_FLIP_Y, &flip_y },
            { MPV_RENDER_PARAM_DEPTH, &depth },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        int err = api.render_context_render(ctx, params);
        if (err < 0) {
            out_error = std::string("mpv_render_context_render (macOS CGL) failed: ") + api.error_string(err);
            return false;
        }

        glFinish();
        api.render_context_report_swap(ctx);

        // Retain IOSurface per dispatched frame so a concurrent resize/destroySurfaces()
        // can never free the IOSurfaceRef before Electron's main thread calls importSharedTexture().
        CFRetain(s.surface);

        out_frame.type = FrameHandleType::MacIOSurface;
        out_frame.slot_id = slot;
        out_frame.width = width_;
        out_frame.height = height_;
        out_frame.pixel_format = hdr_active_ ? "rgbaf16" : "bgra";
        out_frame.hdr = hdr_active_;
        out_frame.io_surface = reinterpret_cast<uintptr_t>(s.surface);
        out_frame.owns_io_surface = true;

        auto now = std::chrono::steady_clock::now().time_since_epoch();
        out_frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return true;
    }

    void shutdown(const MpvApi& api, mpv_render_context* ctx) override {
        if (cgl_ctx_) {
            CGLSetCurrentContext(cgl_ctx_);
        }
        if (ctx) {
            api.render_context_free(ctx);
        }
        destroySurfaces();
        cleanupContext();
    }

private:
    struct SurfaceSlot {
        IOSurfaceRef surface = nullptr;
        GLuint texture = 0;
        GLuint fbo = 0;
    };

    static void* getProcAddress(void* ctx, const char* name) {
        auto* self = static_cast<MacIOSurfaceBackend*>(ctx);
        if (!self || !self->gl_framework_ || !name) return nullptr;
        return dlsym(self->gl_framework_, name);
    }

    bool probeHdrSupport() {
        SurfaceSlot probe;
        std::string ignore_err;
        bool ok = createSlot(probe, 64, 64, true, ignore_err);
        destroySlot(probe);
        return ok;
    }

    bool createSlot(SurfaceSlot& slot, int w, int h, bool hdr, std::string& out_error) {
        int bpe = hdr ? 8 : 4;
        int bytes_per_row = static_cast<int>(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, static_cast<size_t>(w) * bpe));
        int alloc_size = bytes_per_row * h;
        int32_t pix_fmt = static_cast<int32_t>(hdr ? kPixelFmt64RGBAHalf : kPixelFmt32BGRA);

        CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
        );
        if (!dict) {
            out_error = "Failed to allocate IOSurface properties dictionary";
            return false;
        }

        auto putInt = [&](CFStringRef key, int32_t value) {
            CFNumberRef num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
            if (num) {
                CFDictionarySetValue(dict, key, num);
                CFRelease(num);
            }
        };

        putInt(kIOSurfaceWidth, w);
        putInt(kIOSurfaceHeight, h);
        putInt(kIOSurfaceBytesPerElement, bpe);
        putInt(kIOSurfaceBytesPerRow, bytes_per_row);
        putInt(kIOSurfaceAllocSize, alloc_size);
        putInt(kIOSurfacePixelFormat, pix_fmt);

        slot.surface = IOSurfaceCreate(dict);
        CFRelease(dict);

        if (!slot.surface) {
            out_error = "IOSurfaceCreate failed";
            return false;
        }

        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_RECTANGLE, slot.texture);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Apple CGLTexImageIOSurface2D expects unsized GL_RGBA for both 32BGRA and 64RGBAHalf
        // (matching Chromium/ANGLE IOSurfaceSurfaceCGL); fall back to sized tokens if needed.
        CGLError err = CGLTexImageIOSurface2D(
            cgl_ctx_,
            GL_TEXTURE_RECTANGLE,
            GL_RGBA,
            w,
            h,
            hdr ? GL_RGBA : GL_BGRA,
            hdr ? GL_HALF_FLOAT : GL_UNSIGNED_INT_8_8_8_8_REV,
            slot.surface,
            0
        );
        if (err != kCGLNoError) {
            err = CGLTexImageIOSurface2D(
                cgl_ctx_,
                GL_TEXTURE_RECTANGLE,
                hdr ? GL_RGBA16F : GL_RGBA8,
                w,
                h,
                hdr ? GL_RGBA : GL_BGRA,
                hdr ? GL_HALF_FLOAT : GL_UNSIGNED_INT_8_8_8_8_REV,
                slot.surface,
                0
            );
        }
        glBindTexture(GL_TEXTURE_RECTANGLE, 0);

        if (err != kCGLNoError) {
            out_error = std::string("CGLTexImageIOSurface2D failed: ") + CGLErrorString(err);
            destroySlot(slot);
            return false;
        }

        glGenFramebuffers(1, &slot.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, slot.texture, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            out_error = "macOS IOSurface FBO is incomplete";
            destroySlot(slot);
            return false;
        }
        return true;
    }

    void destroySlot(SurfaceSlot& s) {
        if (s.fbo) {
            glDeleteFramebuffers(1, &s.fbo);
            s.fbo = 0;
        }
        if (s.texture) {
            glDeleteTextures(1, &s.texture);
            s.texture = 0;
        }
        if (s.surface) {
            CFRelease(s.surface);
            s.surface = nullptr;
        }
    }

    void destroySurfaces() {
        for (int i = 0; i < kSurfacePoolSize; ++i) {
            destroySlot(slots_[i]);
        }
        surfaces_ready_ = false;
    }

    void cleanupContext() {
        if (cgl_ctx_) {
            CGLSetCurrentContext(nullptr);
            CGLDestroyContext(cgl_ctx_);
            cgl_ctx_ = nullptr;
        }
        if (gl_framework_) {
            dlclose(gl_framework_);
            gl_framework_ = nullptr;
        }
    }

    void* gl_framework_ = nullptr;
    CGLContextObj cgl_ctx_ = nullptr;
    bool hdr_supported_ = false;
    bool hdr_active_ = false;
    bool surfaces_ready_ = false;
    int width_ = 0;
    int height_ = 0;
    std::array<SurfaceSlot, kSurfacePoolSize> slots_;
};

}  // namespace electron_mpv

#endif  // __APPLE__
