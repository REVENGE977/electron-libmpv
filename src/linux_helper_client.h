#pragma once

#ifdef __linux__

#include <napi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "gpu_backend.h"
#include "linux_ipc_proto.h"

namespace electron_mpv {

namespace linux_client_detail {

using namespace linux_ipc;

inline Napi::Value ReadValueToJs(Napi::Env env, Reader& r) {
    uint8_t vt = VAL_NULL;
    if (!r.u8(vt)) return env.Null();
    switch (vt) {
        case VAL_BOOL: {
            uint8_t b = 0;
            return r.u8(b) ? Napi::Boolean::New(env, b != 0) : env.Null();
        }
        case VAL_INT64: {
            int64_t v = 0;
            return r.i64(v) ? Napi::Number::New(env, static_cast<double>(v)) : env.Null();
        }
        case VAL_DOUBLE: {
            double v = 0.0;
            return r.f64(v) ? Napi::Number::New(env, v) : env.Null();
        }
        case VAL_STRING: {
            std::string s;
            return r.str(s) ? Napi::String::New(env, s) : env.Null();
        }
        case VAL_ARRAY: {
            uint32_t n = 0;
            if (!r.u32(n)) return env.Null();
            Napi::Array arr = Napi::Array::New(env, n);
            for (uint32_t i = 0; i < n; ++i) {
                arr.Set(i, ReadValueToJs(env, r));
            }
            return arr;
        }
        case VAL_MAP: {
            uint32_t n = 0;
            if (!r.u32(n)) return env.Null();
            Napi::Object obj = Napi::Object::New(env);
            for (uint32_t i = 0; i < n; ++i) {
                std::string k;
                if (!r.str(k)) break;
                obj.Set(k, ReadValueToJs(env, r));
            }
            return obj;
        }
        case VAL_NULL:
        default:
            return env.Null();
    }
}

inline std::string FindHelperBinaryPath() {
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void*>(&FindHelperBinaryPath), &info) != 0 && info.dli_fname) {
        std::string so_path(info.dli_fname);
        size_t slash = so_path.find_last_of('/');
        if (slash != std::string::npos) {
            return so_path.substr(0, slash + 1) + "electron-libmpv-helper";
        }
    }
    return "electron-libmpv-helper";
}

struct CachedSlot {
    int dmabuf_fd = -1;
    uint32_t stride = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t modifier = 0;
    bool supports_zero_copy_webgpu = false;
    std::string pixel_format = "bgra";
};

struct CachedPool {
    uint64_t generation = 0;
    int width = 0;
    int height = 0;
    bool hdr = false;
    std::array<CachedSlot, kSurfacePoolSize> slots;

    ~CachedPool() {
        for (auto& s : slots) {
            if (s.dmabuf_fd >= 0) {
                close(s.dmabuf_fd);
                s.dmabuf_fd = -1;
            }
        }
    }
};

}  // namespace linux_client_detail

class LinuxHelperMpvPlayer : public Napi::ObjectWrap<LinuxHelperMpvPlayer> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "MpvPlayer", {
            InstanceMethod("initialize", &LinuxHelperMpvPlayer::Initialize),
            InstanceMethod("resize", &LinuxHelperMpvPlayer::Resize),
            InstanceMethod("releaseFrame", &LinuxHelperMpvPlayer::ReleaseFrame),
            InstanceMethod("enableSoftwareFallback", &LinuxHelperMpvPlayer::EnableSoftwareFallback),
            InstanceMethod("command", &LinuxHelperMpvPlayer::Command),
            InstanceMethod("setProperty", &LinuxHelperMpvPlayer::SetProperty),
            InstanceMethod("getProperty", &LinuxHelperMpvPlayer::GetProperty),
            InstanceMethod("observeProperty", &LinuxHelperMpvPlayer::ObserveProperty),
            InstanceMethod("unobserveAll", &LinuxHelperMpvPlayer::UnobserveAll),
            InstanceMethod("getBackendInfo", &LinuxHelperMpvPlayer::GetBackendInfo),
            InstanceMethod("destroy", &LinuxHelperMpvPlayer::Destroy),
        });

        auto* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        env.SetInstanceData(constructor);

        exports.Set("MpvPlayer", func);
        return exports;
    }

    explicit LinuxHelperMpvPlayer(const Napi::CallbackInfo& info)
        : Napi::ObjectWrap<LinuxHelperMpvPlayer>(info) {
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
    }

    ~LinuxHelperMpvPlayer() override {
        ShutdownInternal();
    }

