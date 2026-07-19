#include "uefi.hpp"
#include "hybrid/boot_info.hpp"

namespace {

EFI_SYSTEM_TABLE* st = nullptr;
EFI_HANDLE image = nullptr;

constexpr EFI_GUID kLoadedImageProtocol = {0x5B1B31A1,0x9562,0x11d2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
constexpr EFI_GUID kSimpleFsProtocol = {0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
constexpr EFI_GUID kFileInfoGuid = {0x09576e92,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
constexpr EFI_GUID kGopProtocol = {0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
constexpr EFI_GUID kAcpi20Table = {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
constexpr EFI_GUID kAcpi10Table = {0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
constexpr uint16_t kScanUp = 0x0001;
constexpr uint16_t kScanDown = 0x0002;
constexpr UINTN kBootMenuOptions = 4;

struct BootModuleSpec {
    const CHAR16* firmware_path;
    const char* kernel_path;
};

constexpr BootModuleSpec kBootModuleSpecs[] = {
    {L"\\user\\init.elf", "/user/init.elf"},
    {L"\\bin\\hello.elf", "/bin/hello.elf"},
    {L"\\bin\\args.elf", "/bin/args.elf"},
    {L"\\bin\\cat.elf", "/bin/cat.elf"},
    {L"\\bin\\ls.elf", "/bin/ls.elf"},
    {L"\\bin\\uname.elf", "/bin/uname.elf"},
    {L"\\bin\\hostname.elf", "/bin/hostname.elf"},
    {L"\\bin\\free.elf", "/bin/free.elf"},
    {L"\\bin\\uptime.elf", "/bin/uptime.elf"},
    {L"\\bin\\date.elf", "/bin/date.elf"},
    {L"\\bin\\dmesg.elf", "/bin/dmesg.elf"},
    {L"\\bin\\ps.elf", "/bin/ps.elf"},
    {L"\\bin\\pwd.elf", "/bin/pwd.elf"},
    {L"\\bin\\env.elf", "/bin/env.elf"},
    {L"\\bin\\sysinfo.elf", "/bin/sysinfo.elf"},
    {L"\\bin\\fastfetch.elf", "/bin/fastfetch.elf"},
    {L"\\bin\\sysctl.elf", "/bin/sysctl.elf"},
    {L"\\bin\\id.elf", "/bin/id.elf"},
    {L"\\bin\\ids.elf", "/bin/ids.elf"},
    {L"\\bin\\ctx.elf", "/bin/ctx.elf"},
    {L"\\bin\\echo.elf", "/bin/echo.elf"},
    {L"\\bin\\sleep.elf", "/bin/sleep.elf"},
    {L"\\bin\\true.elf", "/bin/true.elf"},
    {L"\\bin\\false.elf", "/bin/false.elf"},
    {L"\\bin\\touch.elf", "/bin/touch.elf"},
    {L"\\bin\\append.elf", "/bin/append.elf"},
    {L"\\bin\\rm.elf", "/bin/rm.elf"},
    {L"\\bin\\cp.elf", "/bin/cp.elf"},
    {L"\\bin\\mv.elf", "/bin/mv.elf"},
    {L"\\bin\\wc.elf", "/bin/wc.elf"},
    {L"\\bin\\grep.elf", "/bin/grep.elf"},
    {L"\\bin\\tee.elf", "/bin/tee.elf"},
    {L"\\bin\\mkdir.elf", "/bin/mkdir.elf"},
    {L"\\bin\\rmdir.elf", "/bin/rmdir.elf"},
    {L"\\bin\\err.elf", "/bin/err.elf"},
    {L"\\bin\\stat.elf", "/bin/stat.elf"},
    {L"\\bin\\whoami.elf", "/bin/whoami.elf"},
    {L"\\bin\\basename.elf", "/bin/basename.elf"},
    {L"\\bin\\dirname.elf", "/bin/dirname.elf"},
    {L"\\bin\\head.elf", "/bin/head.elf"},
    {L"\\bin\\tail.elf", "/bin/tail.elf"},
    {L"\\bin\\test.elf", "/bin/test.elf"},
    {L"\\bin\\sort.elf", "/bin/sort.elf"},
    {L"\\bin\\uniq.elf", "/bin/uniq.elf"},
    {L"\\bin\\find.elf", "/bin/find.elf"},
    {L"\\bin\\sh.elf", "/bin/sh.elf"},
    {L"\\bin\\duptest.elf", "/bin/duptest.elf"},
    {L"\\bin\\fds.elf", "/bin/fds.elf"},
    {L"\\bin\\lsof.elf", "/bin/lsof.elf"},
    {L"\\bin\\fdinh.elf", "/bin/fdinh.elf"},
    {L"\\bin\\ln.elf", "/bin/ln.elf"},
    {L"\\bin\\readlink.elf", "/bin/readlink.elf"},
    {L"\\bin\\truncate.elf", "/bin/truncate.elf"},
    {L"\\bin\\blk.elf", "/bin/blk.elf"},
    {L"\\bin\\mount.elf", "/bin/mount.elf"},
    {L"\\bin\\df.elf", "/bin/df.elf"},
    {L"\\bin\\du.elf", "/bin/du.elf"},
    {L"\\bin\\lsblk.elf", "/bin/lsblk.elf"},
    {L"\\bin\\pipeinfo.elf", "/bin/pipeinfo.elf"},
    {L"\\bin\\kill.elf", "/bin/kill.elf"},
    {L"\\bin\\killall.elf", "/bin/killall.elf"},
    {L"\\bin\\pgrep.elf", "/bin/pgrep.elf"},
    {L"\\bin\\uyield.elf", "/bin/uyield.elf"},
    {L"\\bin\\ubusy.elf", "/bin/ubusy.elf"},
    {L"\\bin\\slowcat.elf", "/bin/slowcat.elf"},
    {L"\\bin\\burst.elf", "/bin/burst.elf"},
    {L"\\bin\\loop.elf", "/bin/loop.elf"},
    {L"\\bin\\devio.elf", "/bin/devio.elf"},
    {L"\\bin\\tty.elf", "/bin/tty.elf"},
    {L"\\bin\\stty.elf", "/bin/stty.elf"},
    {L"\\bin\\ttyread.elf", "/bin/ttyread.elf"},
    {L"\\bin\\clear.elf", "/bin/clear.elf"},
};

constexpr UINTN kBootModuleCount = sizeof(kBootModuleSpecs) / sizeof(kBootModuleSpecs[0]);
static_assert(kBootModuleCount <= hybrid::kMaxBootModules);

struct LoadedBootModule {
    void* file;
    UINTN size;
};

struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

void print(const CHAR16* s) { st->con_out->output_string(st->con_out, s); }

void print_dec(UINTN value) {
    CHAR16 digits[24];
    UINTN count = 0;
    do {
        digits[count++] = static_cast<CHAR16>(L'0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits) / sizeof(digits[0]));
    while (count != 0) {
        CHAR16 out[2] = {digits[--count], 0};
        print(out);
    }
}

void sleep_us(UINTN microseconds) {
    if (st && st->boot_services && st->boot_services->stall) st->boot_services->stall(microseconds);
}

void set_text_attribute(UINTN attribute) {
    if (st && st->con_out && st->con_out->set_attribute) st->con_out->set_attribute(st->con_out, attribute);
}

void clear_screen() {
    if (st && st->con_out && st->con_out->clear_screen) st->con_out->clear_screen(st->con_out);
}

void set_cursor(UINTN column, UINTN row) {
    if (st && st->con_out && st->con_out->set_cursor_position) st->con_out->set_cursor_position(st->con_out, column, row);
}

void boot_menu_line(UINTN index, UINTN selected, const CHAR16* label, const CHAR16* detail) {
    print(index == selected ? L"> " : L"  ");
    set_text_attribute(index == selected ? 0x70 : 0x0f);
    print(label);
    set_text_attribute(0x07);
    print(detail);
}

void draw_boot_menu(UINTN selected) {
    clear_screen();
    set_cursor(0, 0);
    set_text_attribute(0x0f);
    print(L"IanOS Boot Manager\r\n");
    set_text_attribute(0x07);
    print(L"\r\n");
    boot_menu_line(0, selected, L"Normal boot (UEFI)", L"       Interactive OS shell\r\n");
    boot_menu_line(1, selected, L"Recovery shell (UEFI)", L"    Diagnostics-first shell mode\r\n");
    boot_menu_line(2, selected, L"Debug boot (UEFI)", L"        Verbose framebuffer kernel logs\r\n");
    boot_menu_line(3, selected, L"BIOS/legacy boot", L"         Not installed in this image\r\n");
    print(L"\r\nUse Up/Down or 1-4, then Enter.\r\n");
}

bool read_menu_key(EFI_INPUT_KEY* key) {
    if (!st || !st->con_in || !st->con_in->read_key_stroke) return false;
    return st->con_in->read_key_stroke(st->con_in, key) == EFI_SUCCESS;
}

bool file_exists(const CHAR16* path);

uint32_t select_boot_flags() {
    uint32_t flags = 0;
    if (file_exists(L"\\boot\\runtests")) flags |= hybrid::kBootFlagRunBootScript;
    if (file_exists(L"\\boot\\recovery")) flags |= hybrid::kBootFlagRecovery;
    if ((flags & hybrid::kBootFlagRunBootScript) != 0) {
        print(L"IanOS UEFI loader\r\n");
        print(L"Automated boot-test marker found; skipping menu\r\n");
        return flags;
    }
    if ((flags & hybrid::kBootFlagRecovery) != 0) {
        print(L"IanOS UEFI loader\r\n");
        print(L"Recovery boot marker found; skipping menu\r\n");
        return flags;
    }

    UINTN selected = 0;
    draw_boot_menu(selected);
    if (st && st->con_in && st->con_in->reset) st->con_in->reset(st->con_in, false);
    for (;;) {
        EFI_INPUT_KEY key;
        key.scan_code = 0;
        key.unicode_char = 0;
        if (!read_menu_key(&key)) {
            sleep_us(20000);
            continue;
        }
        if (key.unicode_char == L'1') selected = 0;
        else if (key.unicode_char == L'2') selected = 1;
        else if (key.unicode_char == L'3') selected = 2;
        else if (key.unicode_char == L'4') selected = 3;
        else if (key.scan_code == kScanUp) selected = selected == 0 ? kBootMenuOptions - 1 : selected - 1;
        else if (key.scan_code == kScanDown) selected = (selected + 1) % kBootMenuOptions;
        else if (key.unicode_char == L'\r' || key.unicode_char == L'\n') break;
        else continue;
        draw_boot_menu(selected);
    }

    if (selected == 3) {
        set_text_attribute(0x0e);
        print(L"\r\nLegacy BIOS boot is not installed yet; continuing with UEFI normal boot.\r\n");
        set_text_attribute(0x07);
        sleep_us(1500000);
        selected = 0;
    }
    if (selected == 1) flags |= hybrid::kBootFlagRecovery;
    if (selected == 2) flags |= hybrid::kBootFlagDebug;
    clear_screen();
    if (selected == 1) print(L"IanOS UEFI loader: recovery boot\r\n");
    else if (selected == 2) print(L"IanOS UEFI loader: debug boot\r\n");
    else print(L"IanOS UEFI loader: normal boot\r\n");
    return flags;
}

void draw_progress_bar(UINTN percent) {
    constexpr UINTN kWidth = 30;
    UINTN filled = (percent * kWidth) / 100;
    print(L"[");
    for (UINTN i = 0; i < kWidth; ++i) print(i < filled ? L"#" : L".");
    print(L"] ");
    if (percent < 10) print(L"  ");
    else if (percent < 100) print(L" ");
    print_dec(percent);
    print(L"%\r\n");
}

void boot_progress(uint32_t flags, const CHAR16* text, UINTN percent) {
    clear_screen();
    set_cursor(0, 0);
    set_text_attribute(0x0f);
    if ((flags & hybrid::kBootFlagDebug) != 0) print(L"Loading IanOS debug environment\r\n");
    else if ((flags & hybrid::kBootFlagRecovery) != 0) print(L"Loading IanOS recovery environment\r\n");
    else print(L"Loading IanOS\r\n");
    set_text_attribute(0x07);
    draw_progress_bar(percent);
    print(text);
    print(L"\r\n");
}

void* memset(void* dst, int value, UINTN count) {
    auto* p = static_cast<unsigned char*>(dst);
    while (count--) *p++ = static_cast<unsigned char>(value);
    return dst;
}

void* memcpy(void* dst, const void* src, UINTN count) {
    auto* d = static_cast<unsigned char*>(dst);
    auto* s = static_cast<const unsigned char*>(src);
    while (count--) *d++ = *s++;
    return dst;
}

void copy_module_path(char (&out)[64], const char* path) {
    UINTN i = 0;
    for (; i + 1 < sizeof(out) && path[i] != 0; ++i) out[i] = path[i];
    out[i] = 0;
    for (++i; i < sizeof(out); ++i) out[i] = 0;
}

uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

EFI_STATUS open_root(EFI_FILE_PROTOCOL** root) {
    EFI_LOADED_IMAGE_PROTOCOL* loaded = nullptr;
    auto bs = st->boot_services;
    EFI_STATUS status = bs->open_protocol(image, const_cast<EFI_GUID*>(&kLoadedImageProtocol),
        reinterpret_cast<void**>(&loaded), image, nullptr, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(status)) return status;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = nullptr;
    status = bs->open_protocol(loaded->device_handle, const_cast<EFI_GUID*>(&kSimpleFsProtocol),
        reinterpret_cast<void**>(&fs), image, nullptr, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
    if (EFI_ERROR(status)) return status;
    return fs->open_volume(fs, root);
}

EFI_STATUS load_file(const CHAR16* path, void** buffer, UINTN* size) {
    EFI_FILE_PROTOCOL* root = nullptr;
    EFI_STATUS status = open_root(&root);
    if (EFI_ERROR(status)) return status;

    EFI_FILE_PROTOCOL* file = nullptr;
    status = root->open(root, &file, const_cast<CHAR16*>(path), EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (EFI_ERROR(status)) return status;

    unsigned char info_buf[256];
    UINTN info_size = sizeof(info_buf);
    status = file->get_info(file, const_cast<EFI_GUID*>(&kFileInfoGuid), &info_size, info_buf);
    if (EFI_ERROR(status)) return status;
    uint64_t file_size = *reinterpret_cast<uint64_t*>(info_buf + 8);

    status = st->boot_services->allocate_pool(EfiLoaderData, file_size, buffer);
    if (EFI_ERROR(status)) return status;
    *size = file_size;
    status = file->read(file, size, *buffer);
    file->close(file);
    return status;
}

bool file_exists(const CHAR16* path) {
    EFI_FILE_PROTOCOL* root = nullptr;
    if (EFI_ERROR(open_root(&root))) return false;
    EFI_FILE_PROTOCOL* file = nullptr;
    EFI_STATUS status = root->open(root, &file, const_cast<CHAR16*>(path), EFI_FILE_MODE_READ, EFI_FILE_READ_ONLY);
    if (EFI_ERROR(status)) return false;
    file->close(file);
    return true;
}

EFI_STATUS load_kernel(void* elf, UINTN elf_size, uint64_t* entry, uint64_t* kernel_base, uint64_t* kernel_end) {
    if (elf_size < sizeof(Elf64_Ehdr)) return EFI_LOAD_ERROR;
    auto* eh = static_cast<Elf64_Ehdr*>(elf);
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return EFI_LOAD_ERROR;
    if (eh->e_machine != 62 || eh->e_phentsize != sizeof(Elf64_Phdr)) return EFI_LOAD_ERROR;

    auto* ph = reinterpret_cast<Elf64_Phdr*>(static_cast<unsigned char*>(elf) + eh->e_phoff);
    *kernel_base = UINT64_MAX;
    *kernel_end = 0;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != 1) continue;
        uint64_t phys = ph[i].p_paddr ? ph[i].p_paddr : ph[i].p_vaddr;
        uint64_t pages = align_up(ph[i].p_memsz, 4096) / 4096;
        EFI_STATUS status = st->boot_services->allocate_pages(AllocateAddress, EfiLoaderData, pages, &phys);
        if (EFI_ERROR(status)) return status;
        if (phys < *kernel_base) *kernel_base = phys;
        uint64_t segment_end = phys + align_up(ph[i].p_memsz, 4096);
        if (segment_end > *kernel_end) *kernel_end = segment_end;
        memcpy(reinterpret_cast<void*>(phys), static_cast<unsigned char*>(elf) + ph[i].p_offset, ph[i].p_filesz);
        memset(reinterpret_cast<unsigned char*>(phys) + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
    }
    *entry = eh->e_entry;
    if (*kernel_base == UINT64_MAX || *kernel_end <= *kernel_base) return EFI_LOAD_ERROR;
    return EFI_SUCCESS;
}

EFI_STATUS validate_user_elf(void* elf, UINTN elf_size) {
    if (elf_size < sizeof(Elf64_Ehdr)) return EFI_LOAD_ERROR;
    auto* eh = static_cast<Elf64_Ehdr*>(elf);
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return EFI_LOAD_ERROR;
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return EFI_LOAD_ERROR;
    if (eh->e_machine != 62 || eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_entry == 0) return EFI_LOAD_ERROR;
    if (eh->e_phoff + static_cast<uint64_t>(eh->e_phnum) * sizeof(Elf64_Phdr) > elf_size) return EFI_LOAD_ERROR;
    auto* ph = reinterpret_cast<Elf64_Phdr*>(static_cast<unsigned char*>(elf) + eh->e_phoff);
    bool has_load = false;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != 1) continue;
        has_load = true;
        if (ph[i].p_filesz > ph[i].p_memsz) return EFI_LOAD_ERROR;
        if (ph[i].p_offset + ph[i].p_filesz > elf_size) return EFI_LOAD_ERROR;
    }
    return has_load ? EFI_SUCCESS : EFI_LOAD_ERROR;
}

EFI_STATUS load_boot_module(const BootModuleSpec& spec, LoadedBootModule* loaded) {
    EFI_STATUS status = load_file(spec.firmware_path, &loaded->file, &loaded->size);
    if (EFI_ERROR(status)) {
        print(spec.firmware_path);
        print(L" load failed\r\n");
        return status;
    }
    status = validate_user_elf(loaded->file, loaded->size);
    if (EFI_ERROR(status)) {
        print(spec.firmware_path);
        print(L" ELF validation failed\r\n");
        return status;
    }
    return EFI_SUCCESS;
}

uint64_t find_rsdp() {
    for (UINTN i = 0; i < st->number_of_table_entries; ++i) {
        auto& table = st->configuration_table[i];
        if (guid_eq(table.vendor_guid, kAcpi20Table) || guid_eq(table.vendor_guid, kAcpi10Table)) {
            return reinterpret_cast<uint64_t>(table.vendor_table);
        }
    }
    return 0;
}

hybrid::MemoryType convert_memory_type(uint32_t type) {
    switch (type) {
    case EfiConventionalMemory: return hybrid::MemoryType::Usable;
    case EfiBootServicesCode:
    case EfiBootServicesData: return hybrid::MemoryType::BootServices;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData: return hybrid::MemoryType::RuntimeServices;
    case EfiACPIReclaimMemory: return hybrid::MemoryType::AcpiReclaim;
    case EfiACPIMemoryNVS: return hybrid::MemoryType::AcpiNvs;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace: return hybrid::MemoryType::Mmio;
    case EfiUnusableMemory: return hybrid::MemoryType::Bad;
    default: return hybrid::MemoryType::Reserved;
    }
}

EFI_STATUS locate_gop(hybrid::FramebufferInfo* fb) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = nullptr;
    EFI_STATUS status = st->boot_services->locate_protocol(const_cast<EFI_GUID*>(&kGopProtocol), nullptr, reinterpret_cast<void**>(&gop));
    if (EFI_ERROR(status)) return status;
    auto* mode = gop->mode;
    fb->base = mode->frame_buffer_base;
    fb->width = mode->info->horizontal_resolution;
    fb->height = mode->info->vertical_resolution;
    fb->pixels_per_scanline = mode->info->pixels_per_scan_line;
    fb->bytes_per_pixel = 4;
    fb->format = mode->info->pixel_format;
    fb->red_mask = mode->info->pixel_information[0];
    fb->green_mask = mode->info->pixel_information[1];
    fb->blue_mask = mode->info->pixel_information[2];
    fb->reserved_mask = mode->info->pixel_information[3];
    return EFI_SUCCESS;
}

EFI_STATUS build_memory_map(hybrid::BootInfo* boot, UINTN* key) {
    static unsigned char map_storage[128 * 1024];
    static hybrid::MemoryRegion regions[2048];

    UINTN map_size = sizeof(map_storage), descriptor_size = 0;
    UINT32 descriptor_version = 0;
    auto* map = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(map_storage);
    EFI_STATUS status = st->boot_services->get_memory_map(&map_size, map, key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) return status;
    UINTN entries = map_size / descriptor_size;
    if (entries > 2048) return EFI_BUFFER_TOO_SMALL;

    for (UINTN i = 0; i < entries; ++i) {
        auto* desc = reinterpret_cast<EFI_MEMORY_DESCRIPTOR*>(reinterpret_cast<unsigned char*>(map) + i * descriptor_size);
        regions[i] = {desc->physical_start, desc->number_of_pages * 4096, convert_memory_type(desc->type), static_cast<uint32_t>(desc->attribute)};
    }
    boot->memory_map = reinterpret_cast<uint64_t>(regions);
    boot->memory_map_entries = entries;
    boot->memory_map_descriptor_size = sizeof(hybrid::MemoryRegion);
    return EFI_SUCCESS;
}

} // namespace

extern "C" EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE* system_table) {
    image = image_handle;
    st = system_table;
    st->boot_services->set_watchdog_timer(0, 0, 0, nullptr);
    uint32_t boot_flags = select_boot_flags();
    boot_progress(boot_flags, L"Preparing boot services", 5);

    void* kernel_file = nullptr;
    UINTN kernel_size = 0;
    EFI_STATUS status = load_file(L"\\kernel.elf", &kernel_file, &kernel_size);
    if (EFI_ERROR(status)) { print(L"kernel.elf load failed\r\n"); return status; }
    boot_progress(boot_flags, L"Kernel image read from ESP", 20);

    LoadedBootModule loaded_modules[kBootModuleCount];
    for (UINTN i = 0; i < kBootModuleCount; ++i) {
        status = load_boot_module(kBootModuleSpecs[i], &loaded_modules[i]);
        if (EFI_ERROR(status)) return status;
    }
    boot_progress(boot_flags, L"Userland modules loaded", 45);

    uint64_t entry = 0;
    uint64_t kernel_base = 0;
    uint64_t kernel_end = 0;
    status = load_kernel(kernel_file, kernel_size, &entry, &kernel_base, &kernel_end);
    if (EFI_ERROR(status)) { print(L"ELF64 load failed\r\n"); return status; }
    boot_progress(boot_flags, L"Mattas ELF64 image mapped", 65);

    hybrid::BootInfo* boot = nullptr;
    status = st->boot_services->allocate_pool(EfiLoaderData, sizeof(hybrid::BootInfo), reinterpret_cast<void**>(&boot));
    if (EFI_ERROR(status)) return status;
    memset(boot, 0, sizeof(*boot));
    hybrid::BootModule* modules = nullptr;
    status = st->boot_services->allocate_pool(EfiLoaderData, sizeof(hybrid::BootModule) * kBootModuleCount, reinterpret_cast<void**>(&modules));
    if (EFI_ERROR(status)) return status;
    memset(modules, 0, sizeof(hybrid::BootModule) * kBootModuleCount);
    for (UINTN i = 0; i < kBootModuleCount; ++i) {
        modules[i].base = reinterpret_cast<uint64_t>(loaded_modules[i].file);
        modules[i].size = loaded_modules[i].size;
        copy_module_path(modules[i].path, kBootModuleSpecs[i].kernel_path);
    }

    boot->magic = hybrid::kBootInfoMagic;
    boot->version = hybrid::kBootInfoVersion;
    boot->size = sizeof(hybrid::BootInfo);
    boot->flags = boot_flags;
    boot->rsdp = find_rsdp();
    boot->kernel_physical_base = kernel_base;
    boot->kernel_physical_end = kernel_end;
    boot->kernel_entry = entry;
    boot->user_init_base = reinterpret_cast<uint64_t>(loaded_modules[0].file);
    boot->user_init_size = loaded_modules[0].size;
    boot->boot_modules = reinterpret_cast<uint64_t>(modules);
    boot->boot_module_count = kBootModuleCount;
    boot->hhdm_offset = 0;
    locate_gop(&boot->framebuffer);
    boot_progress(boot_flags, L"Framebuffer and boot data prepared", 82);

    UINTN map_key = 0;
    status = build_memory_map(boot, &map_key);
    if (EFI_ERROR(status)) { print(L"memory map failed\r\n"); return status; }
    boot_progress(boot_flags, L"Leaving UEFI boot services", 100);

    status = st->boot_services->exit_boot_services(image, map_key);
    if (EFI_ERROR(status)) {
        status = build_memory_map(boot, &map_key);
        if (EFI_ERROR(status)) return status;
        status = st->boot_services->exit_boot_services(image, map_key);
        if (EFI_ERROR(status)) return status;
    }

    using KernelEntry = void(__attribute__((ms_abi)) *)(hybrid::BootInfo*);
    reinterpret_cast<KernelEntry>(entry)(boot);
    for (;;) {}
}
