#include "hybrid/user.hpp"

namespace {

const char* class_name(hybrid::DeviceClass value) {
    switch (value) {
    case hybrid::DeviceClass::Storage: return "storage";
    case hybrid::DeviceClass::Network: return "network";
    case hybrid::DeviceClass::Display: return "display";
    case hybrid::DeviceClass::Unknown: break;
    }
    return "unknown";
}

const char* resource_name(hybrid::DeviceResourceType value) {
    switch (value) {
    case hybrid::DeviceResourceType::Mmio: return "mmio";
    case hybrid::DeviceResourceType::Io: return "io";
    case hybrid::DeviceResourceType::None: break;
    }
    return "none";
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

void append_bdf(char* line, uint64_t capacity, uint64_t& cursor, const hybrid::DeviceInfo& info) {
    append_decimal(line, capacity, cursor, info.bus);
    hybrid::user::append_char(line, capacity, cursor, ':');
    append_decimal(line, capacity, cursor, info.device);
    hybrid::user::append_char(line, capacity, cursor, '.');
    append_decimal(line, capacity, cursor, info.function);
}

void emit_device(uint64_t index, const hybrid::DeviceInfo& info) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lsdev] device ");
    hybrid::user::append_hex(line, sizeof(line), cursor, index);
    hybrid::user::append_text(line, sizeof(line), cursor, " class ");
    hybrid::user::append_text(line, sizeof(line), cursor, class_name(info.device_class));
    hybrid::user::append_text(line, sizeof(line), cursor, " bdf ");
    append_bdf(line, sizeof(line), cursor, info);
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

void emit_resource(uint64_t device_index, uint64_t resource_index, const hybrid::DeviceResourceInfo& resource) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lsdev] resource ");
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

bool get_first_by_class(hybrid::DeviceClass device_class, hybrid::DeviceInfo& out) {
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetDeviceInfoByClass,
                                        static_cast<uint64_t>(device_class),
                                        0,
                                        reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetDeviceCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lsdev] ", "count error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_line("[lsdev] INDEX CLASS BDF VENDOR DEVICE COMMAND RESOURCES");
    hybrid::user::write_hex_line("[lsdev] ", "devices ", count.value);

    uint64_t storage = 0;
    uint64_t network = 0;
    uint64_t display = 0;
    uint64_t resources = 0;

    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::DeviceInfo info{};
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetDeviceInfo,
                                            i,
                                            reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[lsdev] ", "info error ", result.error);
            hybrid::user::exit(2);
        }

        if (info.device_class == hybrid::DeviceClass::Storage) ++storage;
        if (info.device_class == hybrid::DeviceClass::Network) ++network;
        if (info.device_class == hybrid::DeviceClass::Display) ++display;
        emit_device(i, info);

        for (uint64_t r = 0; r < info.resource_count && r < 3; ++r) {
            if (info.resources[r].type == hybrid::DeviceResourceType::None) continue;
            ++resources;
            emit_resource(i, r, info.resources[r]);
        }
    }

    hybrid::user::write_hex_line("[lsdev] ", "storage ", storage);
    hybrid::user::write_hex_line("[lsdev] ", "network ", network);
    hybrid::user::write_hex_line("[lsdev] ", "display ", display);
    hybrid::user::write_hex_line("[lsdev] ", "resources ", resources);

    hybrid::DeviceInfo first_storage{};
    hybrid::DeviceInfo first_network{};
    hybrid::DeviceInfo first_display{};
    hybrid::user::write_text_line("[lsdev] ", "first storage ", get_first_by_class(hybrid::DeviceClass::Storage, first_storage) ? class_name(first_storage.device_class) : "missing");
    hybrid::user::write_text_line("[lsdev] ", "first network ", get_first_by_class(hybrid::DeviceClass::Network, first_network) ? class_name(first_network.device_class) : "missing");
    hybrid::user::write_text_line("[lsdev] ", "first display ", get_first_by_class(hybrid::DeviceClass::Display, first_display) ? class_name(first_display.device_class) : "missing");

    hybrid::user::exit(count.value >= 3 && storage != 0 && network != 0 && display != 0 && resources != 0 ? 0 : 3);
}
