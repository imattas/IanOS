#pragma once
#include <stdint.h>

namespace hk {

enum class LogLevel { Debug, Info, Warn, Error };

void serial_initialize();
void serial_write(const char* text);
void set_console_log_enabled(bool enabled);
bool console_log_enabled();
void log(LogLevel level, const char* text);
void log_hex(LogLevel level, const char* label, uint64_t value);
uint64_t kernel_log_size();
uint64_t copy_kernel_log(char* out, uint64_t capacity, uint64_t offset);
[[noreturn]] void panic(const char* reason, const char* file, int line);

} // namespace hk

#define HK_PANIC(reason) hk::panic((reason), __FILE__, __LINE__)
#define HK_ASSERT(expr) do { if (!(expr)) hk::panic("assertion failed: " #expr, __FILE__, __LINE__); } while (0)