private:
    bool sendCmd(uint32_t type, const linux_ipc::Writer& w) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (sock_fd_ < 0 || destroyed_.load(std::memory_order_acquire)) return false;
        return linux_ipc::SendMessage(sock_fd_, type, w.data());
    }

    bool spawnHelper(std::string& out_error) {
        std::string helper_path = linux_client_detail::FindHelperBinaryPath();
        if (access(helper_path.c_str(), X_OK) != 0) {
            out_error = "electron-libmpv-helper executable not found at: " + helper_path;
            return false;
        }

        int sv[2] = { -1, -1 };
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
            out_error = "socketpair(AF_UNIX) failed";
            return false;
        }

        std::string fd_arg = std::to_string(sv[1]);
        pid_t pid = fork();
        if (pid < 0) {
            close(sv[0]);
            close(sv[1]);
            out_error = "fork() failed while spawning electron-libmpv-helper";
            return false;
        }

        if (pid == 0) {
            // Child process: close parent end, clear FD_CLOEXEC only in child so concurrent
            // parent forks cannot inherit sv[1], and exec the clean native helper binary.
            close(sv[0]);
            int flags = fcntl(sv[1], F_GETFD);
            if (flags >= 0) {
                fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
            }
            char* argv[] = {
                const_cast<char*>(helper_path.c_str()),
                const_cast<char*>(fd_arg.c_str()),
                nullptr
            };
            execv(helper_path.c_str(), argv);
            _exit(127);
        }

        close(sv[1]);
        sock_fd_ = sv[0];
        child_pid_ = pid;
        reader_thread_ = std::thread(&LinuxHelperMpvPlayer::readerThreadMain, this);
        return true;
    }

    void readerThreadMain() {
        uint32_t type = 0;
        std::vector<uint8_t> payload;
        std::vector<int> fds;

        while (!destroyed_.load(std::memory_order_acquire)) {
            if (!linux_ipc::RecvMessage(sock_fd_, type, payload, fds)) {
                break;
            }

            linux_ipc::Reader r(payload.data(), payload.size());

            switch (type) {
                case linux_ipc::EVT_INIT_RESULT: {
                    uint8_t ok = 0, st = 0, hdr = 0, loaded = 0;
                    std::string bname, lpath, lerr, gerr, err;
                    r.u8(ok);
                    r.u8(st);
                    r.u8(hdr);
                    r.u8(loaded);
                    r.str(bname);
                    r.str(lpath);
                    r.str(lerr);
                    r.str(gerr);
                    r.str(err);
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        init_done_ = true;
                        init_ok_ = (ok != 0);
                        backend_shared_texture_ = (st != 0);
                        backend_hdr_supported_ = (hdr != 0);
                        libmpv_loaded_ = (loaded != 0);
                        backend_name_ = bname;
                        libmpv_path_ = lpath;
                        libmpv_error_ = lerr;
                        gpu_error_ = gerr;
                        init_error_ = err;
                    }
                    init_cv_.notify_all();
                    break;
                }

                case linux_ipc::EVT_BACKEND_UPDATE: {
                    std::string bname;
                    uint8_t st = 0, hdr = 0;
                    if (r.str(bname) && r.u8(st) && r.u8(hdr)) {
                        {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            backend_name_ = bname;
                            backend_shared_texture_ = (st != 0);
                            backend_hdr_supported_ = (hdr != 0);
                        }
                        if (st == 0) {
                            std::lock_guard<std::mutex> lock(pool_mutex_);
                            active_pool_.reset();
                        }
                    }
                    break;
                }

                case linux_ipc::EVT_SURFACE_POOL: {
                    uint64_t gen = 0;
                    int32_t w = 0, h = 0;
                    uint8_t hdr = 0;
                    uint32_t count = 0;
                    if (r.u64(gen) && r.i32(w) && r.i32(h) && r.u8(hdr) && r.u32(count) &&
                        count == static_cast<uint32_t>(kSurfacePoolSize) && fds.size() == count) {
                        auto pool = std::make_shared<linux_client_detail::CachedPool>();
                        pool->generation = gen;
                        pool->width = w;
                        pool->height = h;
                        pool->hdr = (hdr != 0);
                        bool parse_ok = true;
                        for (uint32_t i = 0; i < count; ++i) {
                            auto& s = pool->slots[i];
                            s.dmabuf_fd = fds[i];
                            uint8_t zc = 0;
                            if (!r.u32(s.stride) || !r.u64(s.offset) || !r.u64(s.size) ||
                                !r.u64(s.modifier) || !r.u8(zc) || !r.str(s.pixel_format)) {
                                parse_ok = false;
                                break;
                            }
                            s.supports_zero_copy_webgpu = (zc != 0);
                        }
                        if (parse_ok) {
                            fds.clear();  // Ownership transferred to shared_ptr<CachedPool>.
                            std::lock_guard<std::mutex> lock(pool_mutex_);
                            active_pool_ = std::move(pool);
                        }
                    }
                    break;
                }

                case linux_ipc::EVT_GPU_FRAME: {
                    int32_t slot_id = -1;
                    uint64_t gen = 0;
                    int64_t ts = 0;
                    if (!r.i32(slot_id) || !r.u64(gen) || !r.i64(ts)) break;

                    std::shared_ptr<linux_client_detail::CachedPool> pool;
                    {
                        std::lock_guard<std::mutex> lock(pool_mutex_);
                        pool = active_pool_;
                    }
                    if (!pool || pool->generation != gen || slot_id < 0 || slot_id >= kSurfacePoolSize) {
                        linux_ipc::Writer rw;
                        rw.i32(slot_id);
                        rw.u64(gen);
                        sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                        break;
                    }
                    if (!has_frame_tsfn_ || destroyed_.load(std::memory_order_acquire)) {
                        linux_ipc::Writer rw;
                        rw.i32(slot_id);
                        rw.u64(gen);
                        sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                        break;
                    }

                    struct GpuDispatch {
                        std::shared_ptr<linux_client_detail::CachedPool> pool;
                        int32_t slot_id;
                        uint64_t gen;
                        int64_t ts;
                    };
                    auto* d = new GpuDispatch{ std::move(pool), slot_id, gen, ts };
                    auto alive = alive_;
                    napi_status st = frame_tsfn_.NonBlockingCall(
                        d,
                        [this, alive](Napi::Env env, Napi::Function js_cb, GpuDispatch* data) {
                            std::unique_ptr<GpuDispatch> guard(data);
                            if (!alive->load(std::memory_order_acquire) || !js_cb || js_cb.IsEmpty()) {
                                if (alive->load(std::memory_order_acquire)) {
                                    linux_ipc::Writer rw;
                                    rw.i32(guard->slot_id);
                                    rw.u64(guard->gen);
                                    sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                                }
                                return;
                            }
                            const auto& s = guard->pool->slots[guard->slot_id];
                            Napi::Object obj = Napi::Object::New(env);
                            obj.Set("kind", "sharedTexture");
                            obj.Set("slotId", Napi::Number::New(env, guard->slot_id));
                            obj.Set("generation", Napi::Number::New(env, static_cast<double>(guard->gen)));
                            obj.Set("width", Napi::Number::New(env, guard->pool->width));
                            obj.Set("height", Napi::Number::New(env, guard->pool->height));
                            obj.Set("pixelFormat", Napi::String::New(env, s.pixel_format));
                            obj.Set("hdr", Napi::Boolean::New(env, guard->pool->hdr));
                            obj.Set("timestamp", Napi::Number::New(env, static_cast<double>(guard->ts)));

                            Napi::Object plane = Napi::Object::New(env);
                            plane.Set("stride", Napi::Number::New(env, s.stride));
                            plane.Set("offset", Napi::Number::New(env, static_cast<double>(s.offset)));
                            plane.Set("size", Napi::Number::New(env, static_cast<double>(s.size)));
                            plane.Set("fd", Napi::Number::New(env, s.dmabuf_fd));

                            Napi::Array planes = Napi::Array::New(env, 1);
                            planes.Set(static_cast<uint32_t>(0), plane);

                            Napi::Object pixmap = Napi::Object::New(env);
                            pixmap.Set("planes", planes);
                            pixmap.Set("modifier", Napi::String::New(env, std::to_string(s.modifier)));
                            pixmap.Set("supportsZeroCopyWebGpuImport", Napi::Boolean::New(env, s.supports_zero_copy_webgpu));

                            Napi::Object handle = Napi::Object::New(env);
                            handle.Set("nativePixmap", pixmap);
                            obj.Set("handle", handle);

                            js_cb.Call({ obj });
                        }
                    );
                    if (st != napi_ok) {
                        delete d;
                        linux_ipc::Writer rw;
                        rw.i32(slot_id);
                        rw.u64(gen);
                        sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                    }
                    break;
                }

                case linux_ipc::EVT_SW_FRAME: {
                    int32_t slot_id = -1, w = 0, h = 0;
                    uint64_t gen = 0, stride = 0;
                    int64_t ts = 0;
                    const uint8_t* px = nullptr;
                    size_t px_len = 0;
                    if (!r.i32(slot_id) || !r.u64(gen) || !r.i32(w) || !r.i32(h) ||
                        !r.u64(stride) || !r.i64(ts) || !r.bytesView(px, px_len)) {
                        break;
                    }
                    if (!has_frame_tsfn_ || destroyed_.load(std::memory_order_acquire)) {
                        linux_ipc::Writer rw;
                        rw.i32(slot_id);
                        rw.u64(gen);
                        sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                        break;
                    }

                    struct SwDispatch {
                        int32_t slot_id;
                        uint64_t gen;
                        int32_t w;
                        int32_t h;
                        uint64_t stride;
                        int64_t ts;
                        std::vector<uint8_t> pixels;
                    };
                    auto* d = new SwDispatch{ slot_id, gen, w, h, stride, ts, std::vector<uint8_t>(px, px + px_len) };
                    auto alive = alive_;
                    napi_status st = frame_tsfn_.NonBlockingCall(
                        d,
                        [this, alive](Napi::Env env, Napi::Function js_cb, SwDispatch* data) {
                            std::unique_ptr<SwDispatch> guard(data);
                            if (!alive->load(std::memory_order_acquire) || !js_cb || js_cb.IsEmpty()) {
                                if (alive->load(std::memory_order_acquire)) {
                                    linux_ipc::Writer rw;
                                    rw.i32(guard->slot_id);
                                    rw.u64(guard->gen);
                                    sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                                }
                                return;
                            }
                            Napi::Object obj = Napi::Object::New(env);
                            obj.Set("kind", "software");
                            obj.Set("slotId", Napi::Number::New(env, guard->slot_id));
                            obj.Set("generation", Napi::Number::New(env, static_cast<double>(guard->gen)));
                            obj.Set("width", Napi::Number::New(env, guard->w));
                            obj.Set("height", Napi::Number::New(env, guard->h));
                            obj.Set("pixelFormat", Napi::String::New(env, "rgba"));
                            obj.Set("hdr", Napi::Boolean::New(env, false));
                            obj.Set("timestamp", Napi::Number::New(env, static_cast<double>(guard->ts)));
                            obj.Set("stride", Napi::Number::New(env, static_cast<double>(guard->stride)));
                            obj.Set("buffer", Napi::Buffer<uint8_t>::Copy(env, guard->pixels.data(), guard->pixels.size()));
                            js_cb.Call({ obj });
                        }
                    );
                    if (st != napi_ok) {
                        delete d;
                        linux_ipc::Writer rw;
                        rw.i32(slot_id);
                        rw.u64(gen);
                        sendCmd(linux_ipc::CMD_RELEASE_FRAME, rw);
                    }
                    break;
                }

                case linux_ipc::EVT_MPV_EVENT: {
                    if (!has_event_tsfn_ || destroyed_.load(std::memory_order_acquire)) break;
                    auto* raw = new std::vector<uint8_t>(std::move(payload));
                    auto alive = alive_;
                    napi_status st = event_tsfn_.NonBlockingCall(
                        raw,
                        [alive](Napi::Env env, Napi::Function js_cb, std::vector<uint8_t>* buf) {
                            std::unique_ptr<std::vector<uint8_t>> guard(buf);
                            if (!alive->load(std::memory_order_acquire) || !js_cb || js_cb.IsEmpty()) return;

                            linux_ipc::Reader er(guard->data(), guard->size());
                            uint8_t kind = 0;
                            if (!er.u8(kind)) return;

                            Napi::Object obj = Napi::Object::New(env);
                            switch (kind) {
                                case linux_ipc::MEVT_START_FILE:
                                    obj.Set("type", "start-file");
                                    break;
                                case linux_ipc::MEVT_FILE_LOADED:
                                    obj.Set("type", "file-loaded");
                                    break;
                                case linux_ipc::MEVT_PLAYBACK_RESTART:
                                    obj.Set("type", "playback-restart");
                                    break;
                                case linux_ipc::MEVT_END_FILE: {
                                    std::string reason;
                                    uint8_t has_err = 0;
                                    er.str(reason);
                                    er.u8(has_err);
                                    obj.Set("type", "end-file");
                                    obj.Set("reason", reason);
                                    if (has_err) {
                                        std::string err_str;
                                        er.str(err_str);
                                        obj.Set("error", err_str);
                                    }
                                    break;
                                }
                                case linux_ipc::MEVT_PROPERTY_CHANGE: {
                                    std::string name;
                                    if (!er.str(name)) return;
                                    obj.Set("type", "property-change");
                                    obj.Set("name", name);
                                    obj.Set("value", linux_client_detail::ReadValueToJs(env, er));
                                    break;
                                }
                                case linux_ipc::MEVT_LOG_MESSAGE: {
                                    std::string prefix, level, text;
                                    er.str(prefix);
                                    er.str(level);
                                    er.str(text);
                                    obj.Set("type", "log-message");
                                    obj.Set("prefix", prefix);
                                    obj.Set("level", level);
                                    obj.Set("text", text);
                                    break;
                                }
                                default:
                                    return;
                            }
                            js_cb.Call({ obj });
                        }
                    );
                    if (st != napi_ok) delete raw;
                    break;
                }

                case linux_ipc::EVT_GET_PROP_RESULT: {
                    uint64_t req_id = 0;
                    if (r.u64(req_id)) {
                        std::lock_guard<std::mutex> lock(prop_mutex_);
                        if (req_id == pending_prop_req_id_) {
                            pending_prop_payload_ = std::move(payload);
                            pending_prop_done_ = true;
                            prop_cv_.notify_all();
                        }
                    }
                    break;
                }

                default:
                    break;
            }

            for (int fd : fds) close(fd);
            fds.clear();
        }

        // Unblock any waiters if helper exited unexpectedly.
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!init_done_) {
                init_done_ = true;
                init_ok_ = false;
                if (init_error_.empty()) init_error_ = "electron-libmpv-helper exited unexpectedly during init";
            }
        }
        init_cv_.notify_all();
        {
            std::lock_guard<std::mutex> lock(prop_mutex_);
            pending_prop_done_ = true;
        }
        prop_cv_.notify_all();
    }

    void stopHelperProcess() {
        if (sock_fd_ >= 0) {
            linux_ipc::Writer w;
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                linux_ipc::SendMessage(sock_fd_, linux_ipc::CMD_SHUTDOWN, w.data());
            }
            shutdown(sock_fd_, SHUT_RDWR);
        }

        if (reader_thread_.joinable()) {
            reader_thread_.join();
        }

        if (sock_fd_ >= 0) {
            close(sock_fd_);
            sock_fd_ = -1;
        }

        if (child_pid_ > 0) {
            int status = 0;
            bool reaped = false;
            for (int i = 0; i < 20; ++i) {
                pid_t r = waitpid(child_pid_, &status, WNOHANG);
                if (r == child_pid_ || r < 0) {
                    reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!reaped) {
                kill(child_pid_, SIGTERM);
                for (int i = 0; i < 50; ++i) {
                    pid_t r = waitpid(child_pid_, &status, WNOHANG);
                    if (r == child_pid_ || r < 0) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            if (!reaped) {
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0);
            }
            child_pid_ = -1;
        }

        {
            std::lock_guard<std::mutex> lock(pool_mutex_);
            active_pool_.reset();
        }
    }

    void ShutdownInternal() {
        bool expected = false;
        if (!destroyed_.compare_exchange_strong(expected, true)) {
            return;
        }
        alive_->store(false, std::memory_order_release);

        stopHelperProcess();

        if (has_event_tsfn_) {
            event_tsfn_.Release();
            has_event_tsfn_ = false;
        }
        if (has_frame_tsfn_) {
            frame_tsfn_.Release();
            has_frame_tsfn_ = false;
        }
    }

    Napi::Value Initialize(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Object result = Napi::Object::New(env);

        if (initialized_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            result.Set("ok", true);
            result.Set("backend", backend_name_);
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

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            init_done_ = false;
            init_ok_ = false;
            init_error_.clear();
        }

        std::string spawn_err;
        if (!spawnHelper(spawn_err)) {
            result.Set("ok", false);
            result.Set("error", spawn_err);
            return result;
        }

        linux_ipc::Writer w;
        w.i32(target_width_);
        w.i32(target_height_);
        w.u8(target_hdr_ ? 1 : 0);
        w.u8(prefer_gpu ? 1 : 0);
        w.str(custom_libmpv_path_);
        w.str(mpv_config_path_);
        sendCmd(linux_ipc::CMD_INIT, w);

        bool ok = false;
        std::string err_copy;
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            init_cv_.wait(lock, [this]() { return init_done_; });
            ok = init_ok_;
            err_copy = init_error_;
            if (ok) {
                initialized_.store(true, std::memory_order_release);
                result.Set("ok", true);
                result.Set("backend", backend_name_);
                result.Set("sharedTexture", backend_shared_texture_);
                result.Set("hdrSupported", backend_hdr_supported_);
                result.Set("libmpvPath", libmpv_path_);
                if (!gpu_error_.empty()) {
                    result.Set("gpuError", gpu_error_);
                }
            }
        }
        if (!ok) {
            stopHelperProcess();
            result.Set("ok", false);
            result.Set("error", err_copy);
        }
        return result;
    }

    Napi::Value Resize(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire) || info.Length() < 2) {
            return env.Undefined();
        }
        int32_t w = std::max(16, info[0].As<Napi::Number>().Int32Value());
        int32_t h = std::max(16, info[1].As<Napi::Number>().Int32Value());
        bool has_hdr = (info.Length() >= 3 && info[2].IsBoolean());
        bool hdr = has_hdr ? info[2].As<Napi::Boolean>().Value() : false;

        linux_ipc::Writer wr;
        wr.i32(w);
        wr.i32(h);
        wr.u8(has_hdr ? 1 : 0);
        wr.u8(hdr ? 1 : 0);
        sendCmd(linux_ipc::CMD_RESIZE, wr);
        return env.Undefined();
    }

    Napi::Value ReleaseFrame(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire) || info.Length() < 1) {
            return env.Undefined();
        }
        int32_t slot = info[0].As<Napi::Number>().Int32Value();
        uint64_t gen = (info.Length() >= 2 && info[1].IsNumber())
            ? static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value())
            : std::numeric_limits<uint64_t>::max();
        linux_ipc::Writer wr;
        wr.i32(slot);
        wr.u64(gen);
        sendCmd(linux_ipc::CMD_RELEASE_FRAME, wr);
        return env.Undefined();
    }

    Napi::Value EnableSoftwareFallback(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire)) return Napi::Boolean::New(env, false);
        linux_ipc::Writer wr;
        return Napi::Boolean::New(env, sendCmd(linux_ipc::CMD_SW_FALLBACK, wr));
    }

    Napi::Value Command(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire) || info.Length() == 0) {
            return Napi::Number::New(env, -1);
        }
        linux_ipc::Writer wr;
        wr.u32(static_cast<uint32_t>(info.Length()));
        for (size_t i = 0; i < info.Length(); ++i) {
            wr.str(info[i].ToString().Utf8Value());
        }
        return Napi::Number::New(env, sendCmd(linux_ipc::CMD_COMMAND, wr) ? 0 : -1);
    }

    Napi::Value SetProperty(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire) || info.Length() < 2) {
            return Napi::Number::New(env, -1);
        }
        std::string name = info[0].ToString().Utf8Value();
        Napi::Value val = info[1];
        linux_ipc::Writer wr;
        wr.str(name);
        if (val.IsBoolean()) {
            wr.u8(linux_ipc::VAL_BOOL);
            wr.u8(val.As<Napi::Boolean>().Value() ? 1 : 0);
        } else if (val.IsNumber()) {
            wr.u8(linux_ipc::VAL_DOUBLE);
            wr.f64(val.As<Napi::Number>().DoubleValue());
        } else {
            wr.u8(linux_ipc::VAL_STRING);
            wr.str(val.ToString().Utf8Value());
        }
        return Napi::Number::New(env, sendCmd(linux_ipc::CMD_SET_PROP, wr) ? 0 : -1);
    }

    Napi::Value GetProperty(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire) || info.Length() < 1) {
            return env.Null();
        }
        std::string name = info[0].ToString().Utf8Value();
        std::string fmt = (info.Length() >= 2 && info[1].IsString())
            ? info[1].As<Napi::String>().Utf8Value()
            : "string";

        uint64_t req_id = next_prop_req_id_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(prop_mutex_);
            pending_prop_req_id_ = req_id;
            pending_prop_done_ = false;
            pending_prop_payload_.clear();
        }

        linux_ipc::Writer wr;
        wr.u64(req_id);
        wr.str(name);
        wr.str(fmt);
        if (!sendCmd(linux_ipc::CMD_GET_PROP, wr)) {
            return env.Null();
        }

        std::vector<uint8_t> resp;
        {
            std::unique_lock<std::mutex> lock(prop_mutex_);
            prop_cv_.wait_for(lock, std::chrono::milliseconds(1000), [this]() { return pending_prop_done_; });
            if (!pending_prop_done_ || pending_prop_payload_.size() < sizeof(uint64_t)) {
                return env.Null();
            }
            resp = std::move(pending_prop_payload_);
        }

        linux_ipc::Reader rr(resp.data(), resp.size());
        uint64_t resp_id = 0;
        if (!rr.u64(resp_id) || resp_id != req_id) return env.Null();
        return linux_client_detail::ReadValueToJs(env, rr);
    }

    Napi::Value ObserveProperty(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire) || info.Length() < 1) {
            return Napi::Number::New(env, -1);
        }
        std::string name = info[0].ToString().Utf8Value();
        std::string fmt = (info.Length() >= 2 && info[1].IsString())
            ? info[1].As<Napi::String>().Utf8Value()
            : "string";
        linux_ipc::Writer wr;
        wr.str(name);
        wr.str(fmt);
        return Napi::Number::New(env, sendCmd(linux_ipc::CMD_OBSERVE_PROP, wr) ? 0 : -1);
    }

    Napi::Value UnobserveAll(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (!initialized_.load(std::memory_order_acquire)) return Napi::Number::New(env, -1);
        linux_ipc::Writer wr;
        return Napi::Number::New(env, sendCmd(linux_ipc::CMD_UNOBSERVE_ALL, wr) ? 0 : -1);
    }

    Napi::Value GetBackendInfo(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Object obj = Napi::Object::New(env);
        std::lock_guard<std::mutex> lock(state_mutex_);
        obj.Set("libmpvLoaded", libmpv_loaded_);
        obj.Set("libmpvPath", libmpv_path_);
        obj.Set("libmpvError", libmpv_error_);
        obj.Set("initialized", initialized_.load(std::memory_order_acquire));
        obj.Set("backend", backend_name_);
        obj.Set("sharedTexture", backend_shared_texture_);
        obj.Set("hdrSupported", backend_hdr_supported_);
        return obj;
    }

    Napi::Value Destroy(const Napi::CallbackInfo& info) {
        ShutdownInternal();
        return info.Env().Undefined();
    }

    int sock_fd_ = -1;
    pid_t child_pid_ = -1;
    std::mutex write_mutex_;
    std::thread reader_thread_;

    Napi::ThreadSafeFunction event_tsfn_;
    Napi::ThreadSafeFunction frame_tsfn_;
    bool has_event_tsfn_ = false;
    bool has_frame_tsfn_ = false;

    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> destroyed_{false};
    std::atomic<bool> initialized_{false};

    std::mutex state_mutex_;
    std::condition_variable init_cv_;
    bool init_done_ = false;
    bool init_ok_ = false;
    bool libmpv_loaded_ = false;
    bool backend_shared_texture_ = false;
    bool backend_hdr_supported_ = false;
    std::string backend_name_ = "uninitialized";
    std::string libmpv_path_;
    std::string libmpv_error_;
    std::string gpu_error_;
    std::string init_error_;

    std::mutex pool_mutex_;
    std::shared_ptr<linux_client_detail::CachedPool> active_pool_;

    std::mutex prop_mutex_;
    std::condition_variable prop_cv_;
    std::atomic<uint64_t> next_prop_req_id_{1};
    uint64_t pending_prop_req_id_ = 0;
    bool pending_prop_done_ = false;
    std::vector<uint8_t> pending_prop_payload_;

    int target_width_ = 1280;
    int target_height_ = 720;
    bool target_hdr_ = false;
    std::string custom_libmpv_path_;
    std::string mpv_config_path_;
};

}  // namespace electron_mpv

#endif  // __linux__
