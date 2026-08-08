// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "input_common/network_touch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

#include "common/logging/log.h"
#include "common/param_package.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace InputCommon {

namespace {

constexpr u8 NETWORK_TOUCH_MAGIC = 0x54; // 'T'
constexpr u16 NETWORK_TOUCH_DEFAULT_PORT = 5002;
constexpr std::size_t NETWORK_TOUCH_PACKET_SIZE = 10;

#ifdef _WIN32
using SocketHolder = unsigned long long;
constexpr SocketHolder INVALID_SOCKET_HOLDER = static_cast<SocketHolder>(~0ull);
#define CLOSE_SOCKET(s) closesocket(s)
#else
using SocketHolder = int;
constexpr SocketHolder INVALID_SOCKET_HOLDER = -1;
#define CLOSE_SOCKET(s) ::close(s)
#endif

/**
 * Touch device backed by a UDP socket. Stream receivers send touch packets to
 * the configured port; the HID thread polls this device every frame and the
 * latest received state is forwarded to the 3DS touch screen.
 */
class NetworkTouchDevice final : public Input::TouchDevice {
public:
    explicit NetworkTouchDevice(u16 port) {
#ifdef _WIN32
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            LOG_ERROR(Input, "NetworkTouch: WSAStartup failed");
            return;
        }
#endif
        socket_fd = static_cast<SocketHolder>(::socket(AF_INET, SOCK_DGRAM, 0));
        if (socket_fd == INVALID_SOCKET_HOLDER) {
            LOG_ERROR(Input, "NetworkTouch: failed to create UDP socket");
            return;
        }

        int reuse = 1;
        ::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(socket_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            LOG_ERROR(Input, "NetworkTouch: failed to bind UDP port {}", port);
            CLOSE_SOCKET(socket_fd);
            socket_fd = INVALID_SOCKET_HOLDER;
            return;
        }

#ifdef _WIN32
        u_long nonblocking = 1;
        ioctlsocket(socket_fd, FIONBIO, &nonblocking);
#else
        const int flags = ::fcntl(socket_fd, F_GETFL, 0);
        ::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
#endif
        LOG_INFO(Input, "NetworkTouch: listening for touch input on UDP port {}", port);
    }

    ~NetworkTouchDevice() override {
        if (socket_fd != INVALID_SOCKET_HOLDER) {
            CLOSE_SOCKET(socket_fd);
        }
    }

    std::tuple<float, float, bool> GetStatus() const override {
        std::lock_guard lock{mutex};
        std::array<u8, NETWORK_TOUCH_PACKET_SIZE> buffer{};
        while (socket_fd != INVALID_SOCKET_HOLDER) {
            sockaddr_in sender{};
            socklen_t sender_len = sizeof(sender);
#ifdef _WIN32
            const int len = ::recvfrom(socket_fd, reinterpret_cast<char*>(buffer.data()),
                                       static_cast<int>(buffer.size()), 0,
                                       reinterpret_cast<sockaddr*>(&sender), &sender_len);
#else
            const ssize_t len = ::recvfrom(socket_fd, buffer.data(), buffer.size(), 0,
                                           reinterpret_cast<sockaddr*>(&sender), &sender_len);
#endif
            if (len < 0) {
                break; // No more packets pending (non-blocking)
            }
            if (len != static_cast<ssize_t>(buffer.size()) || buffer[0] != NETWORK_TOUCH_MAGIC) {
                continue;
            }
            float packet_x = 0.f;
            float packet_y = 0.f;
            std::memcpy(&packet_x, buffer.data() + 2, sizeof(float));
            std::memcpy(&packet_y, buffer.data() + 6, sizeof(float));
            const bool packet_pressed = (buffer[1] & 1) != 0;
            const float clamped_x = std::clamp(packet_x, 0.0f, 1.0f);
            const float clamped_y = std::clamp(packet_y, 0.0f, 1.0f);
            if (packet_pressed != pressed) {
                LOG_INFO(Input, "NetworkTouch: {} at ({:.3f}, {:.3f})",
                         packet_pressed ? "pressed" : "released", clamped_x, clamped_y);
            }
            x = clamped_x;
            y = clamped_y;
            pressed = packet_pressed;
        }
        return {x, y, pressed};
    }

private:
    mutable std::mutex mutex;
    SocketHolder socket_fd = INVALID_SOCKET_HOLDER;
    mutable float x = 0.5f;
    mutable float y = 0.5f;
    mutable bool pressed = false;
};

} // namespace

std::unique_ptr<Input::TouchDevice> NetworkTouchFactory::Create(
    const Common::ParamPackage& params) {
    const u16 port = static_cast<u16>(params.Get("port", NETWORK_TOUCH_DEFAULT_PORT));
    return std::make_unique<NetworkTouchDevice>(port);
}

} // namespace InputCommon
