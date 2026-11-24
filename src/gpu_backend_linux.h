#pragma once

#ifdef __linux__

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "gpu_backend.h"

namespace electron_mpv {

// Minimal self-contained EGL / GLES2 / GBM definitions so Linux builds have zero
// compile-time dependency on libgbm-dev, libegl-dev, or libgles2-mesa-dev.
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLImageKHR;
typedef void* EGLClientBuffer;
typedef void* EGLNativeDisplayType;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t EGLint;
typedef intptr_t EGLAttrib;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;

struct gbm_device;
struct gbm_bo;

constexpr EGLDisplay kLinuxEglNoDisplay = nullptr;
constexpr EGLContext kLinuxEglNoContext = nullptr;
constexpr EGLSurface kLinuxEglNoSurface = nullptr;
constexpr EGLImageKHR kLinuxEglNoImage = nullptr;
constexpr EGLConfig kLinuxEglNoConfig = nullptr;

constexpr EGLint kLinuxEglNone = 0x3038;
constexpr EGLint kLinuxEglRedSize = 0x3024;
constexpr EGLint kLinuxEglGreenSize = 0x3023;
constexpr EGLint kLinuxEglBlueSize = 0x3022;
constexpr EGLint kLinuxEglAlphaSize = 0x3021;
constexpr EGLint kLinuxEglRenderableType = 0x3040;
constexpr EGLint kLinuxEglOpenglBit = 0x0008;
constexpr EGLint kLinuxEglOpenglEs3Bit = 0x0040;
constexpr EGLint kLinuxEglExtensions = 0x3055;
constexpr EGLint kLinuxEglWidth = 0x3057;
constexpr EGLint kLinuxEglHeight = 0x3056;
constexpr EGLint kLinuxEglContextClientVersion = 0x3098;
constexpr EGLint kLinuxEglContextMajorVersion = 0x3098;
constexpr EGLint kLinuxEglContextMinorVersion = 0x30FB;
constexpr EGLint kLinuxEglContextOpenglProfileMask = 0x30FD;
constexpr EGLint kLinuxEglContextOpenglCoreProfileBit = 0x00000001;
constexpr EGLenum kLinuxEglOpenglApi = 0x30A2;
constexpr EGLenum kLinuxEglOpenglEsApi = 0x30A0;
constexpr EGLenum kLinuxEglPlatformGbmKhr = 0x31D7;
constexpr EGLenum kLinuxEglLinuxDmaBufExt = 0x3270;
constexpr EGLint kLinuxEglLinuxDrmFourccExt = 0x3271;
constexpr EGLint kLinuxEglDmaBufPlane0FdExt = 0x3272;
constexpr EGLint kLinuxEglDmaBufPlane0OffsetExt = 0x3273;
constexpr EGLint kLinuxEglDmaBufPlane0PitchExt = 0x3274;
constexpr EGLint kLinuxEglDmaBufPlane0ModifierLoExt = 0x3443;
constexpr EGLint kLinuxEglDmaBufPlane0ModifierHiExt = 0x3444;

constexpr GLenum kLinuxGlTexture2d = 0x0DE1;
constexpr GLenum kLinuxGlTextureMinFilter = 0x2801;
constexpr GLenum kLinuxGlTextureMagFilter = 0x2800;
constexpr GLenum kLinuxGlTextureWrapS = 0x2802;
constexpr GLenum kLinuxGlTextureWrapT = 0x2803;
constexpr GLint kLinuxGlLinear = 0x2601;
constexpr GLint kLinuxGlClampToEdge = 0x812F;
constexpr GLenum kLinuxGlFramebuffer = 0x8D40;
constexpr GLenum kLinuxGlColorAttachment0 = 0x8CE0;
constexpr GLenum kLinuxGlFramebufferComplete = 0x8CD5;
constexpr GLint kLinuxGlRgba8 = 0x8058;
constexpr GLint kLinuxGlRgba16f = 0x881A;

constexpr uint64_t kDrmFormatModInvalid = ((1ULL << 56) - 1);
constexpr uint64_t kDrmFormatModLinear = 0ULL;
constexpr uint32_t kGbmFormatArgb8888 = 0x34325241U;        // fourcc('A','R','2','4')
constexpr uint32_t kGbmFormatAbgr8888 = 0x34324241U;        // fourcc('A','B','2','4')
constexpr uint32_t kGbmFormatAbgr16161616f = 0x48344241U;   // fourcc('A','B','4','H')
constexpr uint32_t kGbmBoUseRendering = (1U << 2);
constexpr uint32_t kGbmBoUseLinear = (1U << 4);

typedef EGLDisplay (*PfnEglGetPlatformDisplayExt)(EGLenum, void*, const EGLint*);
typedef EGLImageKHR (*PfnEglCreateImageKhr)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
typedef EGLBoolean (*PfnEglDestroyImageKhr)(EGLDisplay, EGLImageKHR);
typedef void (*PfnGlEglImageTargetTexture2dOes)(GLenum, EGLImageKHR);

class LinuxDmaBufBackend final : public RenderBackend {
public:
    const char* name() const override { return "egl-dmabuf"; }
    bool supportsSharedTexture() const override { return true; }
    bool supportsHdr() const override { return hdr_supported_; }

