#include "hk/drivers/ps2_keyboard.hpp"
#include "hk/interrupts/irq.hpp"
#include "hk/log.hpp"
#include "hk/terminal.hpp"

namespace {

constexpr uint16_t kDataPort = 0x60;
constexpr uint16_t kStatusPort = 0x64;
constexpr uint8_t kStatusOutputFull = 1u << 0;
bool shift_down = false;
bool ctrl_down = false;
bool extended_prefix = false;

uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

char normal_key(uint8_t scancode) {
    static const char table[128] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
        '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
        0,'a','s','d','f','g','h','j','k','l',';','\'','`',
        0,'\\','z','x','c','v','b','n','m',',','.','/',
        0,'*',0,' '
    };
    return scancode < sizeof(table) ? table[scancode] : 0;
}

char shifted_key(uint8_t scancode) {
    static const char table[128] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
        '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
        0,'A','S','D','F','G','H','J','K','L',':','"','~',
        0,'|','Z','X','C','V','B','N','M','<','>','?',
        0,'*',0,' '
    };
    return scancode < sizeof(table) ? table[scancode] : 0;
}

char translate(uint8_t scancode) {
    if (scancode == 0xe0) {
        extended_prefix = true;
        return 0;
    }
    if (extended_prefix) {
        extended_prefix = false;
        if (scancode & 0x80) return 0;
        if (scancode == 0x48) return 0x10; // Up arrow
        if (scancode == 0x50) return 0x0e; // Down arrow
        if (scancode == 0x47) return 0x01; // Home
        if (scancode == 0x4b) return 0x02; // Left arrow
        if (scancode == 0x4d) return 0x06; // Right arrow
        if (scancode == 0x4f) return 0x05; // End
        if (scancode == 0x49) return 0x11; // Page Up
        if (scancode == 0x51) return 0x12; // Page Down
        if (scancode == 0x53) return 0x7f; // Delete
        return 0;
    }
    if (scancode == 0x2a || scancode == 0x36) {
        shift_down = true;
        return 0;
    }
    if (scancode == 0xaa || scancode == 0xb6) {
        shift_down = false;
        return 0;
    }
    if (scancode == 0x1d) {
        ctrl_down = true;
        return 0;
    }
    if (scancode == 0x9d) {
        ctrl_down = false;
        return 0;
    }
    if (scancode & 0x80) return 0;
    char c = shift_down ? shifted_key(scancode) : normal_key(scancode);
    if (ctrl_down && c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 1);
    if (ctrl_down && c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 1);
    return c;
}

}

namespace hk::drivers::ps2_keyboard {

void initialize() {
    while (inb(kStatusPort) & kStatusOutputFull) {
        (void)inb(kDataPort);
    }
    hk::interrupts::set_irq_mask(1, false);
    hk::log(hk::LogLevel::Info, "PS/2 keyboard initialized");
}

void handle_irq() {
    if ((inb(kStatusPort) & kStatusOutputFull) == 0) return;
    uint8_t scancode = inb(kDataPort);
    hk::terminal::push_input(translate(scancode));
}

bool pop_char(char& out) {
    return hk::terminal::read_key(&out, 1) == 1;
}

uint64_t buffered_count() {
    return hk::terminal::buffered_input_count();
}

}
