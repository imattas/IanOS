#pragma once
#include <stddef.h>
extern "C" void* memcpy(void* dst, const void* src, size_t n);
extern "C" void* memset(void* dst, int c, size_t n);
extern "C" void* memmove(void* dst, const void* src, size_t n);
