#ifdef __linux__

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/prctl.h>
#include <unistd.h>

#include "mpv_loader.h"
#include "gpu_backend.h"
#include "gpu_backend_linux.h"
#include "gpu_backend_sw.h"
#include "linux_ipc_proto.h"

namespace electron_mpv {
namespace {

using namespace linux_ipc;

void WriteMpvNode(Writer& w, const mpv_node& node) {
    switch (node.format) {
        case MPV_FORMAT_STRING:
            if (node.u.string) {
                w.u8(VAL_STRING);
                w.str(node.u.string);
            } else {
                w.u8(VAL_NULL);
            }
            break;
        case MPV_FORMAT_FLAG:
            w.u8(VAL_BOOL);
            w.u8(node.u.flag ? 1 : 0);
            break;
        case MPV_FORMAT_INT64:
            w.u8(VAL_INT64);
            w.i64(node.u.int64);
            break;
        case MPV_FORMAT_DOUBLE:
            w.u8(VAL_DOUBLE);
            w.f64(node.u.double_);
            break;
        case MPV_FORMAT_NODE_ARRAY: {
            w.u8(VAL_ARRAY);
            int num = node.u.list ? node.u.list->num : 0;
            w.u32(static_cast<uint32_t>(std::max(0, num)));
            for (int i = 0; i < num; ++i) {
                WriteMpvNode(w, node.u.list->values[i]);
            }
            break;
        }
        case MPV_FORMAT_NODE_MAP: {
            w.u8(VAL_MAP);
            int num = node.u.list ? node.u.list->num : 0;
            uint32_t valid = 0;
            for (int i = 0; i < num; ++i) {
                if (node.u.list->keys && node.u.list->keys[i]) ++valid;
            }
            w.u32(valid);
            for (int i = 0; i < num; ++i) {
                if (node.u.list->keys && node.u.list->keys[i]) {
                    w.str(node.u.list->keys[i]);
                    WriteMpvNode(w, node.u.list->values[i]);
                }
            }
            break;
        }
        case MPV_FORMAT_NONE:
        default:
            w.u8(VAL_NULL);
            break;
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

class HelperApp {
public:
    explicit HelperApp(int sock_fd) : sock_fd_(sock_fd) {}

    ~HelperApp() {
        shutdown();
    }

    int run() {
        uint32_t type = 0;
        std::vector<uint8_t> payload;
        std::vector<int> fds;

        while (!stop_.load(std::memory_order_acquire)) {
            if (!RecvMessage(sock_fd_, type, payload, fds)) {
                break;
            }
            for (int fd : fds) close(fd);
            fds.clear();

            Reader r(payload.data(), payload.size());
            if (!handleCommand(type, r)) {
                break;
            }
        }

        shutdown();
        return 0;
    }

private:
    bool sendMsg(uint32_t type, const Writer& w, const std::vector<int>& fds = {}) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (sock_fd_ < 0) return false;
        return SendMessage(sock_fd_, type, w.data(), fds);
    }

    void publishSurfacePool(LinuxDmaBufBackend* gpu, uint64_t generation) {
        Writer w;
        w.u64(generation);
        w.i32(gpu->currentWidth());
        w.i32(gpu->currentHeight());
        w.u8(gpu->currentHdr() ? 1 : 0);
        w.u32(static_cast<uint32_t>(kSurfacePoolSize));

        std::vector<int> fds;
        fds.reserve(kSurfacePoolSize);
        for (int i = 0; i < kSurfacePoolSize; ++i) {
            LinuxDmaBufBackend::ExportedSlot slot;
            if (!gpu->getExportedSlot(i, slot)) {
                return;
            }
            fds.push_back(slot.dmabuf_fd);
            w.u32(slot.stride);
            w.u64(slot.offset);
            w.u64(slot.size);
            w.u64(slot.modifier);
            w.u8(0);  // supports_zero_copy_webgpu
            w.str(slot.pixel_format);
        }
        sendMsg(EVT_SURFACE_POOL, w, fds);
    }

    static void OnRenderUpdate(void* ctx) {
        auto* self = static_cast<HelperApp*>(ctx);
        if (!self || self->stop_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lock(self->render_mutex_);
            self->render_frame_pending_ = true;
        }
        self->render_cv_.notify_one();
    }

    void eventThreadMain() {
        while (!stop_.load(std::memory_order_acquire) && mpv_ && api_.loaded) {
            mpv_event* ev = api_.wait_event(mpv_, 0.25);
            if (!ev || ev->event_id == MPV_EVENT_NONE) continue;
            if (ev->event_id == MPV_EVENT_SHUTDOWN) break;

            Writer w;
            bool emit = true;

            switch (ev->event_id) {
                case MPV_EVENT_START_FILE:
                    w.u8(MEVT_START_FILE);
                    break;

                case MPV_EVENT_FILE_LOADED:
                    w.u8(MEVT_FILE_LOADED);
                    break;

                case MPV_EVENT_PLAYBACK_RESTART:
                    w.u8(MEVT_PLAYBACK_RESTART);
                    break;

                case MPV_EVENT_END_FILE: {
                    auto* ef = static_cast<mpv_event_end_file*>(ev->data);
                    w.u8(MEVT_END_FILE);
                    w.str(ef ? EndFileReasonToString(ef->reason) : "other");
                    if (ef && ef->error < 0) {
                        w.u8(1);
                        w.str(api_.error_string(ef->error));
                    } else {
                        w.u8(0);
                    }
                    break;
                }

                case MPV_EVENT_PROPERTY_CHANGE: {
                    auto* prop = static_cast<mpv_event_property*>(ev->data);
                    if (!prop || !prop->name) {
                        emit = false;
                        break;
                    }
                    w.u8(MEVT_PROPERTY_CHANGE);
                    w.str(prop->name);
                    if (!prop->data) {
                        w.u8(VAL_NULL);
                    } else {
                        switch (prop->format) {
                            case MPV_FORMAT_STRING: {
                                const char* str = *static_cast<const char**>(prop->data);
                                if (str) {
                                    w.u8(VAL_STRING);
                                    w.str(str);
                                } else {
                                    w.u8(VAL_NULL);
                                }
                                break;
                            }
                            case MPV_FORMAT_FLAG: {
                                int flag = *static_cast<int*>(prop->data);
                                w.u8(VAL_BOOL);
                                w.u8(flag ? 1 : 0);
                                break;
                            }
                            case MPV_FORMAT_INT64: {
                                int64_t val = *static_cast<int64_t*>(prop->data);
                                w.u8(VAL_INT64);
                                w.i64(val);
                                break;
                            }
                            case MPV_FORMAT_DOUBLE: {
                                double val = *static_cast<double*>(prop->data);
                                w.u8(VAL_DOUBLE);
                                w.f64(val);
                                break;
                            }
                            case MPV_FORMAT_NODE: {
                                auto* node = static_cast<mpv_node*>(prop->data);
                                WriteMpvNode(w, *node);
                                break;
                            }
                            default:
                                w.u8(VAL_NULL);
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
                    w.u8(MEVT_LOG_MESSAGE);
                    w.str(lm->prefix ? lm->prefix : "");
                    w.str(lm->level ? lm->level : "");
                    w.str(lm->text);
                    break;
                }

                default:
                    emit = false;
                    break;
            }

            if (emit) {
                if (!sendMsg(EVT_MPV_EVENT, w)) break;
            }
        }
    }

    void renderThreadMain(bool prefer_gpu) {
        std::string init_error;
        std::string gpu_error;
        bool init_ok = false;
        LinuxDmaBufBackend* gpu_ptr = nullptr;

        if (prefer_gpu) {
            auto gpu = std::make_unique<LinuxDmaBufBackend>();
            if (gpu->init(api_, mpv_, &render_ctx_, gpu_error)) {
                gpu_ptr = gpu.get();
                backend_ = std::move(gpu);
                init_ok = true;
            } else {
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
                    gpu_ptr = nullptr;
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

        if (init_ok && gpu_ptr) {
            publishSurfacePool(gpu_ptr, surface_generation_.load(std::memory_order_acquire));
        }

        if (init_ok && render_ctx_ && backend_) {
            api_.render_context_set_update_callback(render_ctx_, &HelperApp::OnRenderUpdate, this);
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
                    render_frame_pending_ = false;
                }
            }

            if (do_fallback && backend_->supportsSharedTexture()) {
                api_.render_context_set_update_callback(render_ctx_, nullptr, nullptr);
                surface_generation_.fetch_add(1, std::memory_order_acq_rel);
                gpu_ptr = nullptr;
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
                    api_.render_context_set_update_callback(render_ctx_, &HelperApp::OnRenderUpdate, this);
                    {
                        std::lock_guard<std::mutex> lock(render_mutex_);
                        backend_name_ = backend_->name();
                        backend_shared_texture_ = false;
                        backend_hdr_supported_ = false;
                    }
                    Writer bw;
                    bw.str(backend_->name());
                    bw.u8(0);
                    bw.u8(0);
                    sendMsg(EVT_BACKEND_UPDATE, bw);
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
                        uint64_t new_gen = surface_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
                        if (gpu_ptr) {
                            publishSurfacePool(gpu_ptr, new_gen);
                        }
                        force_redraw = true;
                    }
                }
            }

            uint64_t update_flags = api_.render_context_update(render_ctx_);
            bool should_render = ((update_flags & MPV_RENDER_UPDATE_FRAME) != 0) || force_redraw;
            if (!should_render) continue;

            int slot = backend_->acquireFreeSlot();
            if (slot < 0) {
                int skip = 1;
                mpv_render_param skip_params[] = {
                    { MPV_RENDER_PARAM_SKIP_RENDERING, &skip },
                    { MPV_RENDER_PARAM_INVALID, nullptr },
                };
                api_.render_context_render(render_ctx_, skip_params);
                api_.render_context_report_swap(render_ctx_);
                continue;
            }

            uint64_t gen = surface_generation_.load(std::memory_order_acquire);
            FrameDescriptor frame;
            std::string render_err;
            if (!backend_->renderFrame(api_, render_ctx_, slot, frame, render_err)) {
                backend_->releaseSlot(slot);
                continue;
            }

            if (frame.type == FrameHandleType::LinuxNativePixmap) {
                Writer fw;
                fw.i32(frame.slot_id);
                fw.u64(gen);
                fw.i64(frame.timestamp_us);
                if (!sendMsg(EVT_GPU_FRAME, fw)) {
                    backend_->releaseSlot(slot);
                    break;
                }
            } else if (frame.type == FrameHandleType::SoftwareRgba) {
                Writer fw;
                fw.i32(frame.slot_id);
                fw.u64(gen);
                fw.i32(frame.width);
                fw.i32(frame.height);
                fw.u64(static_cast<uint64_t>(frame.sw_stride));
                fw.i64(frame.timestamp_us);
                fw.bytes(frame.sw_pixels.data(), frame.sw_pixels.size());
                if (!sendMsg(EVT_SW_FRAME, fw)) {
                    backend_->releaseSlot(slot);
                    break;
                }
            } else {
                backend_->releaseSlot(slot);
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

    void sendInitResult(bool ok, const std::string& error) {
        Writer w;
        w.u8(ok ? 1 : 0);
        w.u8(backend_shared_texture_ ? 1 : 0);
        w.u8(backend_hdr_supported_ ? 1 : 0);
        w.u8(api_.loaded ? 1 : 0);
        w.str(backend_name_);
        w.str(api_.loaded_path);
        w.str(api_.last_error);
        w.str(gpu_init_error_);
        w.str(error);
        sendMsg(EVT_INIT_RESULT, w);
    }

    bool handleCommand(uint32_t type, Reader& r) {
        switch (type) {
            case CMD_INIT: {
                int32_t w = 1280, h = 720;
                uint8_t hdr = 0, prefer_gpu = 1;
                std::string libmpv_path, config_path;
                if (!r.i32(w) || !r.i32(h) || !r.u8(hdr) || !r.u8(prefer_gpu) ||
                    !r.str(libmpv_path) || !r.str(config_path)) {
                    sendInitResult(false, "Malformed CMD_INIT");
                    return false;
                }
                target_width_ = std::max(16, static_cast<int>(w));
                target_height_ = std::max(16, static_cast<int>(h));
                target_hdr_ = (hdr != 0);

                if (!api_.load(libmpv_path)) {
                    sendInitResult(false, api_.last_error);
                    return true;
                }

                std::setlocale(LC_NUMERIC, "C");
                mpv_ = api_.create();
                if (!mpv_) {
                    sendInitResult(false, "mpv_create() returned null");
                    return true;
                }

                api_.set_option_string(mpv_, "title", "Stremio Enhanced");
                api_.set_option_string(mpv_, "audio-client-name", "Stremio Enhanced");
                api_.set_option_string(mpv_, "video-timing-offset", "0");
                api_.set_option_string(mpv_, "video-sync", "audio");
                api_.set_option_string(mpv_, "hwdec", "vaapi,vaapi-copy,auto-copy");
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

                if (!config_path.empty()) {
                    api_.set_option_string(mpv_, "include", config_path.c_str());
                }

                api_.set_option_string(mpv_, "vo", "libmpv");

                int err = api_.initialize(mpv_);
                if (err < 0) {
                    std::string msg = std::string("mpv_initialize() failed: ") + api_.error_string(err);
                    api_.destroy(mpv_);
                    mpv_ = nullptr;
                    sendInitResult(false, msg);
                    return true;
                }

                api_.request_log_messages(mpv_, "info");

                render_init_done_ = false;
                render_init_ok_ = false;
                render_stop_ = false;
                render_thread_ = std::thread(&HelperApp::renderThreadMain, this, prefer_gpu != 0);

                {
                    std::unique_lock<std::mutex> lock(render_mutex_);
                    init_cv_.wait(lock, [this]() { return render_init_done_; });
                }

                if (!render_init_ok_) {
                    if (render_thread_.joinable()) render_thread_.join();
                    api_.terminate_destroy(mpv_);
                    mpv_ = nullptr;
                    sendInitResult(false, render_init_error_);
                    return true;
                }

                event_thread_ = std::thread(&HelperApp::eventThreadMain, this);
                sendInitResult(true, "");
                return true;
            }

            case CMD_RESIZE: {
                int32_t w = 0, h = 0;
                uint8_t has_hdr = 0, hdr = 0;
                if (!r.i32(w) || !r.i32(h) || !r.u8(has_hdr) || !r.u8(hdr)) return true;
                w = std::max(16, static_cast<int>(w));
                h = std::max(16, static_cast<int>(h));
                {
                    std::lock_guard<std::mutex> lock(render_mutex_);
                    bool req_hdr = has_hdr ? (hdr != 0) : target_hdr_;
                    if (w == target_width_ && h == target_height_ && req_hdr == target_hdr_) {
                        return true;
                    }
                    target_width_ = w;
                    target_height_ = h;
                    target_hdr_ = req_hdr;
                    render_resize_pending_ = true;
                }
                render_cv_.notify_one();
                return true;
            }

            case CMD_RELEASE_FRAME: {
                int32_t slot = -1;
                uint64_t gen = 0;
                if (!r.i32(slot) || !r.u64(gen)) return true;
                std::lock_guard<std::mutex> blk(backend_mutex_);
                if (backend_ && (gen == std::numeric_limits<uint64_t>::max() ||
                                 gen == surface_generation_.load(std::memory_order_acquire))) {
                    backend_->releaseSlot(slot);
                }
                return true;
            }

            case CMD_SW_FALLBACK: {
                {
                    std::lock_guard<std::mutex> lock(render_mutex_);
                    render_fallback_requested_ = true;
                }
                render_cv_.notify_one();
                return true;
            }

            case CMD_COMMAND: {
                uint32_t argc = 0;
                if (!r.u32(argc) || !mpv_ || !api_.loaded || argc == 0) return true;
                std::vector<std::string> args_str(argc);
                for (uint32_t i = 0; i < argc; ++i) {
                    if (!r.str(args_str[i])) return true;
                }
                std::vector<const char*> args_ptr;
                args_ptr.reserve(argc + 1);
                for (const auto& s : args_str) args_ptr.push_back(s.c_str());
                args_ptr.push_back(nullptr);
                api_.command_async(mpv_, 0, args_ptr.data());
                return true;
            }

            case CMD_SET_PROP: {
                std::string name;
                uint8_t vt = 0;
                if (!r.str(name) || !r.u8(vt) || !mpv_ || !api_.loaded) return true;
                if (vt == VAL_BOOL) {
                    uint8_t b = 0;
                    if (!r.u8(b)) return true;
                    int flag = b ? 1 : 0;
                    if (api_.set_property(mpv_, name.c_str(), MPV_FORMAT_FLAG, &flag) < 0) {
                        api_.set_property_string(mpv_, name.c_str(), flag ? "yes" : "no");
                    }
                } else if (vt == VAL_DOUBLE) {
                    double d = 0.0;
                    if (!r.f64(d)) return true;
                    int res = -1;
                    bool is_int = std::isfinite(d) && std::trunc(d) == d && std::abs(d) <= 9007199254740991.0;
                    if (is_int) {
                        int64_t i64 = static_cast<int64_t>(d);
                        res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_INT64, &i64);
                        if (res < 0) {
                            res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_DOUBLE, &d);
                        }
                    } else {
                        res = api_.set_property(mpv_, name.c_str(), MPV_FORMAT_DOUBLE, &d);
                    }
                    if (res < 0) {
                        std::string s;
                        if (is_int) {
                            s = std::to_string(static_cast<int64_t>(d));
                        } else {
                            char buf[64];
                            std::snprintf(buf, sizeof(buf), "%.15g", d);
                            s = buf;
                        }
                        api_.set_property_string(mpv_, name.c_str(), s.c_str());
                    }
                } else if (vt == VAL_STRING) {
                    std::string s;
                    if (!r.str(s)) return true;
                    api_.set_property_string(mpv_, name.c_str(), s.c_str());
                }
                return true;
            }

            case CMD_GET_PROP: {
                uint64_t req_id = 0;
                std::string name, fmt;
                if (!r.u64(req_id) || !r.str(name) || !r.str(fmt)) return true;
                Writer w;
                w.u64(req_id);
                if (!mpv_ || !api_.loaded) {
                    w.u8(VAL_NULL);
                } else if (fmt == "node" || fmt == "json") {
                    mpv_node node = {};
                    if (api_.get_property(mpv_, name.c_str(), MPV_FORMAT_NODE, &node) >= 0) {
                        WriteMpvNode(w, node);
                        api_.free_node_contents(&node);
                    } else {
                        w.u8(VAL_NULL);
                    }
                } else {
                    char* res = api_.get_property_string(mpv_, name.c_str());
                    if (res) {
                        w.u8(VAL_STRING);
                        w.str(res);
                        api_.free(res);
                    } else {
                        w.u8(VAL_NULL);
                    }
                }
                sendMsg(EVT_GET_PROP_RESULT, w);
                return true;
            }

            case CMD_OBSERVE_PROP: {
                std::string name, fmt;
                if (!r.str(name) || !r.str(fmt) || !mpv_ || !api_.loaded) return true;
                api_.observe_property(mpv_, 1, name.c_str(), ParseObserveFormat(fmt));
                return true;
            }

            case CMD_UNOBSERVE_ALL: {
                if (mpv_ && api_.loaded) {
                    api_.unobserve_property(mpv_, 1);
                }
                return true;
            }

            case CMD_SHUTDOWN:
                return false;

            default:
                return true;
        }
    }

    void shutdown() {
        bool expected = false;
        if (!stop_.compare_exchange_strong(expected, true)) {
            return;
        }

        if (render_thread_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(render_mutex_);
                render_stop_ = true;
            }
            render_cv_.notify_all();
            render_thread_.join();
        }

        if (mpv_ && api_.loaded) {
            const char* quit_cmd[] = { "quit", nullptr };
            api_.command_async(mpv_, 0, quit_cmd);
        }

        if (event_thread_.joinable()) {
            event_thread_.join();
        }

        if (mpv_ && api_.loaded) {
            api_.terminate_destroy(mpv_);
            mpv_ = nullptr;
        }

        if (sock_fd_ >= 0) {
            close(sock_fd_);
            sock_fd_ = -1;
        }
    }

    int sock_fd_ = -1;
    std::mutex write_mutex_;
    std::atomic<bool> stop_{false};

    MpvApi api_;
    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ctx_ = nullptr;
    std::unique_ptr<RenderBackend> backend_;
    std::mutex backend_mutex_;

    std::thread event_thread_;
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
};

}  // namespace
}  // namespace electron_mpv

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) {
        return 0;
    }
    if (argc < 2) {
        return 1;
    }
    int sock_fd = std::atoi(argv[1]);
    if (sock_fd < 0) {
        return 1;
    }
    electron_mpv::HelperApp app(sock_fd);
    return app.run();
}

#else
int main() { return 0; }
#endif
