#pragma once

#include <stdint.h>
#include "hybrid/boot_info.hpp"

namespace hk {

struct ConsoleStats {
    uint64_t clear_calls;
    uint64_t write_calls;
    uint64_t put_calls;
    uint64_t newline_count;
    uint64_t backspace_count;
    uint64_t render_calls;
    uint64_t glyph_draws;
    uint64_t cells_written;
    uint64_t scroll_relative_ops;
    uint64_t scroll_to_bottom_ops;
    uint64_t input_line_resets;
    uint64_t cursor_row;
    uint64_t cursor_column;
    uint64_t viewport_bottom;
    uint64_t oldest_row;
    uint32_t visible_columns;
    uint32_t visible_rows;
    bool follow_tail;
};

class Console {
public:
    void initialize(const hybrid::FramebufferInfo& fb);
    void clear();
    void write(const char* text);
    void write_hex(uint64_t value);
    void put(char c);
    void scroll_view(int64_t rows);
    void scroll_to_bottom();
    void reset_input_line();
    ConsoleStats stats() const;
    bool self_test();

private:
    static constexpr uint32_t kMaxColumns = 160;
    static constexpr uint32_t kScrollbackRows = 512;

    void newline();
    void backspace();
    void clear_cell(uint32_t cell_x, uint32_t cell_y);
    void clear_row(uint32_t cell_y);
    void clear_pixel_cell(uint32_t cell_x, uint32_t cell_y);
    void clear_pixel_row(uint32_t cell_y);
    void clear_pixels();
    void clear_history_row(uint64_t absolute_row);
    void update_view_to_tail();
    void render();
    void draw_char(char c, uint32_t x, uint32_t y, uint32_t color);
    uint32_t columns() const;
    uint32_t rows() const;
    uint64_t oldest_row() const;

    hybrid::FramebufferInfo fb_{};
    uint32_t cursor_x_ = 0;
    uint64_t cursor_row_ = 0;
    uint64_t viewport_bottom_ = 0;
    bool follow_tail_ = true;
    ConsoleStats stats_{};
    uint32_t fg_ = 0x00d6dee7;
    uint32_t bg_ = 0x00101820;
    char cells_[kScrollbackRows][kMaxColumns]{};
};

Console& console();

} // namespace hk
