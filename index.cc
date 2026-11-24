#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <napi.h>
#include "src/linux_helper_client.h"

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
    return electron_mpv::LinuxHelperMpvPlayer::Init(env, exports);
}

NODE_API_MODULE(electron_libmpv, InitModule)
#else
#include <napi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/mpv_loader.h"
#include "src/gpu_backend.h"
#include "src/gpu_backend_sw.h"
#ifdef _WIN32
#include "src/gpu_backend_win.h"
#elif defined(__APPLE__)
#include "src/gpu_backend_mac.h"
#elif defined(__linux__)
#include "src/gpu_backend_linux.h"
#endif

namespace electron_mpv {

namespace {

Napi::Value MpvNodeToJs(Napi::Env env, const mpv_node& node) {
    switch (node.format) {
        case MPV_FORMAT_STRING:
            return node.u.string ? Napi::String::New(env, node.u.string) : env.Null();
        case MPV_FORMAT_FLAG:
            return Napi::Boolean::New(env, node.u.flag != 0);
        case MPV_FORMAT_INT64:
            return Napi::Number::New(env, static_cast<double>(node.u.int64));
        case MPV_FORMAT_DOUBLE:
            return Napi::Number::New(env, node.u.double_);
        case MPV_FORMAT_NODE_ARRAY: {
            if (!node.u.list) return Napi::Array::New(env, 0);
            Napi::Array arr = Napi::Array::New(env, static_cast<size_t>(node.u.list->num));
            for (int i = 0; i < node.u.list->num; ++i) {
                arr.Set(static_cast<uint32_t>(i), MpvNodeToJs(env, node.u.list->values[i]));
            }
            return arr;
        }
        case MPV_FORMAT_NODE_MAP: {
            Napi::Object obj = Napi::Object::New(env);
            if (!node.u.list) return obj;
            for (int i = 0; i < node.u.list->num; ++i) {
                if (node.u.list->keys && node.u.list->keys[i]) {
                    obj.Set(node.u.list->keys[i], MpvNodeToJs(env, node.u.list->values[i]));
                }
            }
            return obj;
        }
        case MPV_FORMAT_NONE:
        default:
            return env.Null();
    }
}

const char* EndFileReasonToString(int reason) {
    switch (reason) {
        case MPV_END_FILE_REASON_EOF: return "eof";
        case MPV_END_FILE_REASON_STOP: return "stop";
        case MPV_END_FILE_REASON_QUIT: return "quit";
        case MPV_END_FILE_REASON_ERROR: return "error";
        case MPV_END_FILE_REASON_REDIRECT: return "redirect";
        default: return "other";
    }
}

mpv_format ParseObserveFormat(const std::string& fmt) {
    if (fmt == "double" || fmt == "number") return MPV_FORMAT_DOUBLE;
    if (fmt == "flag" || fmt == "bool" || fmt == "boolean") return MPV_FORMAT_FLAG;
    if (fmt == "int64") return MPV_FORMAT_INT64;
    if (fmt == "node" || fmt == "json") return MPV_FORMAT_NODE;
    return MPV_FORMAT_STRING;
}

std::vector<std::unique_ptr<RenderBackend>> CreateGpuBackends() {
    std::vector<std::unique_ptr<RenderBackend>> candidates;
#ifdef _WIN32
    // Electron <= 43 bundles libEGL.dll / libGLESv2.dll; Electron 44+ statically links ANGLE,
    // so fall back seamlessly to WGL + WGL_NV_DX_interop2 -> D3D11 shared NT handle.
    candidates.push_back(std::make_unique<WinAngleD3D11Backend>());
    candidates.push_back(std::make_unique<WinWglD3D11Backend>());
#elif defined(__APPLE__)
    candidates.push_back(std::make_unique<MacIOSurfaceBackend>());
#elif defined(__linux__)
    candidates.push_back(std::make_unique<LinuxDmaBufBackend>());
#endif
    return candidates;
}

struct DispatchedFrame {
    FrameDescriptor frame;
    uint64_t generation = 0;

    ~DispatchedFrame() {
#ifdef _WIN32
        if (frame.owns_nt_handle && frame.nt_handle != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(frame.nt_handle));
            frame.nt_handle = 0;
            frame.owns_nt_handle = false;
        }
#elif defined(__APPLE__)
        if (frame.owns_io_surface && frame.io_surface != 0) {
            CFRelease(reinterpret_cast<IOSurfaceRef>(frame.io_surface));
            frame.io_surface = 0;
            frame.owns_io_surface = false;
        }
#endif
    }
};

}  // namespace

class MpvPlayer : public Napi::ObjectWrap<MpvPlayer> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    explicit MpvPlayer(const Napi::CallbackInfo& info);
    ~MpvPlayer() override;

