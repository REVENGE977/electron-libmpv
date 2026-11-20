#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "gpu_backend.h"

namespace electron_mpv {

// Minimal EGL/GLES2 types & ANGLE constants so Windows builds have zero external EGL header dependencies.
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLImageKHR;
typedef void* EGLClientBuffer;
typedef void* EGLDeviceEXT;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t EGLint;
typedef intptr_t EGLAttrib;

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;

constexpr EGLDisplay kEglNoDisplay = nullptr;
constexpr EGLContext kEglNoContext = nullptr;
constexpr EGLSurface kEglNoSurface = nullptr;
constexpr EGLImageKHR kEglNoImage = nullptr;
constexpr EGLint kEglNone = 0x3038;
constexpr EGLint kEglRedSize = 0x3024;
constexpr EGLint kEglGreenSize = 0x3023;
constexpr EGLint kEglBlueSize = 0x3022;
constexpr EGLint kEglAlphaSize = 0x3021;
constexpr EGLint kEglSurfaceType = 0x3033;
constexpr EGLint kEglPbufferBit = 0x0001;
constexpr EGLint kEglRenderableType = 0x3040;
constexpr EGLint kEglOpenglEs3Bit = 0x0040;
constexpr EGLint kEglContextClientVersion = 0x3098;
constexpr EGLint kEglExtensions = 0x3055;
constexpr EGLenum kEglOpenglEsApi = 0x30A0;
constexpr EGLint kEglBindToTextureRgba = 0x303A;
constexpr EGLint kEglTextureFormat = 0x3080;
constexpr EGLint kEglTextureTarget = 0x3081;
constexpr EGLint kEglTextureRgba = 0x305E;
constexpr EGLint kEglTexture2d = 0x305F;
constexpr EGLint kEglBackBuffer = 0x3084;
constexpr EGLint kEglWidth = 0x3057;
constexpr EGLint kEglHeight = 0x3056;
constexpr EGLenum kEglPlatformDeviceExt = 0x313F;
constexpr EGLenum kEglD3d11DeviceAngle = 0x33A1;
constexpr EGLenum kEglD3dTextureAngle = 0x33A3;
constexpr EGLenum kEglD3d11TextureAngle = 0x3484;

constexpr GLenum kGlTexture2d = 0x0DE1;
constexpr GLenum kGlTextureMinFilter = 0x2801;
constexpr GLenum kGlTextureMagFilter = 0x2800;
constexpr GLenum kGlTextureWrapS = 0x2802;
constexpr GLenum kGlTextureWrapT = 0x2803;
constexpr GLint kGlLinear = 0x2601;
constexpr GLint kGlClampToEdge = 0x812F;
constexpr GLenum kGlFramebuffer = 0x8D40;
constexpr GLenum kGlColorAttachment0 = 0x8CE0;
constexpr GLenum kGlFramebufferComplete = 0x8CD5;
constexpr GLint kGlRgba8 = 0x8058;
constexpr GLint kGlRgba16f = 0x881A;

class WinAngleD3D11Backend final : public RenderBackend {
public:
    const char* name() const override { return "angle-d3d11"; }
    bool supportsSharedTexture() const override { return true; }
    bool supportsHdr() const override { return hdr_supported_; }

    bool init(const MpvApi& api, mpv_handle* mpv, mpv_render_context** out_ctx, std::string& out_error) override {
        if (!loadAngleLibs(out_error)) return false;

        UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL chosen_level = D3D_FEATURE_LEVEL_11_0;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            create_flags,
            feature_levels,
            static_cast<UINT>(sizeof(feature_levels) / sizeof(feature_levels[0])),
            D3D11_SDK_VERSION,
            &d3d_device_,
            &chosen_level,
            &d3d_context_
        );
        if (FAILED(hr)) {
            // Retry without D3D_FEATURE_LEVEL_11_1 on older WDDM stacks.
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &feature_levels[1],
                1,
                D3D11_SDK_VERSION,
                &d3d_device_,
                &chosen_level,
                &d3d_context_
            );
        }
        if (FAILED(hr) || !d3d_device_ || !d3d_context_) {
            out_error = "D3D11CreateDevice failed";
            cleanupContext();
            return false;
        }