    bool init(const MpvApi& api, mpv_handle* mpv, mpv_render_context** out_ctx, std::string& out_error) override {
        if (!loadLibs(out_error)) return false;

        // Open DRM render node for headless GBM + zero-copy VAAPI hwdec.
        // Honor optional ELECTRON_LIBMPV_DRM_DEVICE override for multi-GPU systems.
        std::vector<std::string> drm_candidates;
        const char* env_drm = std::getenv("ELECTRON_LIBMPV_DRM_DEVICE");
        if (env_drm && env_drm[0] != '\0') {
            drm_candidates.push_back(env_drm);
        }
        for (int idx = 128; idx <= 135; ++idx) {
            drm_candidates.push_back("/dev/dri/renderD" + std::to_string(idx));
        }

        for (const auto& path : drm_candidates) {
            int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                gbm_device* dev = gbm_create_device_(fd);
                if (dev) {
                    drm_fd_ = fd;
                    gbm_dev_ = dev;
                    break;
                }
                close(fd);
            }
        }

        if (!gbm_dev_) {
            out_error = "Failed to open any /dev/dri/renderD* GBM device";
            cleanupContext();
            return false;
        }

        // Create GBM EGL display.
        if (eglGetPlatformDisplay_) {
            egl_display_ = eglGetPlatformDisplay_(kLinuxEglPlatformGbmKhr, gbm_dev_, nullptr);
        }
        if (egl_display_ == kLinuxEglNoDisplay && eglGetPlatformDisplayEXT_) {
            egl_display_ = eglGetPlatformDisplayEXT_(kLinuxEglPlatformGbmKhr, gbm_dev_, nullptr);
        }
        if (egl_display_ == kLinuxEglNoDisplay) {
            egl_display_ = eglGetDisplay_(reinterpret_cast<EGLNativeDisplayType>(gbm_dev_));
        }

        if (egl_display_ == kLinuxEglNoDisplay) {
            out_error = "Failed to get GBM EGLDisplay";
            cleanupContext();
            return false;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize_(egl_display_, &major, &minor)) {
            out_error = "eglInitialize failed";
            cleanupContext();
            return false;
        }

        const char* exts = eglQueryString_(egl_display_, kLinuxEglExtensions);
        std::string ext_str = exts ? exts : "";
        has_dmabuf_modifiers_ = ext_str.find("EGL_EXT_image_dma_buf_import_modifiers") != std::string::npos;
        const bool has_no_config = ext_str.find("EGL_KHR_no_config_context") != std::string::npos;

        if (ext_str.find("EGL_EXT_image_dma_buf_import") == std::string::npos) {
            out_error = "EGL_EXT_image_dma_buf_import not supported";
            cleanupContext();
            return false;
        }