private:
    Napi::Value Initialize(const Napi::CallbackInfo& info);
    Napi::Value Resize(const Napi::CallbackInfo& info);
    Napi::Value ReleaseFrame(const Napi::CallbackInfo& info);
    Napi::Value EnableSoftwareFallback(const Napi::CallbackInfo& info);
    Napi::Value Command(const Napi::CallbackInfo& info);
    Napi::Value SetProperty(const Napi::CallbackInfo& info);
    Napi::Value GetProperty(const Napi::CallbackInfo& info);
    Napi::Value ObserveProperty(const Napi::CallbackInfo& info);
    Napi::Value UnobserveAll(const Napi::CallbackInfo& info);
    Napi::Value GetBackendInfo(const Napi::CallbackInfo& info);
    Napi::Value Destroy(const Napi::CallbackInfo& info);

    void ShutdownInternal();
    void DrainMpvEvents(Napi::Env env, Napi::Function js_callback);
    void RenderThreadMain(bool prefer_gpu);

    static void OnMpvWakeup(void* ctx);
    static void OnRenderUpdate(void* ctx);

    MpvApi api_;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;
    std::unique_ptr<RenderBackend> backend_;
    std::mutex backend_mutex_;

    Napi::ThreadSafeFunction event_tsfn_;
    Napi::ThreadSafeFunction frame_tsfn_;
    bool has_event_tsfn_ = false;
    bool has_frame_tsfn_ = false;

    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> destroyed_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> event_wakeup_pending_{false};

    // Render thread synchronization
    std::thread render_thread_;
    std::mutex render_mutex_;
    std::condition_variable render_cv_;
    std::condition_variable init_cv_;
    bool render_stop_ = false;
    bool render_frame_pending_ = false;
    bool render_resize_pending_ = false;
    bool render_fallback_requested_ = false;
    bool render_init_done_ = false;
    bool render_init_ok_ = false;
    std::string render_init_error_;
    std::string gpu_init_error_;
    std::string backend_name_ = "uninitialized";
    bool backend_shared_texture_ = false;
    bool backend_hdr_supported_ = false;

    int target_width_ = 1280;
    int target_height_ = 720;
    bool target_hdr_ = false;
    std::atomic<uint64_t> surface_generation_{1};

    std::string custom_libmpv_path_;
    std::string mpv_config_path_;
};

Napi::Object MpvPlayer::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "MpvPlayer", {
        InstanceMethod("initialize", &MpvPlayer::Initialize),
        InstanceMethod("resize", &MpvPlayer::Resize),
        InstanceMethod("releaseFrame", &MpvPlayer::ReleaseFrame),
        InstanceMethod("enableSoftwareFallback", &MpvPlayer::EnableSoftwareFallback),
        InstanceMethod("command", &MpvPlayer::Command),
        InstanceMethod("setProperty", &MpvPlayer::SetProperty),
        InstanceMethod("getProperty", &MpvPlayer::GetProperty),
        InstanceMethod("observeProperty", &MpvPlayer::ObserveProperty),
        InstanceMethod("unobserveAll", &MpvPlayer::UnobserveAll),
        InstanceMethod("getBackendInfo", &MpvPlayer::GetBackendInfo),
        InstanceMethod("destroy", &MpvPlayer::Destroy),
    });

    auto* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);

    exports.Set("MpvPlayer", func);
    return exports;
}

MpvPlayer::MpvPlayer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MpvPlayer>(info) {
    Napi::Env env = info.Env();
    if (info.Length() > 0 && info[0].IsObject()) {
        Napi::Object opts = info[0].As<Napi::Object>();

        if (opts.Has("libmpvPath") && opts.Get("libmpvPath").IsString()) {
            custom_libmpv_path_ = opts.Get("libmpvPath").As<Napi::String>().Utf8Value();
        }
        if (opts.Has("configPath") && opts.Get("configPath").IsString()) {
            mpv_config_path_ = opts.Get("configPath").As<Napi::String>().Utf8Value();
        }
        if (opts.Has("width") && opts.Get("width").IsNumber()) {
            target_width_ = std::max(16, opts.Get("width").As<Napi::Number>().Int32Value());
        }
        if (opts.Has("height") && opts.Get("height").IsNumber()) {
            target_height_ = std::max(16, opts.Get("height").As<Napi::Number>().Int32Value());
        }
        if (opts.Has("onEvent") && opts.Get("onEvent").IsFunction()) {
            event_tsfn_ = Napi::ThreadSafeFunction::New(
                env, opts.Get("onEvent").As<Napi::Function>(), "MpvEvents", 0, 1
            );
            has_event_tsfn_ = true;
        }
        if (opts.Has("onFrame") && opts.Get("onFrame").IsFunction()) {
            frame_tsfn_ = Napi::ThreadSafeFunction::New(
                env, opts.Get("onFrame").As<Napi::Function>(), "MpvFrames", 0, 1
            );
            has_frame_tsfn_ = true;
        }
    }

    // Load libmpv dynamically so compilation has zero hardcoded library paths.
    api_.load(custom_libmpv_path_);
}

