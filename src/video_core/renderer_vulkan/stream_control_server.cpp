// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_vulkan/stream_control_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <json.hpp>
#include "common/logging/log.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/hle/service/hid/hid.h"

namespace Vulkan {

namespace {
constexpr u16 kControlPort = 5003;
constexpr u32 kProtocolVersion = 1;
constexpr u16 kStreamPorts[2] = {5000, 5001};
constexpr int kMaxLine = 4096;

Settings::StreamingScreen ParseScreen(const std::string& screen) {
    return screen == "top" ? Settings::StreamingScreen::Top
                           : Settings::StreamingScreen::Bottom;
}

std::string ScreenToString(Settings::StreamingScreen screen) {
    return screen == Settings::StreamingScreen::Top ? "top" : "bottom";
}
} // Anonymous namespace

StreamControlServer::StreamControlServer(Core::System& system_) : system(system_) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR(Render_Vulkan, "StreamControlServer: socket() failed: {}", strerror(errno));
        return;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kControlPort);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_WARNING(Render_Vulkan, "StreamControlServer: bind() failed: {}", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }
    if (listen(listen_fd_, 4) < 0) {
        LOG_WARNING(Render_Vulkan, "StreamControlServer: listen() failed: {}", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    stop_ = false;
    thread_ = std::thread(&StreamControlServer::ListenLoop, this);
    LOG_INFO(Render_Vulkan, "StreamControlServer: listening for stream receivers on port {}",
             kControlPort);
}

StreamControlServer::~StreamControlServer() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

void StreamControlServer::SendError(int fd, const std::string& message) {
    nlohmann::json resp{{"version", kProtocolVersion}, {"ok", false}, {"error", message}};
    const std::string out = resp.dump() + "\n";
    send(fd, out.data(), out.size(), MSG_NOSIGNAL);
}

void StreamControlServer::ListenLoop() {
    while (!stop_) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        const int pr = poll(&pfd, 1, 200);
        if (pr <= 0) {
            continue;
        }
        sockaddr_in client{};
        socklen_t client_len = sizeof(client);
        const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &client_len);
        if (fd < 0) {
            continue;
        }
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &client.sin_addr, ip_str, sizeof(ip_str));
        HandleClient(fd, ip_str);
    }
}

