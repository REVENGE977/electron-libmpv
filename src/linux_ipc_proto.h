#pragma once

#ifdef __linux__

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace electron_mpv {
namespace linux_ipc {

enum MsgType : uint32_t {
    // Parent -> Child
    CMD_INIT = 1,
    CMD_RESIZE = 2,
    CMD_RELEASE_FRAME = 3,
    CMD_SW_FALLBACK = 4,
    CMD_COMMAND = 5,
    CMD_SET_PROP = 6,
    CMD_GET_PROP = 7,
    CMD_OBSERVE_PROP = 8,
    CMD_UNOBSERVE_ALL = 9,
    CMD_SHUTDOWN = 10,

    // Child -> Parent
    EVT_INIT_RESULT = 101,
    EVT_SURFACE_POOL = 102,
    EVT_GPU_FRAME = 103,
    EVT_SW_FRAME = 104,
    EVT_MPV_EVENT = 105,
    EVT_GET_PROP_RESULT = 106,
    EVT_BACKEND_UPDATE = 107,
};

enum PropValType : uint8_t {
    VAL_NULL = 0,
    VAL_BOOL = 1,
    VAL_INT64 = 2,
    VAL_DOUBLE = 3,
    VAL_STRING = 4,
    VAL_ARRAY = 5,
    VAL_MAP = 6,
};

enum MpvEvtKind : uint8_t {
    MEVT_START_FILE = 1,
    MEVT_FILE_LOADED = 2,
    MEVT_PLAYBACK_RESTART = 3,
    MEVT_END_FILE = 4,
    MEVT_PROPERTY_CHANGE = 5,
    MEVT_LOG_MESSAGE = 6,
};

struct FrameHeader {
    uint32_t type = 0;
    uint32_t length = 0;
};

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v) { append(&v, sizeof(v)); }
    void i32(int32_t v) { append(&v, sizeof(v)); }
    void u64(uint64_t v) { append(&v, sizeof(v)); }
    void i64(int64_t v) { append(&v, sizeof(v)); }
    void f64(double v) { append(&v, sizeof(v)); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) append(s.data(), s.size());
    }
    void bytes(const void* data, size_t n) {
        u32(static_cast<uint32_t>(n));
        if (n > 0) append(data, n);
    }

    const std::vector<uint8_t>& data() const { return buf_; }

private:
    void append(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : data_(data), len_(len), pos_(0) {}

    bool u8(uint8_t& out) { return read(&out, sizeof(out)); }
    bool u32(uint32_t& out) { return read(&out, sizeof(out)); }
    bool i32(int32_t& out) { return read(&out, sizeof(out)); }
    bool u64(uint64_t& out) { return read(&out, sizeof(out)); }
    bool i64(int64_t& out) { return read(&out, sizeof(out)); }
    bool f64(double& out) { return read(&out, sizeof(out)); }
    bool str(std::string& out) {
        uint32_t n = 0;
        if (!u32(n) || pos_ + n > len_) return false;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return true;
    }
    bool bytesView(const uint8_t*& ptr, size_t& n) {
        uint32_t sz = 0;
        if (!u32(sz) || pos_ + sz > len_) return false;
        ptr = data_ + pos_;
        n = sz;
        pos_ += sz;
        return true;
    }

private:
    bool read(void* dst, size_t n) {
        if (pos_ + n > len_) return false;
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    const uint8_t* data_;
    size_t len_;
    size_t pos_;
};

inline bool SendMessage(int sock, uint32_t type, const std::vector<uint8_t>& payload, const std::vector<int>& fds = {}) {
    FrameHeader hdr{ type, static_cast<uint32_t>(payload.size()) };

    // Send header (with optional SCM_RIGHTS FDs attached to the header bytes).
    size_t hdr_sent = 0;
    bool sent_fds = fds.empty();
    while (hdr_sent < sizeof(hdr)) {
        struct msghdr msg = {};
        struct iovec iov = {
            reinterpret_cast<uint8_t*>(&hdr) + hdr_sent,
            sizeof(hdr) - hdr_sent
        };
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        std::vector<char> cmsg_buf;
        if (!sent_fds) {
            size_t fd_bytes = fds.size() * sizeof(int);
            cmsg_buf.resize(CMSG_SPACE(fd_bytes));
            msg.msg_control = cmsg_buf.data();
            msg.msg_controllen = cmsg_buf.size();
            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(fd_bytes);
            std::memcpy(CMSG_DATA(cmsg), fds.data(), fd_bytes);
        }

        ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        hdr_sent += static_cast<size_t>(n);
        sent_fds = true;
    }

    // Send payload bytes.
    size_t sent = 0;
    while (sent < payload.size()) {
        ssize_t n = send(sock, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline bool RecvMessage(int sock, uint32_t& out_type, std::vector<uint8_t>& out_payload, std::vector<int>& out_fds) {
    out_fds.clear();
    FrameHeader hdr = {};
    size_t hdr_got = 0;

    while (hdr_got < sizeof(hdr)) {
        struct msghdr msg = {};
        struct iovec iov = {
            reinterpret_cast<uint8_t*>(&hdr) + hdr_got,
            sizeof(hdr) - hdr_got
        };
        char cmsg_buf[CMSG_SPACE(sizeof(int) * 16)];
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;

        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
                size_t count = data_len / sizeof(int);
                const int* received = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
                for (size_t i = 0; i < count; ++i) {
                    out_fds.push_back(received[i]);
                }
            }
        }
        hdr_got += static_cast<size_t>(n);
    }

    if (hdr.length > 128U * 1024U * 1024U) {
        for (int fd : out_fds) close(fd);
        out_fds.clear();
        return false;
    }

    out_type = hdr.type;
    out_payload.resize(hdr.length);
    size_t got = 0;
    while (got < hdr.length) {
        ssize_t n = recv(sock, out_payload.data() + got, hdr.length - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            for (int fd : out_fds) close(fd);
            out_fds.clear();
            return false;
        }
        if (n == 0) {
            for (int fd : out_fds) close(fd);
            out_fds.clear();
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace linux_ipc
}  // namespace electron_mpv

#endif  // __linux__
