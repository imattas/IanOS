#pragma once

#include "hybrid/syscall.hpp"

namespace hk::time {

bool read_rtc_datetime(hybrid::DateTimeInfo& out);

} // namespace hk::time