void StreamControlServer::HandleClient(int fd, const std::string& client_ip) {
    // Read one newline-terminated JSON line.
    std::string line;
    char buf[256];
    bool got_line = false;
    while (!got_line) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            close(fd);
            return;
        }
        line.append(buf, static_cast<size_t>(n));
        const size_t nl = line.find('\n');
        if (nl != std::string::npos) {
            line.resize(nl);
            got_line = true;
        }
        if (line.size() > kMaxLine) {
            close(fd);
            return;
        }
    }

    nlohmann::json req;
    try {
        req = nlohmann::json::parse(line);
    } catch (...) {
        SendError(fd, "invalid JSON request");
        close(fd);
        return;
    }
    if (!req.contains("version") || req["version"] != kProtocolVersion) {
        SendError(fd, "unsupported protocol version");
        close(fd);
        return;
    }

    u32 slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        slot = 2;
        for (u32 i = 0; i < sessions_.size(); ++i) {
            if (!sessions_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == 2) {
            SendError(fd, "no free stream slots");
            close(fd);
            return;
        }

        const std::string screen_str = req.value("screen", std::string("bottom"));
        if (screen_str != "top" && screen_str != "bottom") {
            SendError(fd, "screen must be \"top\" or \"bottom\"");
            close(fd);
            return;
        }
        const std::string codec = req.value("codec", std::string("h264"));
        if (codec != "h264" && codec != "auto") {
            SendError(fd, "unsupported codec (only h264 is available)");
            close(fd);
            return;
        }
        const bool custom = req.contains("width") || req.contains("height");
        const u32 width = req.value("width", 0);
        const u32 height = req.value("height", 0);
        if (custom &&
            (width < 64 || width > 3840 || height < 64 || height > 2160 || (width % 2) ||
             (height % 2))) {
            SendError(fd, "invalid resolution (must be even, 64..3840 x 64..2160)");
            close(fd);
            return;
        }
        const u32 bitrate = req.value("bitrate", 0);
        if (bitrate != 0 && (bitrate < 100 || bitrate > 100000)) {
            SendError(fd, "invalid bitrate (must be 100..100000 kbps)");
            close(fd);
            return;
        }

        auto& sess = sessions_[slot];
        auto& v = Settings::values;

        if (slot == 0) {
            sess.prev_enabled = v.streaming_enabled.GetValue();
            sess.prev_screen = v.streaming_screen.GetValue();
            sess.prev_custom_res = v.streaming_custom_resolution.GetValue();
            sess.prev_width = v.streaming_width.GetValue();
            sess.prev_height = v.streaming_height.GetValue();
            sess.prev_ip = v.streaming_target_ip.GetValue();
            sess.prev_port = v.streaming_target_port.GetValue();
            sess.prev_bitrate = v.streaming_bitrate.GetValue();
            sess.prev_qp = v.streaming_qp.GetValue();

            v.streaming_enabled = true;
            v.streaming_screen = ParseScreen(screen_str);
            if (custom) {
                v.streaming_custom_resolution = true;
                v.streaming_width = width;
                v.streaming_height = height;
            }
            v.streaming_target_ip = client_ip;
            v.streaming_target_port = kStreamPorts[slot];
            if (bitrate != 0) {
                v.streaming_bitrate = bitrate;
            }
        } else {
            sess.prev_enabled = v.streaming_enabled_2.GetValue();
            sess.prev_screen = v.streaming_screen_2.GetValue();
            sess.prev_custom_res = v.streaming_custom_resolution_2.GetValue();
            sess.prev_width = v.streaming_width_2.GetValue();
            sess.prev_height = v.streaming_height_2.GetValue();
            sess.prev_ip = v.streaming_target_ip_2.GetValue();
            sess.prev_port = v.streaming_target_port_2.GetValue();
            sess.prev_bitrate = v.streaming_bitrate_2.GetValue();
            sess.prev_qp = v.streaming_qp_2.GetValue();

            v.streaming_enabled_2 = true;
            v.streaming_screen_2 = ParseScreen(screen_str);
            if (custom) {
                v.streaming_custom_resolution_2 = true;
                v.streaming_width_2 = width;
                v.streaming_height_2 = height;
            }
            v.streaming_target_ip_2 = client_ip;
            v.streaming_target_port_2 = kStreamPorts[slot];
            if (bitrate != 0) {
                v.streaming_bitrate_2 = bitrate;
            }
        }

        // Make sure the network touch listener is active so the receiver can
        // send touch input back.
        v.streaming_input_enabled = true;
        sess.active = true;

        // Compute the effective stream resolution the renderer will use.
        const Settings::StreamingScreen eff_screen = ParseScreen(screen_str);
        u32 eff_w = 0;
        u32 eff_h = 0;
        if (custom) {
            eff_w = width;
            eff_h = height;
        } else {
            u32 scale = Settings::values.resolution_factor.GetValue();
            if (scale == 0) {
                scale = 1;
            }
            eff_w = (eff_screen == Settings::StreamingScreen::Top)
                        ? static_cast<u32>(Core::kScreenTopWidth) * scale
                        : static_cast<u32>(Core::kScreenBottomWidth) * scale;
            eff_h = static_cast<u32>(Core::kScreenTopHeight) * scale;
        }

        nlohmann::json resp{
            {"version", kProtocolVersion},
            {"ok", true},
            {"slot", slot},
            {"video_port", kStreamPorts[slot]},
            {"touch_port", v.streaming_input_port.GetValue()},
            {"screen", ScreenToString(eff_screen)},
            {"width", eff_w},
            {"height", eff_h},
            {"codec", "h264"},
            {"pt", 96},
        };
        const std::string out = resp.dump() + "\n";
        send(fd, out.data(), out.size(), MSG_NOSIGNAL);
        LOG_INFO(Render_Vulkan,
                 "StreamControlServer: receiver {} requested {} {}x{} codec={} bitrate={} -> "
                 "slot {}, streaming to port {}",
                 client_ip, screen_str, width, height, codec, bitrate, slot, kStreamPorts[slot]);
    }

    // Reload input devices on the emulation thread so the touch listener
    // (re)binding takes effect immediately.
    if (auto hid = Service::HID::GetModule(system)) {
        hid->ReloadInputDevices();
    }

    // Keep the connection open until the receiver disconnects, then restore
    // the previous settings.
    char b[128];
    while (true) {
        const ssize_t n = recv(fd, b, sizeof(b), 0);
        if (n <= 0) {
            break;
        }
    }
    close(fd);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& sess = sessions_[slot];
        auto& v = Settings::values;
        if (slot == 0) {
            v.streaming_enabled = sess.prev_enabled;
            v.streaming_screen = sess.prev_screen;
            v.streaming_custom_resolution = sess.prev_custom_res;
            v.streaming_width = sess.prev_width;
            v.streaming_height = sess.prev_height;
            v.streaming_target_ip = sess.prev_ip;
            v.streaming_target_port = sess.prev_port;
            v.streaming_bitrate = sess.prev_bitrate;
            v.streaming_qp = sess.prev_qp;
        } else {
            v.streaming_enabled_2 = sess.prev_enabled;
            v.streaming_screen_2 = sess.prev_screen;
            v.streaming_custom_resolution_2 = sess.prev_custom_res;
            v.streaming_width_2 = sess.prev_width;
            v.streaming_height_2 = sess.prev_height;
            v.streaming_target_ip_2 = sess.prev_ip;
            v.streaming_target_port_2 = sess.prev_port;
            v.streaming_bitrate_2 = sess.prev_bitrate;
            v.streaming_qp_2 = sess.prev_qp;
        }
        sess.active = false;
        LOG_INFO(Render_Vulkan, "StreamControlServer: receiver {} disconnected, slot {} released",
                 client_ip, slot);
    }
}

} // namespace Vulkan
