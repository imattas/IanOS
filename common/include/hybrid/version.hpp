#pragma once

namespace hybrid::version {

#define HYBRID_OS_NAME "IanOS"
#define HYBRID_OS_ID "ianos"
#define HYBRID_OS_VERSION "0.1.16"
#define HYBRID_MACHINE "x86_64"
#define HYBRID_OS_RELEASE "0.1.16-x86_64"
#define HYBRID_KERNEL_NAME "Mattas"
#define HYBRID_KERNEL_VERSION "0.1.16-stable"
#define HYBRID_KERNEL_FLAVOR "stable"
#define HYBRID_KERNEL_TYPE "hybrid"
#define HYBRID_KERNEL_DISPLAY "Mattas 0.1.16-stable"
#define HYBRID_PROC_VERSION "Mattas 0.1.16-stable x86_64 uefi hybrid"

constexpr const char* kOsName = HYBRID_OS_NAME;
constexpr const char* kOsId = HYBRID_OS_ID;
constexpr const char* kOsVersion = HYBRID_OS_VERSION;
constexpr const char* kMachine = HYBRID_MACHINE;
constexpr const char* kOsRelease = HYBRID_OS_RELEASE;
constexpr const char* kKernelName = HYBRID_KERNEL_NAME;
constexpr const char* kKernelVersion = HYBRID_KERNEL_VERSION;
constexpr const char* kKernelFlavor = HYBRID_KERNEL_FLAVOR;
constexpr const char* kKernelType = HYBRID_KERNEL_TYPE;
constexpr const char* kKernelDisplay = HYBRID_KERNEL_DISPLAY;
constexpr const char* kProcVersion = HYBRID_PROC_VERSION;

} // namespace hybrid::version