        // Enable multithread protection for zero-copy D3D11VA hardware decoding.
        ID3D11Multithread* mt = nullptr;
        if (SUCCEEDED(d3d_device_->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt))) && mt) {
            mt->SetMultithreadProtected(TRUE);
            mt->Release();
        }

        egl_device_ = eglCreateDeviceANGLE_(kEglD3d11DeviceAngle, d3d_device_, nullptr);
        if (!egl_device_) {
            out_error = "eglCreateDeviceANGLE(EGL_D3D11_DEVICE_ANGLE) failed";
            cleanupContext();
            return false;
        }

        egl_display_ = eglGetPlatformDisplayEXT_(kEglPlatformDeviceExt, egl_device_, nullptr);
        if (egl_display_ == kEglNoDisplay) {
            out_error = "eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT) failed";
            cleanupContext();
            return false;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize_(egl_display_, &major, &minor)) {
            out_error = "ANGLE eglInitialize failed";
            cleanupContext();
            return false;
        }

        const char* exts = eglQueryString_(egl_display_, kEglExtensions);
        std::string ext_str = exts ? exts : "";
        has_d3d11_image_ = ext_str.find("EGL_ANGLE_image_d3d11_texture") != std::string::npos;
        has_d3d_pbuffer_ = ext_str.find("EGL_ANGLE_d3d_texture_client_buffer") != std::string::npos;

        if (!has_d3d11_image_ && !has_d3d_pbuffer_) {
            out_error = "ANGLE lacks both EGL_ANGLE_image_d3d11_texture and EGL_ANGLE_d3d_texture_client_buffer";
            cleanupContext();
            return false;
        }

        eglBindAPI_(kEglOpenglEsApi);

        static const EGLint cfg_attribs[] = {
            kEglSurfaceType, kEglPbufferBit,
            kEglRenderableType, kEglOpenglEs3Bit,
            kEglRedSize, 8,
            kEglGreenSize, 8,
            kEglBlueSize, 8,
            kEglAlphaSize, 8,
            kEglBindToTextureRgba, 1,
            kEglNone
        };
        EGLint num_cfg = 0;
        if (!eglChooseConfig_(egl_display_, cfg_attribs, &egl_config_, 1, &num_cfg) || num_cfg == 0) {
            out_error = "ANGLE eglChooseConfig failed";
            cleanupContext();
            return false;
        }

        static const EGLint ctx_attribs[] = {
            kEglContextClientVersion, 3,
            kEglNone
        };
        egl_context_ = eglCreateContext_(egl_display_, egl_config_, kEglNoContext, ctx_attribs);
        if (egl_context_ == kEglNoContext) {
            out_error = "ANGLE eglCreateContext (ES3) failed";
            cleanupContext();
            return false;
        }

        // Create a tiny dummy pbuffer surface so MakeCurrent succeeds even without surfaceless extension.
        static const EGLint pbuf_attribs[] = { kEglWidth, 1, kEglHeight, 1, kEglNone };
        dummy_surface_ = eglCreatePbufferSurface_(egl_display_, egl_config_, pbuf_attribs);

        if (!eglMakeCurrent_(egl_display_, dummy_surface_, dummy_surface_, egl_context_)) {
            out_error = "ANGLE eglMakeCurrent failed";
            cleanupContext();
            return false;
        }

        if (!resolveGlFunctions(out_error)) {
            cleanupContext();
            return false;
        }

        mpv_opengl_init_params gl_init = {
            &WinAngleD3D11Backend::getProcAddress,
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
            out_error = std::string("mpv_render_context_create (Windows ANGLE) failed: ") + api.error_string(err);
            cleanupContext();
            return false;
        }

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
            out_error = "Windows D3D11 surfaces not ready";
            return false;
        }

        const SurfaceSlot& s = slots_[slot];
        mpv_opengl_fbo fbo = {
            static_cast<int>(s.fbo),
            width_,
            height_,
            hdr_active_ ? kGlRgba16f : kGlRgba8
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
            out_error = std::string("mpv_render_context_render (Windows ANGLE) failed: ") + api.error_string(err);
            return false;
        }

        glFinish_();
        d3d_context_->Flush();
        api.render_context_report_swap(ctx);

        out_frame.type = FrameHandleType::WinNtHandle;
        out_frame.slot_id = slot;
        out_frame.width = width_;
        out_frame.height = height_;
        out_frame.pixel_format = hdr_active_ ? "rgbaf16" : "bgra";
        out_frame.hdr = hdr_active_;
        out_frame.nt_handle = reinterpret_cast<uintptr_t>(s.nt_handle);

        auto now = std::chrono::steady_clock::now().time_since_epoch();
        out_frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return true;
    }

    void shutdown(const MpvApi& api, mpv_render_context* ctx) override {
        if (egl_display_ != kEglNoDisplay && egl_context_ != kEglNoContext) {
            eglMakeCurrent_(egl_display_, dummy_surface_, dummy_surface_, egl_context_);
        }
        if (ctx) {
            api.render_context_free(ctx);
        }
        destroySurfaces();
        cleanupContext();
    }