MpvPlayer::~MpvPlayer() {
    ShutdownInternal();
}

void MpvPlayer::ShutdownInternal() {
    bool expected = false;
    if (!destroyed_.compare_exchange_strong(expected, true)) {
        return;
    }
    alive_->store(false, std::memory_order_release);

    if (render_thread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(render_mutex_);
            render_stop_ = true;
        }
        render_cv_.notify_all();
        render_thread_.join();
    }

    if (mpv_ && api_.loaded) {
        api_.set_wakeup_callback(mpv_, nullptr, nullptr);
        api_.terminate_destroy(mpv_);
        mpv_ = nullptr;
    }

    if (has_event_tsfn_) {
        event_tsfn_.Release();
        has_event_tsfn_ = false;
    }
    if (has_frame_tsfn_) {
        frame_tsfn_.Release();
        has_frame_tsfn_ = false;
    }
}

void MpvPlayer::OnMpvWakeup(void* ctx) {
    auto* self = static_cast<MpvPlayer*>(ctx);
    if (!self || self->destroyed_.load(std::memory_order_acquire) || !self->has_event_tsfn_) return;

    bool expected = false;
    if (!self->event_wakeup_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    auto alive = self->alive_;
    self->event_tsfn_.NonBlockingCall([self, alive](Napi::Env env, Napi::Function js_callback) {
        if (!alive->load(std::memory_order_acquire)) return;
        self->event_wakeup_pending_.store(false, std::memory_order_release);
        if (!self->destroyed_.load(std::memory_order_acquire)) {
            self->DrainMpvEvents(env, js_callback);
        }
    });
}

void MpvPlayer::DrainMpvEvents(Napi::Env env, Napi::Function js_callback) {
    if (!mpv_ || !api_.loaded) return;

    while (mpv_event* ev = api_.wait_event(mpv_, 0.0)) {
        if (ev->event_id == MPV_EVENT_NONE) break;

        Napi::Object payload = Napi::Object::New(env);
        bool emit = true;

        switch (ev->event_id) {
            case MPV_EVENT_START_FILE:
                payload.Set("type", "start-file");
                break;

            case MPV_EVENT_FILE_LOADED:
                payload.Set("type", "file-loaded");
                break;

            case MPV_EVENT_PLAYBACK_RESTART:
                payload.Set("type", "playback-restart");
                break;

            case MPV_EVENT_END_FILE: {
                auto* ef = static_cast<mpv_event_end_file*>(ev->data);
                payload.Set("type", "end-file");
                payload.Set("reason", ef ? EndFileReasonToString(ef->reason) : "other");
                if (ef && ef->error < 0) {
                    payload.Set("error", api_.error_string(ef->error));
                }
                break;
            }

            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* prop = static_cast<mpv_event_property*>(ev->data);
                if (!prop || !prop->name) {
                    emit = false;
                    break;
                }
                payload.Set("type", "property-change");
                payload.Set("name", prop->name);
                if (!prop->data) {
                    payload.Set("value", env.Null());
                } else {
                    switch (prop->format) {
                        case MPV_FORMAT_STRING: {
                            const char* str = *static_cast<const char**>(prop->data);
                            payload.Set("value", str ? Napi::String::New(env, str) : env.Null());
                            break;
                        }
                        case MPV_FORMAT_FLAG: {
                            int flag = *static_cast<int*>(prop->data);
                            payload.Set("value", Napi::Boolean::New(env, flag != 0));
                            break;
                        }
                        case MPV_FORMAT_INT64: {
                            int64_t val = *static_cast<int64_t*>(prop->data);
                            payload.Set("value", Napi::Number::New(env, static_cast<double>(val)));
                            break;
                        }
                        case MPV_FORMAT_DOUBLE: {
                            double val = *static_cast<double*>(prop->data);
                            payload.Set("value", Napi::Number::New(env, val));
                            break;
                        }
                        case MPV_FORMAT_NODE: {
                            auto* node = static_cast<mpv_node*>(prop->data);
                            payload.Set("value", MpvNodeToJs(env, *node));
                            break;
                        }
                        default:
                            payload.Set("value", env.Null());
                            break;
                    }
                }
                break;
            }

            case MPV_EVENT_LOG_MESSAGE: {
                auto* lm = static_cast<mpv_event_log_message*>(ev->data);
                if (!lm || !lm->text) {
                    emit = false;
                    break;
                }
                payload.Set("type", "log-message");
                payload.Set("prefix", lm->prefix ? lm->prefix : "");
                payload.Set("level", lm->level ? lm->level : "");
                payload.Set("text", lm->text);
                break;
            }

            default:
                emit = false;
                break;
        }

        if (emit && js_callback && !js_callback.IsEmpty()) {
            js_callback.Call({ payload });
        }
    }
}

