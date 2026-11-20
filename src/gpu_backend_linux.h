#pragma once

#ifdef __linux__

#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include "gpu_backend.h"

#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif
#ifndef GBM_FORMAT_ABGR16161616F
#define GBM_FORMAT_ABGR16161616F 0x48344241U /* fourcc('A','B','4','H') */
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

namespace electron_mpv {

class LinuxDmaBufBackend final : public RenderBackend {
public:
    const char* name() const override { return "egl-dmabuf"; }
    bool supportsSharedTexture() const override { return true; }
    bool supportsHdr() const override { return hdr_supported_; }

    bool init(const MpvApi& api, mpv_handle* mpv, mpv_render_context** out_ctx, std::string& out_error) override {
        if (!loadLibs(out_error)) return false;

        // Open DRM render node for headless GBM + zero-copy VAAPI hwdec.
        for (int idx = 128; idx <= 135; ++idx) {
            std::string path = "/dev/dri/renderD" + std::to_string(idx);
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
            egl_display_ = eglGetPlatformDisplay_(EGL_PLATFORM_GBM_KHR, gbm_dev_, nullptr);
        }
        if (egl_display_ == EGL_NO_DISPLAY && eglGetPlatformDisplayEXT_) {
            egl_display_ = eglGetPlatformDisplayEXT_(EGL_PLATFORM_GBM_KHR, gbm_dev_, nullptr);
        }
        if (egl_display_ == EGL_NO_DISPLAY) {
            egl_display_ = eglGetDisplay_(reinterpret_cast<EGLNativeDisplayType>(gbm_dev_));
        }

        if (egl_display_ == EGL_NO_DISPLAY) {
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

        const char* exts = eglQueryString_(egl_display_, EGL_EXTENSIONS);
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
                egl_context_ = eglCreateContext_(egl_display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
                if (egl_context_ == EGL_NO_CONTEXT && fallback_ctx_attribs) {
                    egl_context_ = eglCreateContext_(egl_display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, fallback_ctx_attribs);
                }
                if (egl_context_ != EGL_NO_CONTEXT) return true;
            }

            const EGLint cfg_attribs[] = {
                EGL_RENDERABLE_TYPE, renderable_bit,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_NONE
            };
            EGLConfig cfg = nullptr;
            EGLint num_cfg = 0;
            if (eglChooseConfig_(egl_display_, cfg_attribs, &cfg, 1, &num_cfg) && num_cfg > 0) {
                egl_context_ = eglCreateContext_(egl_display_, cfg, EGL_NO_CONTEXT, ctx_attribs);
                if (egl_context_ == EGL_NO_CONTEXT && fallback_ctx_attribs) {
                    egl_context_ = eglCreateContext_(egl_display_, cfg, EGL_NO_CONTEXT, fallback_ctx_attribs);
                }
                if (egl_context_ != EGL_NO_CONTEXT) return true;
            }
            return false;
        };

        bool ctx_ok = false;
        if (gl_lib_) {
            static const EGLint gl_ctx_attribs[] = {
                EGL_CONTEXT_MAJOR_VERSION, 3,
                EGL_CONTEXT_MINOR_VERSION, 2,
                EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                EGL_NONE
            };
            static const EGLint gl_fallback_attribs[] = { EGL_NONE };
            if (tryCreateContext(EGL_OPENGL_API, EGL_OPENGL_BIT, gl_ctx_attribs, gl_fallback_attribs)) {
                using_gles_ = false;
                ctx_ok = true;
            }
        }

        if (!ctx_ok && gles_lib_) {
            static const EGLint gles_ctx_attribs[] = {
                EGL_CONTEXT_CLIENT_VERSION, 3,
                EGL_NONE
            };
            if (tryCreateContext(EGL_OPENGL_ES_API, EGL_OPENGL_ES3_BIT, gles_ctx_attribs, nullptr)) {
                using_gles_ = true;
                ctx_ok = true;
            }
        }

        if (!ctx_ok || !eglMakeCurrent_(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_)) {
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
        char api_type[] = MPV_RENDER_API_TYPE_OPENGL;

        mpv_render_param params[] = {
            { MPV_RENDER_PARAM_API_TYPE, api_type },
            { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
            { MPV_RENDER_PARAM_INVALID, nullptr },
        };

        int err = api.render_context_create(out_ctx, mpv, params);
        if (err < 0) {
            out_error = std::string("mpv_render_context_create (Linux EGL) failed: ") + api.error_string(err);
            cleanupContext();
            return false;
        }

        // Probe whether 16-bit float GBM allocation works for HDR.
        hdr_supported_ = probeHdrSupport();
        return true;
    }

    bool ensureSurface(int width, int height, bool hdr, std::string& out_error) override {
        width = width < 16 ? 16 : width;
        height = height < 16 ? 16 : height;
        bool use_hdr = hdr && hdr_supported_;

        if (width == width_ && height == height_ && use_hdr == hdr_active_ && surfaces_ready_) {
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
                    return ensureSurface(width, height, false, out_error);
                }
                destroySurfaces();
                return false;
            }
        }

        surfaces_ready_ = true;
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
        uint64_t modifier = DRM_FORMAT_MOD_INVALID;
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
        if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent_(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);
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
        uint64_t modifier = DRM_FORMAT_MOD_INVALID;
        EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        GLuint fbo = 0;
        int gl_internal_format = GL_RGBA8;
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

        eglGetPlatformDisplayEXT_ = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress_("eglGetPlatformDisplayEXT"));
        eglCreateImageKHR_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress_("eglCreateImageKHR"));
        eglDestroyImageKHR_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress_("eglDestroyImageKHR"));

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
        glEGLImageTargetTexture2DOES_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
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
            candidates.push_back({ GBM_FORMAT_ABGR16161616F, GL_RGBA16F, "rgbaf16" });
        } else {
            candidates.push_back({ GBM_FORMAT_ARGB8888, GL_RGBA8, "bgra" });
            candidates.push_back({ GBM_FORMAT_ABGR8888, GL_RGBA8, "rgba" });
        }

        for (const auto& cand : candidates) {
            gbm_bo* bo = nullptr;

            // Linear modifier has the highest cross-vendor DMA-BUF compatibility with Chromium.
            if (gbm_bo_create_with_modifiers_) {
                uint64_t linear_mod = DRM_FORMAT_MOD_LINEAR;
                bo = gbm_bo_create_with_modifiers_(gbm_dev_, w, h, cand.gbm_fmt, &linear_mod, 1);
            }
            if (!bo) {
                bo = gbm_bo_create_(gbm_dev_, w, h, cand.gbm_fmt, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            }
            if (!bo) {
                bo = gbm_bo_create_(gbm_dev_, w, h, cand.gbm_fmt, GBM_BO_USE_RENDERING);
            }
            if (!bo) continue;

            int fd = gbm_bo_get_fd_(bo);
            if (fd < 0) {
                gbm_bo_destroy_(bo);
                continue;
            }

            uint32_t stride = gbm_bo_get_stride_(bo);
            uint64_t offset = gbm_bo_get_offset_ ? static_cast<uint64_t>(gbm_bo_get_offset_(bo, 0)) : 0;
            uint64_t modifier = gbm_bo_get_modifier_ ? gbm_bo_get_modifier_(bo) : DRM_FORMAT_MOD_INVALID;
            uint64_t size = static_cast<uint64_t>(stride) * static_cast<uint64_t>(h);

            std::vector<EGLint> attribs = {
                EGL_WIDTH, w,
                EGL_HEIGHT, h,
                EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(cand.gbm_fmt),
                EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(offset),
                EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(stride),
            };
            if (has_dmabuf_modifiers_ && modifier != DRM_FORMAT_MOD_INVALID) {
                attribs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
                attribs.push_back(static_cast<EGLint>(modifier & 0xffffffffULL));
                attribs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
                attribs.push_back(static_cast<EGLint>((modifier >> 32) & 0xffffffffULL));
            }
            attribs.push_back(EGL_NONE);

            EGLImageKHR image = eglCreateImageKHR_(
                egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data()
            );
            if (image == EGL_NO_IMAGE_KHR) {
                close(fd);
                gbm_bo_destroy_(bo);
                continue;
            }

            GLuint tex = 0, fbo = 0;
            glGenTextures_(1, &tex);
            glBindTexture_(GL_TEXTURE_2D, tex);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, image);
            glBindTexture_(GL_TEXTURE_2D, 0);

            glGenFramebuffers_(1, &fbo);
            glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);

            if (status != GL_FRAMEBUFFER_COMPLETE) {
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
        if (s.egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_ && egl_display_ != EGL_NO_DISPLAY) {
            eglDestroyImageKHR_(egl_display_, s.egl_image);
            s.egl_image = EGL_NO_IMAGE_KHR;
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
        if (egl_display_ != EGL_NO_DISPLAY) {
            if (eglMakeCurrent_) eglMakeCurrent_(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (egl_context_ != EGL_NO_CONTEXT && eglDestroyContext_) {
                eglDestroyContext_(egl_display_, egl_context_);
                egl_context_ = EGL_NO_CONTEXT;
            }
            if (eglTerminate_) eglTerminate_(egl_display_);
            egl_display_ = EGL_NO_DISPLAY;
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
    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
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
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT_ = nullptr;
    EGLBoolean (*eglInitialize_)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*eglTerminate_)(EGLDisplay) = nullptr;
    EGLBoolean (*eglBindAPI_)(EGLenum) = nullptr;
    EGLBoolean (*eglChooseConfig_)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLContext (*eglCreateContext_)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*eglDestroyContext_)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*eglMakeCurrent_)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    const char* (*eglQueryString_)(EGLDisplay, EGLint) = nullptr;
    __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_)(const char*) = nullptr;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;

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
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
};

}  // namespace electron_mpv

#endif  // __linux__
