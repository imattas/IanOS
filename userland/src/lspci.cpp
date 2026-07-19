#include "hybrid/user.hpp"

namespace {

const char* class_name(hybrid::DeviceClass device_class) {
    switch (device_class) {
    case hybrid::DeviceClass::Storage: return "storage";
    case hybrid::DeviceClass::Network: return "network";
    case hybrid::DeviceClass::Display: return "display";
    case hybrid::DeviceClass::Unknown: break;
    }
    return "unknown";
}

const char* resource_name(hybrid::DeviceResourceType type) {
    switch (type) {
    case hybrid::DeviceResourceType::Mmio: return "mmio";
    case hybrid::DeviceResourceType::Io: return "io";
    case hybrid::DeviceResourceType::None: break;
    }
    return "none";
}

void write_device_row(uint64_t index, const hybrid::DeviceInfo& info) {
    uint64_t bdf = (static_cast<uint64_t>(info.bus) << 16) |
        (static_cast<uint64_t>(info.device) << 8) |
        static_cast<uint64_t>(info.function);

    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lspci] device ");
    hybrid::user::append_hex(line, sizeof(line), cursor, index);
    hybrid::user::append_text(line, sizeof(line), cursor, " bdf ");
    hybrid::user::append_hex(line, sizeof(line), cursor, bdf);
    hybrid::user::append_text(line, sizeof(line), cursor, " class ");
    hybrid::user::append_text(line, sizeof(line), cursor, class_name(info.device_class));
    hybrid::user::append_text(line, sizeof(line), cursor, " vendor ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.vendor_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " device ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.device_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " command ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.required_command_bits);
    hybrid::user::append_text(line, sizeof(line), cursor, " resources ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.resource_count);
    hybrid::user::write_line(line);
}

void write_resource_row(uint64_t device_index, uint64_t resource_index, const hybrid::DeviceResourceInfo& resource) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lspci] resource ");
    hybrid::user::append_hex(line, sizeof(line), cursor, device_index);
    hybrid::user::append_char(line, sizeof(line), cursor, ':');
    hybrid::user::append_hex(line, sizeof(line), cursor, resource_index);
    hybrid::user::append_char(line, sizeof(line), cursor, ' ');
    hybrid::user::append_text(line, sizeof(line), cursor, resource_name(resource.type));
    hybrid::user::append_text(line, sizeof(line), cursor, " base ");
    hybrid::user::append_hex(line, sizeof(line), cursor, resource.base);
    hybrid::user::append_text(line, sizeof(line), cursor, " size ");
    hybrid::user::append_hex(line, sizeof(line), cursor, resource.size);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetDeviceCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lspci] ", "count error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[lspci] ", "devices ", count.value);
    uint64_t resources = 0;
    uint64_t storage = 0;
    uint64_t network = 0;
    uint64_t display = 0;

    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::DeviceInfo info{};
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetDeviceInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[lspci] ", "info error ", result.error);
            hybrid::user::exit(2);
        }

        if (info.device_class == hybrid::DeviceClass::Storage) ++storage;
        if (info.device_class == hybrid::DeviceClass::Network) ++network;
        if (info.device_class == hybrid::DeviceClass::Display) ++display;
        write_device_row(i, info);

        for (uint64_t r = 0; r < info.resource_count && r < 3; ++r) {
            if (info.resources[r].type == hybrid::DeviceResourceType::None) continue;
            ++resources;
            write_resource_row(i, r, info.resources[r]);
        }
    }

    hybrid::user::write_hex_line("[lspci] ", "storage ", storage);
    hybrid::user::write_hex_line("[lspci] ", "network ", network);
    hybrid::user::write_hex_line("[lspci] ", "display ", display);
    hybrid::user::write_hex_line("[lspci] ", "resources ", resources);
    hybrid::user::exit(count.value != 0 && resources != 0 ? 0 : 3);
}
