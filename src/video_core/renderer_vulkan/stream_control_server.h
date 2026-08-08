// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "common/common_types.h"
#include "common/settings.h"

namespace Core {
class System;
}

namespace Vulkan {

/**
 * StreamControlServer: a small TCP control channel that lets a stream receiver
 * negotiate how azahar should stream, instead of having to configure the
 * emulator's streaming settings first.
 *
 * The server listens on TCP port 5003. A receiver connects, sends one JSON
 * line, and keeps the connection open for the duration of the session:
 *
 *   Request:  {"version": 1, "screen": "bottom", "width": 640, "height": 360,
 *              "codec": "h264", "bitrate": 4000}
 *             (width/height/bitrate optional; "top"/"bottom" screen)
 *
 *   Response: {"version": 1, "ok": true, "slot": 0, "video_port": 5000,
 *              "touch_port": 5002, "screen": "bottom", "width": 640,
 *              "height": 360, "codec": "h264", "pt": 96}
 *             (on error: {"version": 1, "ok": false, "error": "..."})
 *
 * The negotiated stream replaces the configured one for that slot until the
 * receiver closes the connection, at which point the previous settings are
 * restored. Width/height must be even and within [64, 3840]x[64, 2160].
 * Only H.264 is supported (the emulator's encoders are all H.264); the
 * framerate is the emulator's native one.
 */
class StreamControlServer {
public:
    explicit StreamControlServer(Core::System& system);
    ~StreamControlServer();

    StreamControlServer(const StreamControlServer&) = delete;
    StreamControlServer& operator=(const StreamControlServer&) = delete;

private:
    void ListenLoop();
    void HandleClient(int fd, const std::string& client_ip);
    void SendError(int fd, const std::string& message);

    Core::System& system;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int listen_fd_ = -1;
    std::mutex mutex_;

    struct Session {
        bool active = false;
        // Saved previous settings, restored when the session ends.
        bool prev_enabled = false;
        Settings::StreamingScreen prev_screen = Settings::StreamingScreen::Bottom;
        bool prev_custom_res = false;
        u32 prev_width = 0;
        u32 prev_height = 0;
        std::string prev_ip;
        u16 prev_port = 0;
        u32 prev_bitrate = 0;
        u32 prev_qp = 22;
    };
    std::array<Session, 2> sessions_;
};

} // namespace Vulkan