void MpvPlayer::OnRenderUpdate(void* ctx) {
    auto* self = static_cast<MpvPlayer*>(ctx);
    if (!self || self->destroyed_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(self->render_mutex_);
        self->render_frame_pending_ = true;
    }
    self->render_cv_.notify_one();
}

void MpvPlayer::RenderThreadMain(bool prefer_gpu) {
    std::string init_error;
    bool init_ok = false;

    std::string gpu_error;
    if (prefer_gpu) {
        auto candidates = CreateGpuBackends();
        for (auto& candidate : candidates) {
            std::string candidate_err;
            if (candidate->init(api_, mpv_, &render_ctx_, candidate_err)) {
                backend_ = std::move(candidate);
                gpu_error.clear();
                init_ok = true;
                break;
            }
            if (!candidate_err.empty()) {
                if (!gpu_error.empty()) gpu_error += " | ";
                gpu_error += std::string(candidate->name()) + ": " + candidate_err;
            }
        }
        if (!init_ok) {
            init_error = gpu_error;
        }
    }

    if (!init_ok) {
        std::string sw_error;
        backend_ = std::make_unique<SoftwareRenderBackend>();
        if (backend_->init(api_, mpv_, &render_ctx_, sw_error)) {
            init_ok = true;
        } else {
            init_error = init_error.empty() ? sw_error : (init_error + " | SW fallback: " + sw_error);
            backend_.reset();
        }
    }

    if (init_ok && backend_ && render_ctx_) {
        std::string surf_err;
        bool recreated = false;
        if (!backend_->ensureSurface(target_width_, target_height_, target_hdr_, recreated, surf_err)) {
            if (backend_->supportsSharedTexture()) {
                gpu_error = gpu_error.empty() ? surf_err : (gpu_error + " | " + surf_err);
                backend_->shutdown(api_, render_ctx_);
                render_ctx_ = nullptr;
                {
                    std::lock_guard<std::mutex> blk(backend_mutex_);
                    backend_ = std::make_unique<SoftwareRenderBackend>();
                }
                std::string sw_err;
                bool sw_recreated = false;
                if (backend_->init(api_, mpv_, &render_ctx_, sw_err)) {
                    backend_->ensureSurface(target_width_, target_height_, false, sw_recreated, sw_err);
                } else {
                    init_ok = false;
                    init_error = surf_err + " | SW fallback: " + sw_err;
                    std::lock_guard<std::mutex> blk(backend_mutex_);
                    backend_.reset();
                }
            } else {
                init_ok = false;
                init_error = surf_err;
            }
        }
    }

    if (init_ok && render_ctx_ && backend_) {
        api_.render_context_set_update_callback(render_ctx_, &MpvPlayer::OnRenderUpdate, this);
    }

    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        render_init_done_ = true;
        render_init_ok_ = init_ok;
        render_init_error_ = init_error;
        gpu_init_error_ = gpu_error;
        if (init_ok && backend_) {
            backend_name_ = backend_->name();
            backend_shared_texture_ = backend_->supportsSharedTexture();
            backend_hdr_supported_ = backend_->supportsHdr();
        }
    }
    init_cv_.notify_all();

    if (!init_ok || !render_ctx_ || !backend_) {
        return;
    }

    while (true) {
        bool do_frame = false;
        bool do_resize = false;
        bool do_fallback = false;
        bool force_redraw = false;
        int req_w = 0;
        int req_h = 0;
        bool req_hdr = false;

        {
            std::unique_lock<std::mutex> lock(render_mutex_);
            render_cv_.wait(lock, [this]() {
                return render_stop_ || render_frame_pending_ || render_resize_pending_ || render_fallback_requested_;
            });

            if (render_stop_) break;

            req_w = target_width_;
            req_h = target_height_;
            req_hdr = target_hdr_;

            if (render_fallback_requested_) {
                do_fallback = true;
                render_fallback_requested_ = false;
            }
            if (render_resize_pending_) {
                do_resize = true;
                render_resize_pending_ = false;
            }
            if (render_frame_pending_) {
                do_frame = true;
                render_frame_pending_ = false;
            }
        }

        if (do_fallback && backend_->supportsSharedTexture()) {
            api_.render_context_set_update_callback(render_ctx_, nullptr, nullptr);
            surface_generation_.fetch_add(1, std::memory_order_acq_rel);
            std::unique_ptr<RenderBackend> old_backend;
            {
                std::lock_guard<std::mutex> blk(backend_mutex_);
                old_backend = std::move(backend_);
            }
            old_backend->shutdown(api_, render_ctx_);
            old_backend.reset();
            render_ctx_ = nullptr;

            auto sw_backend = std::make_unique<SoftwareRenderBackend>();
            std::string sw_err;
            if (sw_backend->init(api_, mpv_, &render_ctx_, sw_err)) {
                bool sw_recreated = false;
                sw_backend->ensureSurface(req_w, req_h, false, sw_recreated, sw_err);
                {
                    std::lock_guard<std::mutex> blk(backend_mutex_);
                    backend_ = std::move(sw_backend);
                }
                api_.render_context_set_update_callback(render_ctx_, &MpvPlayer::OnRenderUpdate, this);
                {
                    std::lock_guard<std::mutex> lock(render_mutex_);
                    backend_name_ = backend_->name();
                    backend_shared_texture_ = false;
                    backend_hdr_supported_ = false;
                }
                do_resize = false;
                force_redraw = true;
            } else {
                break;
            }
        }

        if (do_resize && backend_) {
            std::string resize_err;
            bool recreated = false;
            if (backend_->ensureSurface(req_w, req_h, req_hdr, recreated, resize_err)) {
                if (recreated) {
                    surface_generation_.fetch_add(1, std::memory_order_acq_rel);
                    force_redraw = true;
                }
            }
        }

        uint64_t update_flags = api_.render_context_update(render_ctx_);
        bool should_render = ((update_flags & MPV_RENDER_UPDATE_FRAME) != 0) || force_redraw;
        if (!should_render) continue;

        int slot = backend_->acquireFreeSlot();
        if (slot < 0) {
            // All triple-buffer slots are held by Electron's GPU process; skip draw so audio stays locked.
            int skip = 1;
            mpv_render_param skip_params[] = {
                { MPV_RENDER_PARAM_SKIP_RENDERING, &skip },
                { MPV_RENDER_PARAM_INVALID, nullptr },
            };
            api_.render_context_render(render_ctx_, skip_params);
            api_.render_context_report_swap(render_ctx_);
            continue;
        }

        auto* dispatched = new DispatchedFrame();
        dispatched->generation = surface_generation_.load(std::memory_order_acquire);

        std::string render_err;
        if (!backend_->renderFrame(api_, render_ctx_, slot, dispatched->frame, render_err)) {
            backend_->releaseSlot(slot);
            delete dispatched;
            continue;
        }

        if (!has_frame_tsfn_ || destroyed_.load(std::memory_order_acquire)) {
            backend_->releaseSlot(slot);
            delete dispatched;
            continue;
        }

        auto alive = alive_;
        napi_status status = frame_tsfn_.NonBlockingCall(
            dispatched,
            [this, alive](Napi::Env env, Napi::Function js_callback, DispatchedFrame* data) {
                std::unique_ptr<DispatchedFrame> guard(data);
                if (!alive->load(std::memory_order_acquire) || !js_callback || js_callback.IsEmpty()) {
                    return;
                }
                if (guard->generation != surface_generation_.load(std::memory_order_acquire)) {
                    // Surface pool was resized or switched while this frame was queued; drop cleanly.
                    // ~DispatchedFrame() closes our duplicated NT handle / releases our retained IOSurface.
                    return;
                }

                const FrameDescriptor& f = guard->frame;
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("slotId", Napi::Number::New(env, f.slot_id));
                obj.Set("generation", Napi::Number::New(env, static_cast<double>(guard->generation)));
                obj.Set("width", Napi::Number::New(env, f.width));
                obj.Set("height", Napi::Number::New(env, f.height));
                obj.Set("pixelFormat", Napi::String::New(env, f.pixel_format));
                obj.Set("hdr", Napi::Boolean::New(env, f.hdr));
                obj.Set("timestamp", Napi::Number::New(env, static_cast<double>(f.timestamp_us)));

                if (f.type == FrameHandleType::WinNtHandle) {
                    obj.Set("kind", "sharedTexture");
                    Napi::Object handle = Napi::Object::New(env);
                    uintptr_t raw = f.nt_handle;
                    handle.Set("ntHandle", Napi::Buffer<uint8_t>::Copy(env, reinterpret_cast<const uint8_t*>(&raw), sizeof(uintptr_t)));
                    obj.Set("handle", handle);
                } else if (f.type == FrameHandleType::MacIOSurface) {
                    obj.Set("kind", "sharedTexture");
                    Napi::Object handle = Napi::Object::New(env);
                    uintptr_t raw = f.io_surface;
                    handle.Set("ioSurface", Napi::Buffer<uint8_t>::Copy(env, reinterpret_cast<const uint8_t*>(&raw), sizeof(uintptr_t)));
                    obj.Set("handle", handle);
                } else if (f.type == FrameHandleType::LinuxNativePixmap) {
                    obj.Set("kind", "sharedTexture");
                    Napi::Object plane = Napi::Object::New(env);
                    plane.Set("stride", Napi::Number::New(env, f.stride));
                    plane.Set("offset", Napi::Number::New(env, static_cast<double>(f.offset)));
                    plane.Set("size", Napi::Number::New(env, static_cast<double>(f.size)));
                    plane.Set("fd", Napi::Number::New(env, f.dmabuf_fd));

                    Napi::Array planes = Napi::Array::New(env, 1);
                    planes.Set(static_cast<uint32_t>(0), plane);

                    Napi::Object pixmap = Napi::Object::New(env);
                    pixmap.Set("planes", planes);
                    // Electron parses modifier with base::StringToUint64 (base-10 decimal).
                    pixmap.Set("modifier", Napi::String::New(env, std::to_string(f.modifier)));
                    pixmap.Set("supportsZeroCopyWebGpuImport", Napi::Boolean::New(env, f.supports_zero_copy_webgpu));

                    Napi::Object handle = Napi::Object::New(env);
                    handle.Set("nativePixmap", pixmap);
                    obj.Set("handle", handle);
                } else if (f.type == FrameHandleType::SoftwareRgba) {
                    obj.Set("kind", "software");
                    obj.Set("stride", Napi::Number::New(env, static_cast<double>(f.sw_stride)));
                    obj.Set("buffer", Napi::Buffer<uint8_t>::Copy(env, f.sw_pixels.data(), f.sw_pixels.size()));
                }

                js_callback.Call({ obj });
            }
        );

        if (status != napi_ok) {
            backend_->releaseSlot(slot);
            delete dispatched;
        }
    }

    if (render_ctx_) {
        api_.render_context_set_update_callback(render_ctx_, nullptr, nullptr);
    }
    std::unique_ptr<RenderBackend> final_backend;
    {
        std::lock_guard<std::mutex> blk(backend_mutex_);
        final_backend = std::move(backend_);
    }
    if (final_backend) {
        final_backend->shutdown(api_, render_ctx_);
        render_ctx_ = nullptr;
        final_backend.reset();
    }
}

