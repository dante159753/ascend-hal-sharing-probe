#pragma once

#include <cstdint>
#include "status/status.h"

namespace UC::Trans {

class Device {
public:
    Status Init();
    Status Setup(int32_t deviceId);
    Status Reset(int32_t deviceId);
    Status Finalize();
};

}  // namespace UC::Trans
