// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <mutex>
#include <optional>
#include <tuple>
#include "common/param_package.h"
#include "common/settings.h"
#include "core/frontend/input.h"
#include "input_common/udp/client.h"
#include "input_common/udp/udp.h"

namespace InputCommon::CemuhookUDP {

    class UDPTouchDevice final : public Input::TouchDevice {
    public:
        explicit UDPTouchDevice(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}
        std::tuple<float, float, bool> GetStatus() const override {
            std::lock_guard guard(status->update_mutex);
            return status->touch_status;
        }

    private:
        std::shared_ptr<DeviceStatus> status;
    };

    class UDPMotionDevice final : public Input::MotionDevice {
    public:
        explicit UDPMotionDevice(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}
        std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
            std::lock_guard guard(status->update_mutex);
            return status->motion_status;
        }

    private:
        std::shared_ptr<DeviceStatus> status;
    };

    class UDPTouchFactory final : public Input::Factory<Input::TouchDevice> {
    public:
        explicit UDPTouchFactory(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

        std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage& params) override {
            {
                std::lock_guard guard(status->update_mutex);
                status->touch_calibration = DeviceStatus::CalibrationData{};
                // These default values work well for DS4 but probably not other touch inputs
                status->touch_calibration->min_x = params.Get("min_x", 100);
                status->touch_calibration->min_y = params.Get("min_y", 50);
                status->touch_calibration->max_x = params.Get("max_x", 1800);
                status->touch_calibration->max_y = params.Get("max_y", 850);
            }
            return std::make_unique<UDPTouchDevice>(status);
        }

    private:
        std::shared_ptr<DeviceStatus> status;
    };

    class UDPMotionFactory final : public Input::Factory<Input::MotionDevice> {
    public:
        explicit UDPMotionFactory(std::shared_ptr<DeviceStatus> status_) : status(std::move(status_)) {}

        std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage& params) override {
            return std::make_unique<UDPMotionDevice>(status);
        }

    private:
        std::shared_ptr<DeviceStatus> status;
    };

    State::State() {
        ReloadUDPClient();
    }

    State::~State() {
        Input::UnregisterFactory<Input::TouchDevice>("cemuhookudp");
        Input::UnregisterFactory<Input::MotionDevice>("cemuhookudp");
    }

    void State::ReloadUDPClient() {
        // 1. Clean up existing factories to release references to old status objects
        Input::UnregisterFactory<Input::TouchDevice>("cemuhookudp");
        Input::UnregisterFactory<Input::MotionDevice>("cemuhookudp");

        // 2. Fetch Settings
        const auto& profile = Settings::values.current_input_profile;

        // 3. Initialize Motion Client (Primary)
        // We create a new status and client for motion every reload to ensure clean state
        auto status_motion = std::make_shared<DeviceStatus>();
        client_motion = std::make_unique<Client>(
            status_motion,
            profile.udp_input_address,
            profile.udp_input_port,
            profile.udp_pad_index
        );

        // 4. Initialize Touch Client (Secondary or Shared)
        std::shared_ptr<DeviceStatus> status_touch;

        if (profile.udp_touch_use_separate) {
            // Create a distinct client/status for touch
            status_touch = std::make_shared<DeviceStatus>();
            client_touch = std::make_unique<Client>(
                status_touch,
                profile.udp_touch_address,
                profile.udp_touch_port,
                profile.udp_touch_pad_index
            );
        } else {
            // Share the motion client/status
            status_touch = status_motion;
            client_touch = nullptr; // Ensure unused client is cleared
        }

        // 5. Re-register factories with the specific status objects
        Input::RegisterFactory<Input::TouchDevice>("cemuhookudp",
                                                   std::make_shared<UDPTouchFactory>(status_touch));
        Input::RegisterFactory<Input::MotionDevice>("cemuhookudp",
                                                    std::make_shared<UDPMotionFactory>(status_motion));
    }

    std::unique_ptr<State> Init() {
        return std::make_unique<State>();
    }

} // namespace InputCommon::CemuhookUDP
