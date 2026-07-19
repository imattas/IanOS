#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hybrid/syscall.hpp"

namespace hk::terminal {

struct Stats {
    uint64_t write_calls;
    uint64_t bytes_written;
    uint64_t raw_input_bytes;
    uint64_t canonical_input_bytes;
    uint64_t canonical_line_commits;
    uint64_t dropped_input_bytes;
    uint64_t input_high_watermark;
    uint64_t scroll_relative_ops;
    uint64_t scroll_to_bottom_ops;
    uint64_t input_line_resets;
};

size_t write(const char* data, size_t length);
void push_input(char c);
size_t inject_input(const char* data, size_t length);
size_t read_input(char* buffer, size_t length);
size_t read_key(char* buffer, size_t length);
uint64_t buffered_input_count();
void set_input_mode(hybrid::TerminalInputMode mode);
hybrid::TerminalInputMode input_mode();
void scroll_relative(int64_t rows);
void scroll_to_bottom();
void reset_input_line();
Stats stats();
bool self_test();

}
