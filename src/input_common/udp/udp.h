// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include "input_common/udp/client.h"

namespace InputCommon::CemuhookUDP {

class State {
public:
    State();
    ~State();
    void ReloadUDPClient();

private:
    // Change this from a single client to two
    std::unique_ptr<Client> client_motion;
    std::unique_ptr<Client> client_touch;
};

std::unique_ptr<State> Init();

} // namespace InputCommon::CemuhookUDP
