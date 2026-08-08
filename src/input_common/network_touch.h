// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include "core/frontend/input.h"

namespace InputCommon {

/**
 * A touch device factory that receives touch commands from a remote stream
 * receiver over UDP and forwards them to the 3DS touch screen.
 *
 * The receiver sends a 10-byte packet per touch event:
 *   [0]     magic byte 0x54 ('T')
 *   [1]     flags (bit 0 = pressed)
 *   [2..5]  float x, normalized 0.0 (left) .. 1.0 (right)
 *   [6..9]  float y, normalized 0.0 (top) .. 1.0 (bottom)
 *
 * Coordinates are relative to the visible game screen content area, so the
 * receiver must account for any letterboxing in the streamed video frame.
 * The device polls the socket from the HID thread (non-blocking) and always
 * reports the most recently received state.
 */
class NetworkTouchFactory final : public Input::Factory<Input::TouchDevice> {
public:
    std::unique_ptr<Input::TouchDevice> Create(const Common::ParamPackage& params) override;
};

} // namespace InputCommon