Napi::Value MpvPlayer::Initialize(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object result = Napi::Object::New(env);

    if (initialized_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(render_mutex_);
        result.Set("ok", true);
        result.Set("backend", backend_name_);
        return result;
    }

    if (!api_.loaded) {
        result.Set("ok", false);
        result.Set("error", api_.last_error);
        return result;
    }

    bool prefer_gpu = true;
    if (info.Length() > 0 && info[0].IsObject()) {
        Napi::Object opts = info[0].As<Napi::Object>();
        if (opts.Has("width") && opts.Get("width").IsNumber()) {
            target_width_ = std::max(16, opts.Get("width").As<Napi::Number>().Int32Value());
        }
        if (opts.Has("height") && opts.Get("height").IsNumber()) {
            target_height_ = std::max(16, opts.Get("height").As<Napi::Number>().Int32Value());
        }
        if (opts.Has("hdr") && opts.Get("hdr").IsBoolean()) {
            target_hdr_ = opts.Get("hdr").As<Napi::Boolean>().Value();
        }
        if (opts.Has("preferGpu") && opts.Get("preferGpu").IsBoolean()) {
            prefer_gpu = opts.Get("preferGpu").As<Napi::Boolean>().Value();
        }
    }

    std::setlocale(LC_NUMERIC, "C");
    mpv_ = api_.create();
    if (!mpv_) {
        result.Set("ok", false);
        result.Set("error", "mpv_create() returned null");
        return result;
    }

    // Core embedded rendering & streaming resilience defaults (set BEFORE user mpv.conf so users can override).
    api_.set_option_string(mpv_, "title", "Stremio Enhanced");
    api_.set_option_string(mpv_, "audio-client-name", "Stremio Enhanced");
    api_.set_option_string(mpv_, "video-timing-offset", "0");
    api_.set_option_string(mpv_, "video-sync", "audio");
    api_.set_option_string(mpv_, "hwdec", "auto-safe");
#ifdef _WIN32
    api_.set_option_string(mpv_, "d3d11va-zero-copy", "yes");
#endif
    api_.set_option_string(
        mpv_,
        "stream-lavf-o",
        "reconnect=1,reconnect_streamed=1,reconnect_on_network_error=1,reconnect_on_http_error=%23%408,429,500,502,503,504,reconnect_delay_max=15"
    );
    api_.set_option_string(mpv_, "osc", "no");
    api_.set_option_string(mpv_, "input-default-bindings", "no");
    api_.set_option_string(mpv_, "input-vo-keyboard", "no");
    api_.set_option_string(mpv_, "keep-open", "no");
    api_.set_option_string(mpv_, "idle", "yes");
    api_.set_option_string(mpv_, "tone-mapping", "bt.2390");
    api_.set_option_string(mpv_, "hdr-compute-peak", "yes");
    api_.set_option_string(mpv_, "dither-depth", "auto");
    api_.set_option_string(mpv_, "deband", "yes");
    api_.set_option_string(mpv_, "scale", "spline36");
    api_.set_option_string(mpv_, "cscale", "spline36");
    api_.set_option_string(mpv_, "demuxer-max-bytes", "150MiB");
    api_.set_option_string(mpv_, "demuxer-max-back-bytes", "75MiB");

    if (!mpv_config_path_.empty()) {
        api_.set_option_string(mpv_, "include", mpv_config_path_.c_str());
    }

    // Always force vo=libmpv after user config so embedded rendering cannot be broken by mpv.conf.
    api_.set_option_string(mpv_, "vo", "libmpv");

    int err = api_.initialize(mpv_);
    if (err < 0) {
        std::string msg = std::string("mpv_initialize() failed: ") + api_.error_string(err);
        api_.destroy(mpv_);
        mpv_ = nullptr;
        result.Set("ok", false);
        result.Set("error", msg);
        return result;
    }

    api_.request_log_messages(mpv_, "info");
    api_.set_wakeup_callback(mpv_, &MpvPlayer::OnMpvWakeup, this);

    render_init_done_ = false;
    render_init_ok_ = false;
    render_stop_ = false;
    render_thread_ = std::thread(&MpvPlayer::RenderThreadMain, this, prefer_gpu);

    {
        std::unique_lock<std::mutex> lock(render_mutex_);
        init_cv_.wait(lock, [this]() { return render_init_done_; });
    }

    if (!render_init_ok_) {
        if (render_thread_.joinable()) render_thread_.join();
        api_.set_wakeup_callback(mpv_, nullptr, nullptr);
        api_.terminate_destroy(mpv_);
        mpv_ = nullptr;
        result.Set("ok", false);
        result.Set("error", render_init_error_);
        return result;
    }

    initialized_.store(true, std::memory_order_release);
    result.Set("ok", true);
    result.Set("backend", backend_name_);
    result.Set("sharedTexture", backend_shared_texture_);
    result.Set("hdrSupported", backend_hdr_supported_);
    result.Set("libmpvPath", api_.loaded_path);
    if (!gpu_init_error_.empty()) {
        result.Set("gpuError", gpu_init_error_);
    }
    return result;
}

