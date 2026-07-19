#pragma once

#include <stddef.h>
#include <stdint.h>

using EFI_STATUS = uint64_t;
using EFI_HANDLE = void*;
using EFI_EVENT = void*;
using CHAR16 = wchar_t;
using UINTN = uint64_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using INT32 = int32_t;
using BOOLEAN = uint8_t;

constexpr EFI_STATUS EFI_SUCCESS = 0;
constexpr EFI_STATUS EFI_BUFFER_TOO_SMALL = 5;
constexpr EFI_STATUS EFI_NOT_READY = 6;
constexpr EFI_STATUS EFI_NOT_FOUND = 14;
constexpr EFI_STATUS EFI_LOAD_ERROR = 1;
constexpr EFI_STATUS EFI_INVALID_PARAMETER = 2;
constexpr uint64_t EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL = 0x00000001;
constexpr uint32_t EFI_LOADED_IMAGE_PROTOCOL_REVISION = 0x1000;
constexpr uint32_t EFI_FILE_MODE_READ = 0x0000000000000001ULL;
constexpr uint32_t EFI_FILE_READ_ONLY = 0x0000000000000001ULL;
constexpr uint32_t EFI_LOCATE_BY_PROTOCOL = 2;

#define EFI_ERROR(s) ((s) != EFI_SUCCESS)

struct EFI_GUID {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct EFI_TABLE_HEADER {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);
    EFI_STATUS (*output_string)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, const CHAR16*);
    EFI_STATUS (*test_string)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, const CHAR16*);
    EFI_STATUS (*query_mode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN, UINTN*, UINTN*);
    EFI_STATUS (*set_mode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
    EFI_STATUS (*set_attribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN);
    EFI_STATUS (*clear_screen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*);
    EFI_STATUS (*set_cursor_position)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, UINTN, UINTN);
    EFI_STATUS (*enable_cursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL*, BOOLEAN);
    void* mode;
};

struct EFI_INPUT_KEY { uint16_t scan_code; CHAR16 unicode_char; };
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, BOOLEAN);
    EFI_STATUS (*read_key_stroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*, EFI_INPUT_KEY*);
    EFI_EVENT wait_for_key;
};

enum EFI_ALLOCATE_TYPE : uint32_t { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };
enum EFI_MEMORY_TYPE : uint32_t {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
};

struct EFI_MEMORY_DESCRIPTOR {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

struct EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER hdr;
    void* raise_tpl; void* restore_tpl;
    EFI_STATUS (*allocate_pages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, uint64_t*);
    EFI_STATUS (*free_pages)(uint64_t, UINTN);
    EFI_STATUS (*get_memory_map)(UINTN*, EFI_MEMORY_DESCRIPTOR*, UINTN*, UINTN*, UINT32*);
    EFI_STATUS (*allocate_pool)(EFI_MEMORY_TYPE, UINTN, void**);
    EFI_STATUS (*free_pool)(void*);
    void* create_event; void* set_timer; void* wait_for_event; void* signal_event; void* close_event; void* check_event;
    void* install_protocol_interface; void* reinstall_protocol_interface; void* uninstall_protocol_interface;
    EFI_STATUS (*handle_protocol)(EFI_HANDLE, EFI_GUID*, void**);
    void* reserved; void* register_protocol_notify; void* locate_handle; void* locate_device_path;
    void* install_configuration_table;
    EFI_STATUS (*load_image)(BOOLEAN, EFI_HANDLE, void*, void*, UINTN, EFI_HANDLE*);
    EFI_STATUS (*start_image)(EFI_HANDLE, UINTN*, CHAR16**);
    void* exit; void* unload_image;
    EFI_STATUS (*exit_boot_services)(EFI_HANDLE, UINTN);
    void* get_next_monotonic_count; EFI_STATUS (*stall)(UINTN);
    EFI_STATUS (*set_watchdog_timer)(UINTN, UINT64, UINTN, CHAR16*);
    EFI_STATUS (*connect_controller)(EFI_HANDLE, EFI_HANDLE*, void*, BOOLEAN);
    void* disconnect_controller;
    EFI_STATUS (*open_protocol)(EFI_HANDLE, EFI_GUID*, void**, EFI_HANDLE, EFI_HANDLE, UINT32);
    void* close_protocol;
    void* open_protocol_information;
    void* protocols_per_handle;
    void* locate_handle_buffer;
    EFI_STATUS (*locate_protocol)(EFI_GUID*, void*, void**);
};

struct EFI_CONFIGURATION_TABLE { EFI_GUID vendor_guid; void* vendor_table; };

struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER hdr;
    CHAR16* firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL* con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* con_out;
    EFI_HANDLE standard_error_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* std_err;
    void* runtime_services;
    EFI_BOOT_SERVICES* boot_services;
    UINTN number_of_table_entries;
    EFI_CONFIGURATION_TABLE* configuration_table;
};

struct EFI_LOADED_IMAGE_PROTOCOL {
    uint32_t revision;
    EFI_HANDLE parent_handle;
    EFI_SYSTEM_TABLE* system_table;
    EFI_HANDLE device_handle;
    void* file_path;
    void* reserved;
    uint32_t load_options_size;
    void* load_options;
    void* image_base;
    uint64_t image_size;
    EFI_MEMORY_TYPE image_code_type;
    EFI_MEMORY_TYPE image_data_type;
    EFI_STATUS (*unload)(EFI_HANDLE);
};

struct EFI_FILE_PROTOCOL {
    uint64_t revision;
    EFI_STATUS (*open)(EFI_FILE_PROTOCOL*, EFI_FILE_PROTOCOL**, CHAR16*, uint64_t, uint64_t);
    EFI_STATUS (*close)(EFI_FILE_PROTOCOL*);
    EFI_STATUS (*delete_file)(EFI_FILE_PROTOCOL*);
    EFI_STATUS (*read)(EFI_FILE_PROTOCOL*, UINTN*, void*);
    EFI_STATUS (*write)(EFI_FILE_PROTOCOL*, UINTN*, void*);
    EFI_STATUS (*get_position)(EFI_FILE_PROTOCOL*, uint64_t*);
    EFI_STATUS (*set_position)(EFI_FILE_PROTOCOL*, uint64_t);
    EFI_STATUS (*get_info)(EFI_FILE_PROTOCOL*, EFI_GUID*, UINTN*, void*);
};

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t revision;
    EFI_STATUS (*open_volume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*, EFI_FILE_PROTOCOL**);
};

struct EFI_GRAPHICS_OUTPUT_MODE_INFORMATION {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    uint32_t pixel_information[4];
    uint32_t pixels_per_scan_line;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE {
    uint32_t max_mode;
    uint32_t mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
    UINTN size_of_info;
    uint64_t frame_buffer_base;
    UINTN frame_buffer_size;
};

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*query_mode)(EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t, UINTN*, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION**);
    EFI_STATUS (*set_mode)(EFI_GRAPHICS_OUTPUT_PROTOCOL*, uint32_t);
    void* blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* mode;
};

inline bool guid_eq(const EFI_GUID& a, const EFI_GUID& b) {
    if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) return false;
    for (int i = 0; i < 8; ++i) if (a.data4[i] != b.data4[i]) return false;
    return true;
}
