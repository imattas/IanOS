#pragma once
#include "hybrid/syscall.hpp"

namespace hk::syscall {

using Number = hybrid::SyscallNumber;
using Result = hybrid::SyscallResult;

constexpr uint64_t kErrorNone = hybrid::kSyscallErrorNone;
constexpr uint64_t kErrorInvalidSyscall = hybrid::kSyscallErrorInvalidSyscall;
constexpr uint64_t kErrorInvalidPointer = hybrid::kSyscallErrorInvalidPointer;
constexpr uint64_t kErrorNotFound = hybrid::kSyscallErrorNotFound;
constexpr uint64_t kErrorWouldBlock = hybrid::kSyscallErrorWouldBlock;

Result dispatch(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3);
bool self_test();

} // namespace hk::syscall