private:
    struct SurfaceSlot {
        ID3D11Texture2D* d3d_tex = nullptr;
        HANDLE nt_handle = nullptr;
        EGLImageKHR egl_image = kEglNoImage;
        EGLSurface pbuffer = kEglNoSurface;
        GLuint texture = 0;
        GLuint fbo = 0;
    };

    static void* getProcAddress(void* ctx, const char* name) {
        auto* self = static_cast<WinAngleD3D11Backend*>(ctx);
        if (!self || !name) return nullptr;
        void* sym = self->eglGetProcAddress_ ? reinterpret_cast<void*>(self->eglGetProcAddress_(name)) : nullptr;
        if (sym) return sym;
        if (self->gles_lib_) {
            sym = reinterpret_cast<void*>(GetProcAddress(self->gles_lib_, name));
            if (sym) return sym;
        }
        if (self->egl_lib_) {
            sym = reinterpret_cast<void*>(GetProcAddress(self->egl_lib_, name));
            if (sym) return sym;
        }
        return nullptr;
    }

    bool loadAngleLibs(std::string& out_error) {
        // Electron bundles libEGL.dll and libGLESv2.dll next to electron.exe.
        char exe_buf[MAX_PATH] = {0};
        std::string exe_dir;
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            exe_dir = exe_buf;
            size_t slash = exe_dir.find_last_of("\\/");
            if (slash != std::string::npos) exe_dir = exe_dir.substr(0, slash + 1);
        }

        egl_lib_ = LoadLibraryA((exe_dir + "libEGL.dll").c_str());
        if (!egl_lib_) egl_lib_ = LoadLibraryA("libEGL.dll");
        gles_lib_ = LoadLibraryA((exe_dir + "libGLESv2.dll").c_str());
        if (!gles_lib_) gles_lib_ = LoadLibraryA("libGLESv2.dll");

        if (!egl_lib_ || !gles_lib_) {
            out_error = "Failed to load Electron's bundled ANGLE (libEGL.dll / libGLESv2.dll)";
            return false;
        }

        eglInitialize_ = reinterpret_cast<decltype(eglInitialize_)>(GetProcAddress(egl_lib_, "eglInitialize"));
        eglTerminate_ = reinterpret_cast<decltype(eglTerminate_)>(GetProcAddress(egl_lib_, "eglTerminate"));
        eglBindAPI_ = reinterpret_cast<decltype(eglBindAPI_)>(GetProcAddress(egl_lib_, "eglBindAPI"));
        eglChooseConfig_ = reinterpret_cast<decltype(eglChooseConfig_)>(GetProcAddress(egl_lib_, "eglChooseConfig"));
        eglCreateContext_ = reinterpret_cast<decltype(eglCreateContext_)>(GetProcAddress(egl_lib_, "eglCreateContext"));
        eglDestroyContext_ = reinterpret_cast<decltype(eglDestroyContext_)>(GetProcAddress(egl_lib_, "eglDestroyContext"));
        eglMakeCurrent_ = reinterpret_cast<decltype(eglMakeCurrent_)>(GetProcAddress(egl_lib_, "eglMakeCurrent"));
        eglQueryString_ = reinterpret_cast<decltype(eglQueryString_)>(GetProcAddress(egl_lib_, "eglQueryString"));
        eglGetProcAddress_ = reinterpret_cast<decltype(eglGetProcAddress_)>(GetProcAddress(egl_lib_, "eglGetProcAddress"));
        eglCreatePbufferSurface_ = reinterpret_cast<decltype(eglCreatePbufferSurface_)>(GetProcAddress(egl_lib_, "eglCreatePbufferSurface"));
        eglCreatePbufferFromClientBuffer_ = reinterpret_cast<decltype(eglCreatePbufferFromClientBuffer_)>(GetProcAddress(egl_lib_, "eglCreatePbufferFromClientBuffer"));
        eglDestroySurface_ = reinterpret_cast<decltype(eglDestroySurface_)>(GetProcAddress(egl_lib_, "eglDestroySurface"));
        eglBindTexImage_ = reinterpret_cast<decltype(eglBindTexImage_)>(GetProcAddress(egl_lib_, "eglBindTexImage"));

        if (!eglInitialize_ || !eglTerminate_ || !eglBindAPI_ || !eglChooseConfig_ ||
            !eglCreateContext_ || !eglDestroyContext_ || !eglMakeCurrent_ || !eglQueryString_ ||
            !eglGetProcAddress_ || !eglCreatePbufferSurface_ || !eglDestroySurface_) {
            out_error = "Missing core EGL symbols in libEGL.dll";
            return false;
        }

        eglCreateDeviceANGLE_ = reinterpret_cast<decltype(eglCreateDeviceANGLE_)>(eglGetProcAddress_("eglCreateDeviceANGLE"));
        eglReleaseDeviceANGLE_ = reinterpret_cast<decltype(eglReleaseDeviceANGLE_)>(eglGetProcAddress_("eglReleaseDeviceANGLE"));
        eglGetPlatformDisplayEXT_ = reinterpret_cast<decltype(eglGetPlatformDisplayEXT_)>(eglGetProcAddress_("eglGetPlatformDisplayEXT"));
        eglCreateImageKHR_ = reinterpret_cast<decltype(eglCreateImageKHR_)>(eglGetProcAddress_("eglCreateImageKHR"));
        eglDestroyImageKHR_ = reinterpret_cast<decltype(eglDestroyImageKHR_)>(eglGetProcAddress_("eglDestroyImageKHR"));

        if (!eglCreateDeviceANGLE_ || !eglGetPlatformDisplayEXT_) {
            out_error = "Missing ANGLE device extensions in libEGL.dll";
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
        glEGLImageTargetTexture2DOES_ = reinterpret_cast<decltype(glEGLImageTargetTexture2DOES_)>(
            getProcAddress(this, "glEGLImageTargetTexture2DOES")
        );

        if (!glGenTextures_ || !glDeleteTextures_ || !glBindTexture_ || !glTexParameteri_ ||
            !glGenFramebuffers_ || !glDeleteFramebuffers_ || !glBindFramebuffer_ ||
            !glFramebufferTexture2D_ || !glCheckFramebufferStatus_ || !glFinish_) {
            out_error = "Missing core GLES2/3 symbols in libGLESv2.dll";
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
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        // Electron's sharedTexture expects NT HANDLE without keyed mutex for bgra/rgba/rgbaf16.
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

        HRESULT hr = d3d_device_->CreateTexture2D(&desc, nullptr, &slot.d3d_tex);
        if (FAILED(hr) || !slot.d3d_tex) {
            out_error = "ID3D11Device::CreateTexture2D failed";
            return false;
        }

        IDXGIResource1* dxgi_res = nullptr;
        hr = slot.d3d_tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&dxgi_res));
        if (FAILED(hr) || !dxgi_res) {
            out_error = "QueryInterface(IDXGIResource1) failed";
            destroySlot(slot);
            return false;
        }

        hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &slot.nt_handle);
        dxgi_res->Release();
        if (FAILED(hr) || !slot.nt_handle) {
            out_error = "IDXGIResource1::CreateSharedHandle failed";
            destroySlot(slot);
            return false;
        }

        glGenTextures_(1, &slot.texture);
        glBindTexture_(kGlTexture2d, slot.texture);
        glTexParameteri_(kGlTexture2d, kGlTextureMinFilter, kGlLinear);
        glTexParameteri_(kGlTexture2d, kGlTextureMagFilter, kGlLinear);
        glTexParameteri_(kGlTexture2d, kGlTextureWrapS, kGlClampToEdge);
        glTexParameteri_(kGlTexture2d, kGlTextureWrapT, kGlClampToEdge);

        bool bound = false;
        if (has_d3d11_image_ && eglCreateImageKHR_ && glEGLImageTargetTexture2DOES_) {
            static const EGLint img_attribs[] = { kEglNone };
            slot.egl_image = eglCreateImageKHR_(
                egl_display_, kEglNoContext, kEglD3d11TextureAngle,
                reinterpret_cast<EGLClientBuffer>(slot.d3d_tex), img_attribs
            );
            if (slot.egl_image != kEglNoImage) {
                glEGLImageTargetTexture2DOES_(kGlTexture2d, slot.egl_image);
                bound = true;
            }
        }

        if (!bound && has_d3d_pbuffer_ && eglCreatePbufferFromClientBuffer_ && eglBindTexImage_) {
            static const EGLint pb_attribs[] = { kEglNone };
            slot.pbuffer = eglCreatePbufferFromClientBuffer_(
                egl_display_, kEglD3dTextureAngle,
                reinterpret_cast<EGLClientBuffer>(slot.d3d_tex), egl_config_, pb_attribs
            );
            if (slot.pbuffer != kEglNoSurface) {
                bound = eglBindTexImage_(egl_display_, slot.pbuffer, kEglBackBuffer) != 0;
            }
        }
        glBindTexture_(kGlTexture2d, 0);

        if (!bound) {
            out_error = "Failed to bind D3D11 texture into ANGLE GL texture";
            destroySlot(slot);
            return false;
        }

        glGenFramebuffers_(1, &slot.fbo);
        glBindFramebuffer_(kGlFramebuffer, slot.fbo);
        glFramebufferTexture2D_(kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, slot.texture, 0);
        GLenum status = glCheckFramebufferStatus_(kGlFramebuffer);
        glBindFramebuffer_(kGlFramebuffer, 0);

        if (status != kGlFramebufferComplete) {
            out_error = "ANGLE D3D11 FBO is incomplete";
            destroySlot(slot);
            return false;
        }
        return true;
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
        if (s.egl_image != kEglNoImage && eglDestroyImageKHR_ && egl_display_ != kEglNoDisplay) {
            eglDestroyImageKHR_(egl_display_, s.egl_image);
            s.egl_image = kEglNoImage;
        }
        if (s.pbuffer != kEglNoSurface && eglDestroySurface_ && egl_display_ != kEglNoDisplay) {
            eglDestroySurface_(egl_display_, s.pbuffer);
            s.pbuffer = kEglNoSurface;
        }
        if (s.nt_handle) {
            CloseHandle(s.nt_handle);
            s.nt_handle = nullptr;
        }
        if (s.d3d_tex) {
            s.d3d_tex->Release();
            s.d3d_tex = nullptr;
        }
    }

    void destroySurfaces() {
        for (int i = 0; i < kSurfacePoolSize; ++i) {
            destroySlot(slots_[i]);
        }
        surfaces_ready_ = false;
    }

    void cleanupContext() {
        if (egl_display_ != kEglNoDisplay) {
            if (eglMakeCurrent_) eglMakeCurrent_(egl_display_, kEglNoSurface, kEglNoSurface, kEglNoContext);
            if (dummy_surface_ != kEglNoSurface && eglDestroySurface_) {
                eglDestroySurface_(egl_display_, dummy_surface_);
                dummy_surface_ = kEglNoSurface;
            }
            if (egl_context_ != kEglNoContext && eglDestroyContext_) {
                eglDestroyContext_(egl_display_, egl_context_);
                egl_context_ = kEglNoContext;
            }
            if (eglTerminate_) eglTerminate_(egl_display_);
            egl_display_ = kEglNoDisplay;
        }
        if (egl_device_ && eglReleaseDeviceANGLE_) {
            eglReleaseDeviceANGLE_(egl_device_);
            egl_device_ = nullptr;
        }
        if (d3d_context_) { d3d_context_->Release(); d3d_context_ = nullptr; }
        if (d3d_device_) { d3d_device_->Release(); d3d_device_ = nullptr; }
        if (gles_lib_) { FreeLibrary(gles_lib_); gles_lib_ = nullptr; }
        if (egl_lib_) { FreeLibrary(egl_lib_); egl_lib_ = nullptr; }
    }

    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    EGLDeviceEXT egl_device_ = nullptr;
    EGLDisplay egl_display_ = kEglNoDisplay;
    EGLConfig egl_config_ = nullptr;
    EGLContext egl_context_ = kEglNoContext;
    EGLSurface dummy_surface_ = kEglNoSurface;

    bool has_d3d11_image_ = false;
    bool has_d3d_pbuffer_ = false;
    bool hdr_supported_ = false;
    bool hdr_active_ = false;
    bool surfaces_ready_ = false;
    int width_ = 0;
    int height_ = 0;
    std::array<SurfaceSlot, kSurfacePoolSize> slots_;

    HMODULE egl_lib_ = nullptr;
    HMODULE gles_lib_ = nullptr;

    EGLBoolean (__stdcall* eglInitialize_)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (__stdcall* eglTerminate_)(EGLDisplay) = nullptr;
    EGLBoolean (__stdcall* eglBindAPI_)(EGLenum) = nullptr;
    EGLBoolean (__stdcall* eglChooseConfig_)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLContext (__stdcall* eglCreateContext_)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (__stdcall* eglDestroyContext_)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (__stdcall* eglMakeCurrent_)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    const char* (__stdcall* eglQueryString_)(EGLDisplay, EGLint) = nullptr;
    void* (__stdcall* eglGetProcAddress_)(const char*) = nullptr;
    EGLSurface (__stdcall* eglCreatePbufferSurface_)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
    EGLSurface (__stdcall* eglCreatePbufferFromClientBuffer_)(EGLDisplay, EGLenum, EGLClientBuffer, EGLConfig, const EGLint*) = nullptr;
    EGLBoolean (__stdcall* eglDestroySurface_)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (__stdcall* eglBindTexImage_)(EGLDisplay, EGLSurface, EGLint) = nullptr;
    EGLDeviceEXT (__stdcall* eglCreateDeviceANGLE_)(EGLint, void*, const EGLAttrib*) = nullptr;
    EGLBoolean (__stdcall* eglReleaseDeviceANGLE_)(EGLDeviceEXT) = nullptr;
    EGLDisplay (__stdcall* eglGetPlatformDisplayEXT_)(EGLenum, void*, const EGLint*) = nullptr;
    EGLImageKHR (__stdcall* eglCreateImageKHR_)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*) = nullptr;
    EGLBoolean (__stdcall* eglDestroyImageKHR_)(EGLDisplay, EGLImageKHR) = nullptr;

    void (__stdcall* glGenTextures_)(GLsizei, GLuint*) = nullptr;
    void (__stdcall* glDeleteTextures_)(GLsizei, const GLuint*) = nullptr;
    void (__stdcall* glBindTexture_)(GLenum, GLuint) = nullptr;
    void (__stdcall* glTexParameteri_)(GLenum, GLenum, GLint) = nullptr;
    void (__stdcall* glGenFramebuffers_)(GLsizei, GLuint*) = nullptr;
    void (__stdcall* glDeleteFramebuffers_)(GLsizei, const GLuint*) = nullptr;
    void (__stdcall* glBindFramebuffer_)(GLenum, GLuint) = nullptr;
    void (__stdcall* glFramebufferTexture2D_)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    GLenum (__stdcall* glCheckFramebufferStatus_)(GLenum) = nullptr;
    void (__stdcall* glFinish_)(void) = nullptr;
    void (__stdcall* glEGLImageTargetTexture2DOES_)(GLenum, EGLImageKHR) = nullptr;
};

}  // namespace electron_mpv

#endif  // _WIN32