Napi::Value MpvPlayer::Resize(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!initialized_.load(std::memory_order_acquire) || info.Length() < 2) {
        return env.Undefined();
    }

    int w = std::max(16, info[0].As<Napi::Number>().Int32Value());
    int h = std::max(16, info[1].As<Napi::Number>().Int32Value());
    bool has_hdr = (info.Length() >= 3 && info[2].IsBoolean());
    bool req_hdr = has_hdr ? info[2].As<Napi::Boolean>().Value() : false;

    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        bool hdr = has_hdr ? req_hdr : target_hdr_;
        if (w == target_width_ && h == target_height_ && hdr == target_hdr_) {
            return env.Undefined();
        }
        target_width_ = w;
        target_height_ = h;
        target_hdr_ = hdr;
        render_resize_pending_ = true;
    }
    render_cv_.notify_one();
    return env.Undefined();
}

Napi::Value MpvPlayer::ReleaseFrame(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1) return env.Undefined();

    int slot = info[0].As<Napi::Number>().Int32Value();
    uint64_t gen = (info.Length() >= 2 && info[1].IsNumber())
        ? static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value())
        : surface_generation_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> blk(backend_mutex_);
    if (backend_ && gen == surface_generation_.load(std::memory_order_acquire)) {
        backend_->releaseSlot(slot);
    }
    return env.Undefined();
}

