#include "hk/console.hpp"
#include "hk/log.hpp"

namespace {

constexpr uint8_t font[96][8] = {
    {0,0,0,0,0,0,0,0},{24,24,24,24,24,0,24,0},{54,54,36,0,0,0,0,0},{54,54,127,54,127,54,54,0},
    {24,62,96,60,6,124,24,0},{98,102,12,24,48,102,70,0},{56,108,56,118,220,204,118,0},{24,24,48,0,0,0,0,0},
    {12,24,48,48,48,24,12,0},{48,24,12,12,12,24,48,0},{0,102,60,255,60,102,0,0},{0,24,24,126,24,24,0,0},
    {0,0,0,0,0,24,24,48},{0,0,0,126,0,0,0,0},{0,0,0,0,0,24,24,0},{2,6,12,24,48,96,64,0},
    {60,102,110,118,102,102,60,0},{24,56,24,24,24,24,126,0},{60,102,6,12,48,96,126,0},{60,102,6,28,6,102,60,0},
    {12,28,60,108,126,12,12,0},{126,96,124,6,6,102,60,0},{28,48,96,124,102,102,60,0},{126,6,12,24,48,48,48,0},
    {60,102,102,60,102,102,60,0},{60,102,102,62,6,12,56,0},{0,24,24,0,0,24,24,0},{0,24,24,0,0,24,24,48},
    {12,24,48,96,48,24,12,0},{0,0,126,0,126,0,0,0},{48,24,12,6,12,24,48,0},{60,102,6,12,24,0,24,0},
    {60,102,110,110,110,96,62,0},{24,60,102,102,126,102,102,0},{124,102,102,124,102,102,124,0},{60,102,96,96,96,102,60,0},
    {120,108,102,102,102,108,120,0},{126,96,96,124,96,96,126,0},{126,96,96,124,96,96,96,0},{60,102,96,110,102,102,60,0},
    {102,102,102,126,102,102,102,0},{126,24,24,24,24,24,126,0},{30,12,12,12,12,108,56,0},{102,108,120,112,120,108,102,0},
    {96,96,96,96,96,96,126,0},{99,119,127,107,99,99,99,0},{102,118,126,126,110,102,102,0},{60,102,102,102,102,102,60,0},
    {124,102,102,124,96,96,96,0},{60,102,102,102,110,60,14,0},{124,102,102,124,120,108,102,0},{60,102,96,60,6,102,60,0},
    {126,24,24,24,24,24,24,0},{102,102,102,102,102,102,60,0},{102,102,102,102,102,60,24,0},{99,99,99,107,127,119,99,0},
    {102,102,60,24,60,102,102,0},{102,102,102,60,24,24,24,0},{126,6,12,24,48,96,126,0},{60,48,48,48,48,48,60,0},
    {64,96,48,24,12,6,2,0},{60,12,12,12,12,12,60,0},{24,60,102,0,0,0,0,0},{0,0,0,0,0,0,0,255},
    {48,24,12,0,0,0,0,0},{0,0,60,6,62,102,62,0},{96,96,124,102,102,102,124,0},{0,0,60,102,96,102,60,0},
    {6,6,62,102,102,102,62,0},{0,0,60,102,126,96,60,0},{28,54,48,120,48,48,48,0},{0,0,62,102,102,62,6,124},
    {96,96,124,102,102,102,102,0},{24,0,56,24,24,24,60,0},{6,0,6,6,6,102,102,60},{96,96,102,108,120,108,102,0},
    {56,24,24,24,24,24,60,0},{0,0,102,127,107,99,99,0},{0,0,124,102,102,102,102,0},{0,0,60,102,102,102,60,0},
    {0,0,124,102,102,124,96,96},{0,0,62,102,102,62,6,6},{0,0,108,118,96,96,96,0},{0,0,62,96,60,6,124,0},
    {48,48,120,48,48,54,28,0},{0,0,102,102,102,102,62,0},{0,0,102,102,102,60,24,0},{0,0,99,99,107,127,54,0},
    {0,0,102,60,24,60,102,0},{0,0,102,102,102,62,6,124},{0,0,126,12,24,48,126,0},{14,24,24,112,24,24,14,0},
    {24,24,24,0,24,24,24,0},{112,24,24,14,24,24,112,0},{118,220,0,0,0,0,0,0},{0,16,56,108,198,198,254,0}
};

} // namespace