        // Prefer Desktop OpenGL first (best VAAPI + libmpv compatibility), fall back to GLES 3.0.
        // Note: Mesa GBM does not advertise EGL_PBUFFER_BIT configs; use EGL_NO_CONFIG_KHR when available.
        auto tryCreateContext = [&](EGLenum api_enum, EGLint renderable_bit, const EGLint* ctx_attribs, const EGLint* fallback_ctx_attribs) -> bool {
            if (!eglBindAPI_(api_enum)) return false;

            if (has_no_config) {
                egl_context_ = eglCreateContext_(egl_display_, kLinuxEglNoConfig, kLinuxEglNoContext, ctx_attribs);
                if (egl_context_ == kLinuxEglNoContext && fallback_ctx_attribs) {
                    egl_context_ = eglCreateContext_(egl_display_, kLinuxEglNoConfig, kLinuxEglNoContext, fallback_ctx_attribs);
                }
                if (egl_context_ != kLinuxEglNoContext) return true;
            }

            const EGLint cfg_attribs[] = {
                kLinuxEglRenderableType, renderable_bit,
                kLinuxEglRedSize, 8,
                kLinuxEglGreenSize, 8,
                kLinuxEglBlueSize, 8,
                kLinuxEglAlphaSize, 8,
                kLinuxEglNone
            };
            EGLConfig cfg = nullptr;
            EGLint num_cfg = 0;
            if (eglChooseConfig_(egl_display_, cfg_attribs, &cfg, 1, &num_cfg) && num_cfg > 0) {
                egl_context_ = eglCreateContext_(egl_display_, cfg, kLinuxEglNoContext, ctx_attribs);
                if (egl_context_ == kLinuxEglNoContext && fallback_ctx_attribs) {
                    egl_context_ = eglCreateContext_(egl_display_, cfg, kLinuxEglNoContext, fallback_ctx_attribs);
                }
                if (egl_context_ != kLinuxEglNoContext) return true;
            }
            return false;
        };

        bool ctx_ok = false;
        if (gl_lib_) {
            static const EGLint gl_ctx_attribs[] = {
                kLinuxEglContextMajorVersion, 3,
                kLinuxEglContextMinorVersion, 2,
                kLinuxEglContextOpenglProfileMask, kLinuxEglContextOpenglCoreProfileBit,
                kLinuxEglNone
            };
            static const EGLint gl_fallback_attribs[] = { kLinuxEglNone };
            if (tryCreateContext(kLinuxEglOpenglApi, kLinuxEglOpenglBit, gl_ctx_attribs, gl_fallback_attribs)) {
                using_gles_ = false;
                ctx_ok = true;
            }
        }

        if (!ctx_ok && gles_lib_) {
            static const EGLint gles_ctx_attribs[] = {
                kLinuxEglContextClientVersion, 3,
                kLinuxEglNone
            };
            if (tryCreateContext(kLinuxEglOpenglEsApi, kLinuxEglOpenglEs3Bit, gles_ctx_attribs, nullptr)) {
                using_gles_ = true;
                ctx_ok = true;
            }
        }

        if (!ctx_ok || !eglMakeCurrent_(egl_display_, kLinuxEglNoSurface, kLinuxEglNoSurface, egl_context_)) {
            out_error = "Failed to create or bind surfaceless EGL context";
            cleanupContext();
            return false;
        }

        if (!resolveGlFunctions(out_error)) {
            cleanupContext();
            return false;
        }

