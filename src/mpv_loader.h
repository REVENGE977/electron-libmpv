#pragma once

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/render_gl.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace electron_mpv {

struct MpvApi {
    mpv_handle* (*create)(void) = nullptr;
    int (*initialize)(mpv_handle*) = nullptr;
    void (*destroy)(mpv_handle*) = nullptr;
    void (*terminate_destroy)(mpv_handle*) = nullptr;
    int (*set_option)(mpv_handle*, const char*, mpv_format, void*) = nullptr;
    int (*set_option_string)(mpv_handle*, const char*, const char*) = nullptr;
    int (*command)(mpv_handle*, const char**) = nullptr;
    int (*command_async)(mpv_handle*, uint64_t, const char**) = nullptr;
    int (*set_property)(mpv_handle*, const char*, mpv_format, void*) = nullptr;
    int (*set_property_string)(mpv_handle*, const char*, const char*) = nullptr;
    int (*set_property_async)(mpv_handle*, uint64_t, const char*, mpv_format, void*) = nullptr;
    int (*get_property)(mpv_handle*, const char*, mpv_format, void*) = nullptr;
    char* (*get_property_string)(mpv_handle*, const char*) = nullptr;
    int (*observe_property)(mpv_handle*, uint64_t, const char*, mpv_format) = nullptr;
    int (*unobserve_property)(mpv_handle*, uint64_t) = nullptr;
    mpv_event* (*wait_event)(mpv_handle*, double) = nullptr;
    void (*set_wakeup_callback)(mpv_handle*, void (*cb)(void*), void*) = nullptr;
    int (*request_log_messages)(mpv_handle*, const char*) = nullptr;
    void (*free)(void*) = nullptr;
    void (*free_node_contents)(mpv_node*) = nullptr;
    const char* (*error_string)(int) = nullptr;

    int (*render_context_create)(mpv_render_context**, mpv_handle*, mpv_render_param*) = nullptr;
    int (*render_context_set_parameter)(mpv_render_context*, mpv_render_param) = nullptr;
    int (*render_context_get_info)(mpv_render_context*, mpv_render_param) = nullptr;
    void (*render_context_set_update_callback)(mpv_render_context*, mpv_render_update_fn, void*) = nullptr;
    uint64_t (*render_context_update)(mpv_render_context*) = nullptr;
    int (*render_context_render)(mpv_render_context*, mpv_render_param*) = nullptr;
    void (*render_context_report_swap)(mpv_render_context*) = nullptr;
    void (*render_context_free)(mpv_render_context*) = nullptr;

    bool loaded = false;
    std::string loaded_path;
    std::string last_error;

#ifdef _WIN32
    HMODULE lib_handle = nullptr;
#else
    void* lib_handle = nullptr;
#endif

    template <typename T>
    bool loadSym(T& fn, const char* name) {
#ifdef _WIN32
        fn = reinterpret_cast<T>(GetProcAddress(lib_handle, name));
#else
        fn = reinterpret_cast<T>(dlsym(lib_handle, name));
#endif
        if (!fn) {
            last_error = std::string("Missing libmpv symbol: ") + name;
            return false;
        }
        return true;
    }

    bool load(const std::string& custom_path = "") {
        if (loaded) return true;

        std::vector<std::string> candidates;
        if (!custom_path.empty()) {
            candidates.push_back(custom_path);
        }

        const char* env_path = std::getenv("ELECTRON_LIBMPV_PATH");
        if (env_path && env_path[0] != '\0') {
            candidates.push_back(env_path);
        }

#ifdef _WIN32
        char exe_buf[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            std::string exe_dir(exe_buf);
            size_t slash = exe_dir.find_last_of("\\/");
            if (slash != std::string::npos) {
                exe_dir = exe_dir.substr(0, slash + 1);
                candidates.push_back(exe_dir + "libmpv-2.dll");
                candidates.push_back(exe_dir + "mpv-2.dll");
                candidates.push_back(exe_dir + "resources\\libmpv-2.dll");
            }
        }
        candidates.push_back("libmpv-2.dll");
        candidates.push_back("mpv-2.dll");
        candidates.push_back("mpv-1.dll");
#elif defined(__APPLE__)
        candidates.push_back("@rpath/libmpv.2.dylib");
        candidates.push_back("@loader_path/libmpv.2.dylib");
        candidates.push_back("@executable_path/../Frameworks/libmpv.2.dylib");
        candidates.push_back("@executable_path/../Resources/libmpv.2.dylib");
        candidates.push_back("/opt/homebrew/lib/libmpv.2.dylib");
        candidates.push_back("/opt/homebrew/lib/libmpv.dylib");
        candidates.push_back("/usr/local/lib/libmpv.2.dylib");
        candidates.push_back("/usr/local/lib/libmpv.dylib");
        candidates.push_back("libmpv.2.dylib");
        candidates.push_back("libmpv.dylib");
#else
        candidates.push_back("libmpv.so.2");
        candidates.push_back("libmpv.so.1");
        candidates.push_back("libmpv.so");
        candidates.push_back("/usr/lib64/libmpv.so.2");
        candidates.push_back("/usr/lib/x86_64-linux-gnu/libmpv.so.2");
        candidates.push_back("/usr/lib/aarch64-linux-gnu/libmpv.so.2");
        candidates.push_back("/usr/lib/libmpv.so.2");
        candidates.push_back("/usr/local/lib/libmpv.so.2");
#endif

        for (const auto& candidate : candidates) {
#ifdef _WIN32
            lib_handle = LoadLibraryA(candidate.c_str());
#else
            lib_handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
            if (lib_handle) {
                loaded_path = candidate;
                break;
            }
        }

        if (!lib_handle) {
#ifdef _WIN32
            last_error = "Unable to load libmpv-2.dll. Ensure libmpv-2.dll is bundled or on PATH.";
#elif defined(__APPLE__)
            const char* err = dlerror();
            last_error = std::string("Unable to load libmpv.2.dylib (install via `brew install mpv` or bundle libmpv): ") + (err ? err : "unknown error");
#else
            const char* err = dlerror();
            last_error = std::string("Unable to load libmpv.so.2 (install mpv / libmpv2): ") + (err ? err : "unknown error");
#endif
            return false;
        }

        bool ok =
            loadSym(create, "mpv_create") &&
            loadSym(initialize, "mpv_initialize") &&
            loadSym(destroy, "mpv_destroy") &&
            loadSym(terminate_destroy, "mpv_terminate_destroy") &&
            loadSym(set_option, "mpv_set_option") &&
            loadSym(set_option_string, "mpv_set_option_string") &&
            loadSym(command, "mpv_command") &&
            loadSym(command_async, "mpv_command_async") &&
            loadSym(set_property, "mpv_set_property") &&
            loadSym(set_property_string, "mpv_set_property_string") &&
            loadSym(set_property_async, "mpv_set_property_async") &&
            loadSym(get_property, "mpv_get_property") &&
            loadSym(get_property_string, "mpv_get_property_string") &&
            loadSym(observe_property, "mpv_observe_property") &&
            loadSym(unobserve_property, "mpv_unobserve_property") &&
            loadSym(wait_event, "mpv_wait_event") &&
            loadSym(set_wakeup_callback, "mpv_set_wakeup_callback") &&
            loadSym(request_log_messages, "mpv_request_log_messages") &&
            loadSym(free, "mpv_free") &&
            loadSym(free_node_contents, "mpv_free_node_contents") &&
            loadSym(error_string, "mpv_error_string") &&
            loadSym(render_context_create, "mpv_render_context_create") &&
            loadSym(render_context_set_parameter, "mpv_render_context_set_parameter") &&
            loadSym(render_context_get_info, "mpv_render_context_get_info") &&
            loadSym(render_context_set_update_callback, "mpv_render_context_set_update_callback") &&
            loadSym(render_context_update, "mpv_render_context_update") &&
            loadSym(render_context_render, "mpv_render_context_render") &&
            loadSym(render_context_report_swap, "mpv_render_context_report_swap") &&
            loadSym(render_context_free, "mpv_render_context_free");

        if (!ok) {
#ifdef _WIN32
            FreeLibrary(lib_handle);
#else
            dlclose(lib_handle);
#endif
            lib_handle = nullptr;
            loaded_path.clear();
            return false;
        }

        loaded = true;
        return true;
    }
};

}  // namespace electron_mpv
