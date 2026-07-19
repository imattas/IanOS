#include "hk/cpu/features.hpp"

namespace {
void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(subleaf));
}
}

namespace hk::cpu {
CpuFeatures detect_features() {
    uint32_t a, b, c, d;
    cpuid(1, 0, a, b, c, d);
    CpuFeatures f{};
    f.apic = d & (1u << 9);
    f.x2apic = c & (1u << 21);
    cpuid(0x80000001, 0, a, b, c, d);
    f.syscall = d & (1u << 11);
    f.nx = d & (1u << 20);
    return f;
}
}