Napi::Value MpvPlayer::EnableSoftwareFallback(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!initialized_.load(std::memory_order_acquire)) return Napi::Boolean::New(env, false);

    {
        std::lock_guard<std::mutex> lock(render_mutex_);
        render_fallback_requested_ = true;
    }
    render_cv_.notify_one();
    return Napi::Boolean::New(env, true);
}

Napi::Value MpvPlayer::Command(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!mpv_ || !api_.loaded || info.Length() == 0) return Napi::Number::New(env, -1);

    std::vector<std::string> args_str;
    args_str.reserve(info.Length());
    for (size_t i = 0; i < info.Length(); ++i) {
        args_str.push_back(info[i].ToString().Utf8Value());
    }

    std::vector<const char*> args_ptr;
    args_ptr.reserve(args_str.size() + 1);
    for (const auto& s : args_str) args_ptr.push_back(s.c_str());
    args_ptr.push_back(nullptr);

    int res = api_.command_async(mpv_, 0, args_ptr.data());
    return Napi::Number::New(env, res);
}

Napi::Value MpvPlayer::SetProperty(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!mpv_ || !api_.loaded || info.Length() < 2) return Napi::Number::New(env, -1);

    std::string name = info[0].ToString().Utf8Value();
    Napi::Value val = info[1];
    int res = 0;

    if (val.IsBoolean()) {
        int flag = val.As<Napi::Boolean>().Value() ? 1 : 0;
        res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_FLAG, &flag);
        if (res < 0) {
            res = api_.set_property_string(mpv_, name.c_str(), flag ? "yes" : "no");
        }
    } else if (val.IsNumber()) {
        double num = val.As<Napi::Number>().DoubleValue();
        if (std::isfinite(num) && std::trunc(num) == num && std::abs(num) <= 9007199254740991.0) {
            int64_t i64 = static_cast<int64_t>(num);
            res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_INT64, &i64);
            if (res < 0) {
                res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_DOUBLE, &num);
            }
        } else {
            res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_DOUBLE, &num);
        }
        if (res < 0) {
            std::string str = val.ToString().Utf8Value();
            res = api_.set_property_string(mpv_, name.c_str(), str.c_str());
        }
    } else {
        std::string str = val.ToString().Utf8Value();
        res = api_.set_property_string(mpv_, name.c_str(), str.c_str());
    }

    return Napi::Number::New(env, res);
}

