#pragma once
#include <stdint.h>
namespace hk::cpu {
struct CpuFeatures {
    bool apic;
    bool x2apic;
    bool syscall;
    bool nx;
};
CpuFeatures detect_features();
}
