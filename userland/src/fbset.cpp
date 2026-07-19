#include "hybrid/user.hpp"

namespace {

const char* format_name(uint32_t format) {
    switch (format) {
    case 0: return "rgb";
    case 1: return "bgr";
    case 2: return "bitmask";
    default: return "unknown";
    }
}

int main_result() {
    hybrid::FramebufferInfo fb;
    auto* bytes = reinterpret_cast<unsigned char*>(&fb);
    for (uint64_t i = 0; i < sizeof(fb); ++i) bytes[i] = 0;

    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetFramebufferInfo, reinterpret_cast<uint64_t>(&fb));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[fbset] ", "framebuffer error ", result.error);
        return 1;
    }

    hybrid::user::write_hex_line("[fbset] ", "base ", fb.base);
    hybrid::user::write_hex_line("[fbset] ", "width ", fb.width);
    hybrid::user::write_hex_line("[fbset] ", "height ", fb.height);
    hybrid::user::write_hex_line("[fbset] ", "scanline ", fb.pixels_per_scanline);
    hybrid::user::write_hex_line("[fbset] ", "bytes per pixel ", fb.bytes_per_pixel);
    hybrid::user::write_hex_line("[fbset] ", "bits per pixel ", fb.bytes_per_pixel * 8u);
    hybrid::user::write_text_line("[fbset] ", "format ", format_name(fb.format));
    hybrid::user::write_hex_line("[fbset] ", "format raw ", fb.format);
    hybrid::user::write_hex_line("[fbset] ", "red mask ", fb.red_mask);
    hybrid::user::write_hex_line("[fbset] ", "green mask ", fb.green_mask);
    hybrid::user::write_hex_line("[fbset] ", "blue mask ", fb.blue_mask);
    hybrid::user::write_hex_line("[fbset] ", "reserved mask ", fb.reserved_mask);
    return fb.base != 0 && fb.width != 0 && fb.height != 0 ? 0 : 2;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::user::exit(static_cast<uint64_t>(main_result()));
}