Napi::Value MpvPlayer::GetProperty(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!mpv_ || !api_.loaded || info.Length() < 1) return env.Null();

    std::string name = info[0].ToString().Utf8Value();
    std::string fmt = (info.Length() >= 2 && info[1].IsString())
        ? info[1].As<Napi::String>().Utf8Value()
        : "string";

    if (fmt == "node" || fmt == "json") {
        mpv_node node = {};
        if (api_.get_property(mpv_, name.c_str(), MPV_FORMAT_NODE, &node) < 0) {
            return env.Null();
        }
        Napi::Value js_val = MpvNodeToJs(env, node);
        api_.free_node_contents(&node);
        return js_val;
    }

    char* result = api_.get_property_string(mpv_, name.c_str());
    if (!result) return env.Null();
    Napi::String js_result = Napi::String::New(env, result);
    api_.free(result);
    return js_result;
}

Napi::Value MpvPlayer::ObserveProperty(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!mpv_ || !api_.loaded || info.Length() < 1) return Napi::Number::New(env, -1);

    std::string name = info[0].ToString().Utf8Value();
    std::string fmt = (info.Length() >= 2 && info[1].IsString())
        ? info[1].As<Napi::String>().Utf8Value()
        : "string";

    int res = api_.observe_property(mpv_, 1, name.c_str(), ParseObserveFormat(fmt));
    return Napi::Number::New(env, res);
}

Napi::Value MpvPlayer::UnobserveAll(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!mpv_ || !api_.loaded) return Napi::Number::New(env, -1);
    return Napi::Number::New(env, api_.unobserve_property(mpv_, 1));
}

Napi::Value MpvPlayer::GetBackendInfo(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object obj = Napi::Object::New(env);
    std::lock_guard<std::mutex> lock(render_mutex_);
    obj.Set("libmpvLoaded", api_.loaded);
    obj.Set("libmpvPath", api_.loaded_path);
    obj.Set("libmpvError", api_.last_error);
    obj.Set("initialized", initialized_.load(std::memory_order_acquire));
    obj.Set("backend", backend_name_);
    obj.Set("sharedTexture", backend_shared_texture_);
    obj.Set("hdrSupported", backend_hdr_supported_);
    return obj;
}

Napi::Value MpvPlayer::Destroy(const Napi::CallbackInfo& info) {
    ShutdownInternal();
    return info.Env().Undefined();
}

}  // namespace electron_mpv

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
    return electron_mpv::MpvPlayer::Init(env, exports);
}

NODE_API_MODULE(electron_libmpv, InitModule)
#endif