        mpv_opengl_init_params gl_init = {
            &LinuxDmaBufBackend::getProcAddress,
            this
        };
        mpv_opengl_drm_params_v2 drm_params = {
            -1,
            0,
            0,
            nullptr,
            drm_fd_
        };
        char api_type[] = MPV_RENDER_API_TYPE_OPENGL;

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_API_TYPE, api_type },
            { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
            { MPV_RENDER_PARAM_DRM_DISPLAY_V2, &drm_params },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        int err = api.render_context_create(out_ctx, mpv, params);
        if (err < 0) {
            // Fallback without DRM_DISPLAY_V2 in case an older libmpv rejects it.
            mpv_render_param fallback_params[] = {
                { MPV_RENDER_PARAM_API_TYPE, api_type },
                { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
                { MPV_RENDER_PARAM_INVALID, nullptr },
            };
            err = api.render_context_create(out_ctx, mpv, fallback_params);
        }
        if (err < 0) {
            out_error = std::string("mpv_render_context_create (Linux EGL) failed: ") + api.error_string(err);
            cleanupContext();
            return false;
        }

        // Probe whether 16-bit float GBM allocation works for HDR.
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

        destroySurfaces();
        resetSlots();

        width_ = width;
        height_ = height;
        hdr_active_ = use_hdr;

        for (int i = 0; i < kSurfacePoolSize; ++i) {
            if (!createSlot(slots_[i], width_, height_, hdr_active_, out_error)) {
                if (hdr_active_) {
                    // Gracefully fall back to 8-bit SDR if 16F DMA-BUF import fails on this GPU.
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
            out_error = "Linux DMA-BUF surfaces not ready";
            return false;
        }

        const SurfaceSlot& s = slots_[slot];
        mpv_opengl_fbo fbo = {
            static_cast<int>(s.fbo),
            width_,
            height_,
            s.gl_internal_format
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
            out_error = std::string("mpv_render_context_render (Linux EGL) failed: ") + api.error_string(err);
            return false;
        }

        // Ensure GPU writes to the DMA-BUF are visible before Chromium's GPU process imports it.
        glFinish_();
        api.render_context_report_swap(ctx);

        out_frame.type = FrameHandleType::LinuxNativePixmap;
        out_frame.slot_id = slot;
        out_frame.width = width_;
        out_frame.height = height_;
        out_frame.pixel_format = s.pixel_format;
        out_frame.hdr = hdr_active_;
        out_frame.dmabuf_fd = s.dmabuf_fd;
        out_frame.stride = s.stride;
        out_frame.offset = s.offset;
        out_frame.size = s.size;
        out_frame.modifier = s.modifier;
        out_frame.supports_zero_copy_webgpu = false;

        auto now = std::chrono::steady_clock::now().time_since_epoch();
        out_frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return true;
    }

    struct ExportedSlot {
        int dmabuf_fd = -1;
        uint32_t stride = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t modifier = kDrmFormatModInvalid;
        std::string pixel_format = "bgra";
    };

    bool getExportedSlot(int slot, ExportedSlot& out) const {
        if (!surfaces_ready_ || slot < 0 || slot >= kSurfacePoolSize) return false;
        const SurfaceSlot& s = slots_[slot];
        out.dmabuf_fd = s.dmabuf_fd;
        out.stride = s.stride;
        out.offset = s.offset;
        out.size = s.size;
        out.modifier = s.modifier;
        out.pixel_format = s.pixel_format;
        return s.dmabuf_fd >= 0;
    }

    int currentWidth() const { return width_; }
    int currentHeight() const { return height_; }
    bool currentHdr() const { return hdr_active_; }

    void shutdown(const MpvApi& api, mpv_render_context* ctx) override {
        if (egl_display_ != kLinuxEglNoDisplay && egl_context_ != kLinuxEglNoContext) {
            eglMakeCurrent_(egl_display_, kLinuxEglNoSurface, kLinuxEglNoSurface, egl_context_);
        }
        if (ctx) {
            api.render_context_free(ctx);
        }
        destroySurfaces();
        cleanupContext();
    }

private:
    struct SurfaceSlot {
        gbm_bo* bo = nullptr;
        int dmabuf_fd = -1;
        uint32_t stride = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t modifier = kDrmFormatModInvalid;
        EGLImageKHR egl_image = kLinuxEglNoImage;
        GLuint texture = 0;
        GLuint fbo = 0;
        int gl_internal_format = kLinuxGlRgba8;
        std::string pixel_format = "bgra";
    };

    static void* getProcAddress(void* ctx, const char* name) {
        auto* self = static_cast<LinuxDmaBufBackend*>(ctx);
        if (!self || !name) return nullptr;
        void* sym = self->eglGetProcAddress_ ? reinterpret_cast<void*>(self->eglGetProcAddress_(name)) : nullptr;
        if (sym) return sym;
        void* primary = self->using_gles_ ? self->gles_lib_ : self->gl_lib_;
        if (primary) {
            sym = dlsym(primary, name);
            if (sym) return sym;
        }
        if (self->egl_lib_) {
            sym = dlsym(self->egl_lib_, name);
            if (sym) return sym;
        }
        return nullptr;
    }

    bool loadLibs(std::string& out_error) {
        gbm_lib_ = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!gbm_lib_) gbm_lib_ = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
        if (!gbm_lib_) {
            out_error = "Failed to dlopen libgbm.so.1";
            return false;
        }

        egl_lib_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!egl_lib_) egl_lib_ = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        if (!egl_lib_) {
            out_error = "Failed to dlopen libEGL.so.1";
            return false;
        }

        gl_lib_ = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!gl_lib_) gl_lib_ = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        gles_lib_ = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!gl_lib_ && !gles_lib_) {
            out_error = "Failed to dlopen libGL.so.1 or libGLESv2.so.2";
            return false;
        }