namespace hk {

Console& console() {
    static Console c;
    return c;
}

void Console::initialize(const hybrid::FramebufferInfo& fb) {
    fb_ = fb;
    clear();
}

void Console::clear() {
    ++stats_.clear_calls;
    for (uint32_t row = 0; row < kScrollbackRows; ++row) {
        for (uint32_t col = 0; col < kMaxColumns; ++col) cells_[row][col] = ' ';
    }
    cursor_x_ = 0;
    cursor_row_ = 0;
    viewport_bottom_ = 0;
    follow_tail_ = true;
    clear_pixels();
}

uint32_t Console::columns() const {
    uint32_t cols = fb_.width / 8;
    return cols > kMaxColumns ? kMaxColumns : cols;
}

uint32_t Console::rows() const {
    return fb_.height / 16;
}

void Console::newline() {
    ++stats_.newline_count;
    cursor_x_ = 0;
    ++cursor_row_;
    clear_history_row(cursor_row_);
    if (follow_tail_) {
        viewport_bottom_ = cursor_row_;
        if (cursor_row_ >= rows()) render();
    }
}

void Console::backspace() {
    ++stats_.backspace_count;
    uint32_t cols = columns();
    if (cols == 0) return;
    if (cursor_x_ == 0 && cursor_row_ == 0) return;
    if (cursor_x_ == 0) {
        --cursor_row_;
        cursor_x_ = cols - 1;
    } else {
        --cursor_x_;
    }
    if (follow_tail_) {
        uint32_t visible_rows = rows();
        uint64_t top = viewport_bottom_ >= visible_rows - 1 ? viewport_bottom_ - (visible_rows - 1) : 0;
        clear_cell(cursor_x_, static_cast<uint32_t>(cursor_row_ - top));
    } else {
        cells_[cursor_row_ % kScrollbackRows][cursor_x_] = ' ';
        render();
    }
}

void Console::clear_cell(uint32_t cell_x, uint32_t cell_y) {
    uint32_t cols = columns();
    uint32_t visible_rows = rows();
    if (cols == 0 || visible_rows == 0) return;
    uint64_t top = viewport_bottom_ >= visible_rows - 1 ? viewport_bottom_ - (visible_rows - 1) : 0;
    uint64_t absolute_row = top + cell_y;
    if (cell_x < cols && absolute_row <= cursor_row_ && absolute_row >= oldest_row()) {
        cells_[absolute_row % kScrollbackRows][cell_x] = ' ';
    }
    clear_pixel_cell(cell_x, cell_y);
}

void Console::clear_row(uint32_t cell_y) {
    uint32_t cols = columns();
    for (uint32_t x = 0; x < cols; ++x) clear_cell(x, cell_y);
}

void Console::clear_pixel_cell(uint32_t cell_x, uint32_t cell_y) {
    auto* pixels = reinterpret_cast<uint32_t*>(fb_.base);
    uint32_t start_x = cell_x * 8;
    uint32_t start_y = cell_y * 16;
    for (uint32_t y = 0; y < 16; ++y) {
        uint32_t py = start_y + y;
        if (py >= fb_.height) break;
        for (uint32_t x = 0; x < 8; ++x) {
            uint32_t px = start_x + x;
            if (px >= fb_.width) break;
            pixels[py * fb_.pixels_per_scanline + px] = bg_;
        }
    }
}

void Console::clear_pixel_row(uint32_t cell_y) {
    uint32_t cols = columns();
    for (uint32_t x = 0; x < cols; ++x) clear_pixel_cell(x, cell_y);
}

void Console::clear_pixels() {
    auto* pixels = reinterpret_cast<uint32_t*>(fb_.base);
    for (uint32_t y = 0; y < fb_.height; ++y) {
        for (uint32_t x = 0; x < fb_.width; ++x) {
            pixels[y * fb_.pixels_per_scanline + x] = bg_;
        }
    }
}

void Console::clear_history_row(uint64_t absolute_row) {
    char* row = cells_[absolute_row % kScrollbackRows];
    for (uint32_t col = 0; col < kMaxColumns; ++col) row[col] = ' ';
}

void Console::update_view_to_tail() {
    viewport_bottom_ = cursor_row_;
    follow_tail_ = true;
    render();
}

uint64_t Console::oldest_row() const {
    return cursor_row_ >= kScrollbackRows ? cursor_row_ - kScrollbackRows + 1 : 0;
}

void Console::render() {
    ++stats_.render_calls;
    uint32_t visible_rows = rows();
    uint32_t cols = columns();
    if (visible_rows == 0 || cols == 0 || fb_.base == 0) return;
    clear_pixels();
    uint64_t top = viewport_bottom_ >= visible_rows - 1 ? viewport_bottom_ - (visible_rows - 1) : 0;
    uint64_t oldest = oldest_row();
    for (uint32_t screen_row = 0; screen_row < visible_rows; ++screen_row) {
        uint64_t absolute_row = top + screen_row;
        if (absolute_row < oldest || absolute_row > cursor_row_) continue;
        const char* row = cells_[absolute_row % kScrollbackRows];
        for (uint32_t col = 0; col < cols; ++col) {
            char c = row[col];
            if (c != 0 && c != ' ') draw_char(c, col * 8, screen_row * 16, fg_);
        }
    }
}

void Console::scroll_view(int64_t amount) {
    ++stats_.scroll_relative_ops;
    uint32_t visible_rows = rows();
    if (visible_rows == 0) return;
    uint64_t oldest = oldest_row();
    uint64_t minimum_bottom = oldest + (visible_rows > 0 ? visible_rows - 1 : 0);
    if (minimum_bottom > cursor_row_) minimum_bottom = cursor_row_;
    if (amount < 0) {
        uint64_t delta = static_cast<uint64_t>(-amount);
        viewport_bottom_ = viewport_bottom_ <= minimum_bottom || delta > viewport_bottom_ - minimum_bottom
            ? minimum_bottom
            : viewport_bottom_ - delta;
    } else if (amount > 0) {
        uint64_t delta = static_cast<uint64_t>(amount);
        viewport_bottom_ = viewport_bottom_ >= cursor_row_ || cursor_row_ - viewport_bottom_ < delta
            ? cursor_row_
            : viewport_bottom_ + delta;
    }
    follow_tail_ = viewport_bottom_ == cursor_row_;
    render();
}

void Console::scroll_to_bottom() {
    ++stats_.scroll_to_bottom_ops;
    update_view_to_tail();
}

void Console::reset_input_line() {
    ++stats_.input_line_resets;
    scroll_to_bottom();
    uint32_t visible_rows = rows();
    if (visible_rows == 0) return;
    uint64_t top = viewport_bottom_ >= visible_rows - 1 ? viewport_bottom_ - (visible_rows - 1) : 0;
    uint32_t screen_row = static_cast<uint32_t>(cursor_row_ - top);
    clear_history_row(cursor_row_);
    cursor_x_ = 0;
    if (screen_row < visible_rows) clear_pixel_row(screen_row);
}

void Console::put(char c) {
    ++stats_.put_calls;
    if (c == '\n') { newline(); return; }
    if (c == '\r') return;
    if (c == '\f') { clear(); return; }
    if (c == '\b') { backspace(); return; }
    uint32_t cols = columns();
    if (cols == 0) return;
    cells_[cursor_row_ % kScrollbackRows][cursor_x_] = c;
    ++stats_.cells_written;
    if (follow_tail_) {
        uint32_t visible_rows = rows();
        if (visible_rows != 0) {
            uint64_t top = viewport_bottom_ >= visible_rows - 1 ? viewport_bottom_ - (visible_rows - 1) : 0;
            if (cursor_row_ >= top && cursor_row_ < top + visible_rows) {
                draw_char(c, cursor_x_ * 8, static_cast<uint32_t>(cursor_row_ - top) * 16, fg_);
            }
        }
    }
    ++cursor_x_;
    if (cursor_x_ >= cols) newline();
}

void Console::write(const char* text) {
    ++stats_.write_calls;
    while (*text) put(*text++);
}

void Console::write_hex(uint64_t value) {
    const char* digits = "0123456789abcdef";
    write("0x");
    for (int i = 60; i >= 0; i -= 4) put(digits[(value >> i) & 0xf]);
}

void Console::draw_char(char c, uint32_t x, uint32_t y, uint32_t color) {
    ++stats_.glyph_draws;
    if (c < 32 || c > 127) c = '?';
    auto* pixels = reinterpret_cast<uint32_t*>(fb_.base);
    const auto* glyph = font[static_cast<unsigned>(c) - 32];
    for (uint32_t row = 0; row < 8; ++row) {
        for (uint32_t col = 0; col < 8; ++col) {
            uint32_t out = (glyph[row] & (1u << (7 - col))) ? color : bg_;
            uint32_t py0 = y + row * 2;
            uint32_t py1 = py0 + 1;
            uint32_t px = x + col;
            if (px < fb_.width && py0 < fb_.height) pixels[py0 * fb_.pixels_per_scanline + px] = out;
            if (px < fb_.width && py1 < fb_.height) pixels[py1 * fb_.pixels_per_scanline + px] = out;
        }
    }
}

ConsoleStats Console::stats() const {
    ConsoleStats snapshot = stats_;
    snapshot.cursor_row = cursor_row_;
    snapshot.cursor_column = cursor_x_;
    snapshot.viewport_bottom = viewport_bottom_;
    snapshot.oldest_row = oldest_row();
    snapshot.visible_columns = columns();
    snapshot.visible_rows = rows();
    snapshot.follow_tail = follow_tail_;
    return snapshot;
}

bool Console::self_test() {
    ConsoleStats before = stats();
    if (fb_.base == 0 || before.visible_columns == 0 || before.visible_rows == 0) return false;
    if (!hk::console_log_enabled()) {
        hk::log(hk::LogLevel::Info, "Console visual self-test skipped in quiet boot");
        return true;
    }

    write("[console] visual backend self-test\n");
    write("[console] scrollback row\n");
    put('X');
    put('\b');
    scroll_view(-1);
    ConsoleStats scrolled = stats();
    if (scrolled.follow_tail && scrolled.cursor_row >= scrolled.visible_rows) return false;
    scroll_to_bottom();
    reset_input_line();
    write("[console] reset line ok");
    ConsoleStats after = stats();

    bool ok = after.write_calls >= before.write_calls + 2 &&
        after.put_calls > before.put_calls &&
        after.newline_count >= before.newline_count + 2 &&
        after.backspace_count > before.backspace_count &&
        after.cells_written > before.cells_written &&
        after.glyph_draws > before.glyph_draws &&
        after.render_calls > before.render_calls &&
        after.scroll_relative_ops > before.scroll_relative_ops &&
        after.scroll_to_bottom_ops > before.scroll_to_bottom_ops &&
        after.input_line_resets > before.input_line_resets &&
        after.viewport_bottom == after.cursor_row &&
        after.follow_tail;
    if (!ok) return false;

    hk::log_hex(hk::LogLevel::Info, "Console visible columns", after.visible_columns);
    hk::log_hex(hk::LogLevel::Info, "Console visible rows", after.visible_rows);
    hk::log_hex(hk::LogLevel::Info, "Console cursor row", after.cursor_row);
    hk::log_hex(hk::LogLevel::Info, "Console render calls", after.render_calls);
    hk::log_hex(hk::LogLevel::Info, "Console glyph draws", after.glyph_draws);
    hk::log_hex(hk::LogLevel::Info, "Console scroll ops", after.scroll_relative_ops);
    hk::log_hex(hk::LogLevel::Info, "Console input line resets", after.input_line_resets);
    hk::log(hk::LogLevel::Info, "Console viewport self-test");
    return true;
}

} // namespace hk