        gbm_create_device_ = reinterpret_cast<decltype(gbm_create_device_)>(dlsym(gbm_lib_, "gbm_create_device"));
        gbm_device_destroy_ = reinterpret_cast<decltype(gbm_device_destroy_)>(dlsym(gbm_lib_, "gbm_device_destroy"));
        gbm_bo_create_ = reinterpret_cast<decltype(gbm_bo_create_)>(dlsym(gbm_lib_, "gbm_bo_create"));
        gbm_bo_create_with_modifiers_ = reinterpret_cast<decltype(gbm_bo_create_with_modifiers_)>(dlsym(gbm_lib_, "gbm_bo_create_with_modifiers"));
        gbm_bo_destroy_ = reinterpret_cast<decltype(gbm_bo_destroy_)>(dlsym(gbm_lib_, "gbm_bo_destroy"));
        gbm_bo_get_fd_ = reinterpret_cast<decltype(gbm_bo_get_fd_)>(dlsym(gbm_lib_, "gbm_bo_get_fd"));
        gbm_bo_get_stride_ = reinterpret_cast<decltype(gbm_bo_get_stride_)>(dlsym(gbm_lib_, "gbm_bo_get_stride"));
        gbm_bo_get_offset_ = reinterpret_cast<decltype(gbm_bo_get_offset_)>(dlsym(gbm_lib_, "gbm_bo_get_offset"));
        gbm_bo_get_modifier_ = reinterpret_cast<decltype(gbm_bo_get_modifier_)>(dlsym(gbm_lib_, "gbm_bo_get_modifier"));

        eglGetDisplay_ = reinterpret_cast<decltype(eglGetDisplay_)>(dlsym(egl_lib_, "eglGetDisplay"));
        eglGetPlatformDisplay_ = reinterpret_cast<decltype(eglGetPlatformDisplay_)>(dlsym(egl_lib_, "eglGetPlatformDisplay"));
        eglInitialize_ = reinterpret_cast<decltype(eglInitialize_)>(dlsym(egl_lib_, "eglInitialize"));
        eglTerminate_ = reinterpret_cast<decltype(eglTerminate_)>(dlsym(egl_lib_, "eglTerminate"));
        eglBindAPI_ = reinterpret_cast<decltype(eglBindAPI_)>(dlsym(egl_lib_, "eglBindAPI"));
        eglChooseConfig_ = reinterpret_cast<decltype(eglChooseConfig_)>(dlsym(egl_lib_, "eglChooseConfig"));
        eglCreateContext_ = reinterpret_cast<decltype(eglCreateContext_)>(dlsym(egl_lib_, "eglCreateContext"));
        eglDestroyContext_ = reinterpret_cast<decltype(eglDestroyContext_)>(dlsym(egl_lib_, "eglDestroyContext"));
        eglMakeCurrent_ = reinterpret_cast<decltype(eglMakeCurrent_)>(dlsym(egl_lib_, "eglMakeCurrent"));
        eglQueryString_ = reinterpret_cast<decltype(eglQueryString_)>(dlsym(egl_lib_, "eglQueryString"));
        eglGetProcAddress_ = reinterpret_cast<decltype(eglGetProcAddress_)>(dlsym(egl_lib_, "eglGetProcAddress"));

        if (!gbm_create_device_ || !gbm_device_destroy_ || !gbm_bo_create_ || !gbm_bo_destroy_ ||
            !gbm_bo_get_fd_ || !gbm_bo_get_stride_ || !eglGetDisplay_ || !eglInitialize_ ||
            !eglTerminate_ || !eglBindAPI_ || !eglChooseConfig_ || !eglCreateContext_ ||
            !eglDestroyContext_ || !eglMakeCurrent_ || !eglQueryString_ || !eglGetProcAddress_) {
            out_error = "Missing required GBM/EGL symbols";
            return false;
        }

        eglGetPlatformDisplayEXT_ = reinterpret_cast<PfnEglGetPlatformDisplayExt>(eglGetProcAddress_("eglGetPlatformDisplayEXT"));
        eglCreateImageKHR_ = reinterpret_cast<PfnEglCreateImageKhr>(eglGetProcAddress_("eglCreateImageKHR"));
        eglDestroyImageKHR_ = reinterpret_cast<PfnEglDestroyImageKhr>(eglGetProcAddress_("eglDestroyImageKHR"));

        if (!eglCreateImageKHR_ || !eglDestroyImageKHR_) {
            out_error = "Missing eglCreateImageKHR/eglDestroyImageKHR";
            return false;
        }
        return true;
    }

    bool resolveGlFunctions(std::string& out_error) {
        glGenTextures_ = reinterpret_cast<decltype(glGenTextures_)>(getProcAddress(this, "glGenTextures"));
        glDeleteTextures_ = reinterpret_cast<decltype(glDeleteTextures_)>(getProcAddress(this, "glDeleteTextures"));
        glBindTexture_ = reinterpret_cast<decltype(glBindTexture_)>(getProcAddress(this, "glBindTexture"));
        glTexParameteri_ = reinterpret_cast<decltype(glTexParameteri_)>(getProcAddress(this, "glTexParameteri"));
        glGenFramebuffers_ = reinterpret_cast<decltype(glGenFramebuffers_)>(getProcAddress(this, "glGenFramebuffers"));
        glDeleteFramebuffers_ = reinterpret_cast<decltype(glDeleteFramebuffers_)>(getProcAddress(this, "glDeleteFramebuffers"));
        glBindFramebuffer_ = reinterpret_cast<decltype(glBindFramebuffer_)>(getProcAddress(this, "glBindFramebuffer"));
        glFramebufferTexture2D_ = reinterpret_cast<decltype(glFramebufferTexture2D_)>(getProcAddress(this, "glFramebufferTexture2D"));
        glCheckFramebufferStatus_ = reinterpret_cast<decltype(glCheckFramebufferStatus_)>(getProcAddress(this, "glCheckFramebufferStatus"));
        glFinish_ = reinterpret_cast<decltype(glFinish_)>(getProcAddress(this, "glFinish"));
        glEGLImageTargetTexture2DOES_ = reinterpret_cast<PfnGlEglImageTargetTexture2dOes>(
            getProcAddress(this, "glEGLImageTargetTexture2DOES")
        );

        if (!glGenTextures_ || !glDeleteTextures_ || !glBindTexture_ || !glTexParameteri_ ||
            !glGenFramebuffers_ || !glDeleteFramebuffers_ || !glBindFramebuffer_ ||
            !glFramebufferTexture2D_ || !glCheckFramebufferStatus_ || !glFinish_ ||
            !glEGLImageTargetTexture2DOES_) {
            out_error = "Missing required OpenGL/EGLImage functions";
            return false;
        }
        return true;
    }

    bool probeHdrSupport() {
        SurfaceSlot probe;
        std::string ignore_err;
        bool ok = createSlot(probe, 64, 64, true, ignore_err);
        destroySlot(probe);
        return ok;
    }

    bool createSlot(SurfaceSlot& slot, int w, int h, bool hdr, std::string& out_error) {
        struct FormatCandidate {
            uint32_t gbm_fmt;
            int gl_internal;
            const char* electron_fmt;
        };

        std::vector<FormatCandidate> candidates;
        if (hdr) {
            candidates.push_back({ kGbmFormatAbgr16161616f, kLinuxGlRgba16f, "rgbaf16" });
        } else {
            candidates.push_back({ kGbmFormatArgb8888, kLinuxGlRgba8, "bgra" });
            candidates.push_back({ kGbmFormatAbgr8888, kLinuxGlRgba8, "rgba" });
        }

        for (const auto& cand : candidates) {
            gbm_bo* bo = nullptr;

            // Linear modifier has the highest cross-vendor DMA-BUF compatibility with Chromium.
            if (gbm_bo_create_with_modifiers_) {
                uint64_t linear_mod = kDrmFormatModLinear;
                bo = gbm_bo_create_with_modifiers_(gbm_dev_, w, h, cand.gbm_fmt, &linear_mod, 1);
            }
            if (!bo) {
                bo = gbm_bo_create_(gbm_dev_, w, h, cand.gbm_fmt, kGbmBoUseRendering | kGbmBoUseLinear);
            }
            if (!bo) {
                bo = gbm_bo_create_(gbm_dev_, w, h, cand.gbm_fmt, kGbmBoUseRendering);
            }
            if (!bo) continue;

            int fd = gbm_bo_get_fd_(bo);
            if (fd < 0) {
                gbm_bo_destroy_(bo);
                continue;
            }

            uint32_t stride = gbm_bo_get_stride_(bo);
            uint64_t offset = gbm_bo_get_offset_ ? static_cast<uint64_t>(gbm_bo_get_offset_(bo, 0)) : 0;
            uint64_t modifier = gbm_bo_get_modifier_ ? gbm_bo_get_modifier_(bo) : kDrmFormatModInvalid;
            uint64_t size = static_cast<uint64_t>(stride) * static_cast<uint64_t>(h);

            std::vector<EGLint> attribs = {
                kLinuxEglWidth, w,
                kLinuxEglHeight, h,
                kLinuxEglLinuxDrmFourccExt, static_cast<EGLint>(cand.gbm_fmt),
                kLinuxEglDmaBufPlane0FdExt, fd,
                kLinuxEglDmaBufPlane0OffsetExt, static_cast<EGLint>(offset),
                kLinuxEglDmaBufPlane0PitchExt, static_cast<EGLint>(stride),
            };
            if (has_dmabuf_modifiers_ && modifier != kDrmFormatModInvalid) {
                attribs.push_back(kLinuxEglDmaBufPlane0ModifierLoExt);
                attribs.push_back(static_cast<EGLint>(modifier & 0xffffffffULL));
                attribs.push_back(kLinuxEglDmaBufPlane0ModifierHiExt);
                attribs.push_back(static_cast<EGLint>((modifier >> 32) & 0xffffffffULL));
            }
            attribs.push_back(kLinuxEglNone);

            EGLImageKHR image = eglCreateImageKHR_(
                egl_display_, kLinuxEglNoContext, kLinuxEglLinuxDmaBufExt, nullptr, attribs.data()
            );
            if (image == kLinuxEglNoImage) {
                close(fd);
                gbm_bo_destroy_(bo);
                continue;
            }

            GLuint tex = 0, fbo = 0;
            glGenTextures_(1, &tex);
            glBindTexture_(kLinuxGlTexture2d, tex);
            glTexParameteri_(kLinuxGlTexture2d, kLinuxGlTextureMinFilter, kLinuxGlLinear);
            glTexParameteri_(kLinuxGlTexture2d, kLinuxGlTextureMagFilter, kLinuxGlLinear);
            glTexParameteri_(kLinuxGlTexture2d, kLinuxGlTextureWrapS, kLinuxGlClampToEdge);
            glTexParameteri_(kLinuxGlTexture2d, kLinuxGlTextureWrapT, kLinuxGlClampToEdge);
            glEGLImageTargetTexture2DOES_(kLinuxGlTexture2d, image);
            glBindTexture_(kLinuxGlTexture2d, 0);

            glGenFramebuffers_(1, &fbo);
            glBindFramebuffer_(kLinuxGlFramebuffer, fbo);
            glFramebufferTexture2D_(kLinuxGlFramebuffer, kLinuxGlColorAttachment0, kLinuxGlTexture2d, tex, 0);
            GLenum status = glCheckFramebufferStatus_(kLinuxGlFramebuffer);
            glBindFramebuffer_(kLinuxGlFramebuffer, 0);

            if (status != kLinuxGlFramebufferComplete) {
                glDeleteFramebuffers_(1, &fbo);
                glDeleteTextures_(1, &tex);
                eglDestroyImageKHR_(egl_display_, image);
                close(fd);
                gbm_bo_destroy_(bo);
                continue;
            }

            slot.bo = bo;
            slot.dmabuf_fd = fd;
            slot.stride = stride;
            slot.offset = offset;
            slot.size = size;
            slot.modifier = modifier;
            slot.egl_image = image;
            slot.texture = tex;
            slot.fbo = fbo;
            slot.gl_internal_format = cand.gl_internal;
            slot.pixel_format = cand.electron_fmt;
            return true;
        }

        out_error = "Failed to allocate renderable GBM DMA-BUF framebuffer";
        return false;
    }

    void destroySlot(SurfaceSlot& s) {
        if (s.fbo && glDeleteFramebuffers_) {
            glDeleteFramebuffers_(1, &s.fbo);
            s.fbo = 0;
        }
        if (s.texture && glDeleteTextures_) {
            glDeleteTextures_(1, &s.texture);
            s.texture = 0;
        }
        if (s.egl_image != kLinuxEglNoImage && eglDestroyImageKHR_ && egl_display_ != kLinuxEglNoDisplay) {
            eglDestroyImageKHR_(egl_display_, s.egl_image);
            s.egl_image = kLinuxEglNoImage;
        }
        if (s.dmabuf_fd >= 0) {
            close(s.dmabuf_fd);
            s.dmabuf_fd = -1;
        }
        if (s.bo && gbm_bo_destroy_) {
            gbm_bo_destroy_(s.bo);
            s.bo = nullptr;
        }
    }

    void destroySurfaces() {
        for (int i = 0; i < kSurfacePoolSize; ++i) {
            destroySlot(slots_[i]);
        }
        surfaces_ready_ = false;
    }

    void cleanupContext() {
        if (egl_display_ != kLinuxEglNoDisplay) {
            if (eglMakeCurrent_) eglMakeCurrent_(egl_display_, kLinuxEglNoSurface, kLinuxEglNoSurface, kLinuxEglNoContext);
            if (egl_context_ != kLinuxEglNoContext && eglDestroyContext_) {
                eglDestroyContext_(egl_display_, egl_context_);
                egl_context_ = kLinuxEglNoContext;
            }
            if (eglTerminate_) eglTerminate_(egl_display_);
            egl_display_ = kLinuxEglNoDisplay;
        }
        if (gbm_dev_ && gbm_device_destroy_) {
            gbm_device_destroy_(gbm_dev_);
            gbm_dev_ = nullptr;
        }
        if (drm_fd_ >= 0) {
            close(drm_fd_);
            drm_fd_ = -1;
        }
        if (gles_lib_) { dlclose(gles_lib_); gles_lib_ = nullptr; }
        if (gl_lib_) { dlclose(gl_lib_); gl_lib_ = nullptr; }
        if (egl_lib_) { dlclose(egl_lib_); egl_lib_ = nullptr; }
        if (gbm_lib_) { dlclose(gbm_lib_); gbm_lib_ = nullptr; }
    }

    int drm_fd_ = -1;
    gbm_device* gbm_dev_ = nullptr;
    EGLDisplay egl_display_ = kLinuxEglNoDisplay;
    EGLContext egl_context_ = kLinuxEglNoContext;
    bool using_gles_ = false;
    bool has_dmabuf_modifiers_ = false;
    bool hdr_supported_ = false;
    bool hdr_active_ = false;
    bool surfaces_ready_ = false;
    int width_ = 0;
    int height_ = 0;
    std::array<SurfaceSlot, kSurfacePoolSize> slots_;

    void* gbm_lib_ = nullptr;
    void* egl_lib_ = nullptr;
    void* gl_lib_ = nullptr;
    void* gles_lib_ = nullptr;

    gbm_device* (*gbm_create_device_)(int) = nullptr;
    void (*gbm_device_destroy_)(gbm_device*) = nullptr;
    gbm_bo* (*gbm_bo_create_)(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
    gbm_bo* (*gbm_bo_create_with_modifiers_)(gbm_device*, uint32_t, uint32_t, uint32_t, const uint64_t*, unsigned int) = nullptr;
    void (*gbm_bo_destroy_)(gbm_bo*) = nullptr;
    int (*gbm_bo_get_fd_)(gbm_bo*) = nullptr;
    uint32_t (*gbm_bo_get_stride_)(gbm_bo*) = nullptr;
    uint32_t (*gbm_bo_get_offset_)(gbm_bo*, int) = nullptr;
    uint64_t (*gbm_bo_get_modifier_)(gbm_bo*) = nullptr;

    EGLDisplay (*eglGetDisplay_)(EGLNativeDisplayType) = nullptr;
    EGLDisplay (*eglGetPlatformDisplay_)(EGLenum, void*, const EGLAttrib*) = nullptr;
    PfnEglGetPlatformDisplayExt eglGetPlatformDisplayEXT_ = nullptr;
    EGLBoolean (*eglInitialize_)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*eglTerminate_)(EGLDisplay) = nullptr;
    EGLBoolean (*eglBindAPI_)(EGLenum) = nullptr;
    EGLBoolean (*eglChooseConfig_)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLContext (*eglCreateContext_)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*eglDestroyContext_)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*eglMakeCurrent_)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    const char* (*eglQueryString_)(EGLDisplay, EGLint) = nullptr;
    __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_)(const char*) = nullptr;
    PfnEglCreateImageKhr eglCreateImageKHR_ = nullptr;
    PfnEglDestroyImageKhr eglDestroyImageKHR_ = nullptr;

    void (*glGenTextures_)(GLsizei, GLuint*) = nullptr;
    void (*glDeleteTextures_)(GLsizei, const GLuint*) = nullptr;
    void (*glBindTexture_)(GLenum, GLuint) = nullptr;
    void (*glTexParameteri_)(GLenum, GLenum, GLint) = nullptr;
    void (*glGenFramebuffers_)(GLsizei, GLuint*) = nullptr;
    void (*glDeleteFramebuffers_)(GLsizei, const GLuint*) = nullptr;
    void (*glBindFramebuffer_)(GLenum, GLuint) = nullptr;
    void (*glFramebufferTexture2D_)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum (*glCheckFramebufferStatus_)(GLenum) = nullptr;
    void (*glFinish_)(void) = nullptr;
    PfnGlEglImageTargetTexture2dOes glEGLImageTargetTexture2DOES_ = nullptr;
};

}  // namespace electron_mpv

#endif  // __linux__
