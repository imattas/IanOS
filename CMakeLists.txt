cmake_minimum_required(VERSION 3.24)

project(IanOS LANGUAGES ASM_NASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLANGXX clang++ HINTS "C:/Program Files/LLVM/bin" REQUIRED)
find_program(CLANG clang HINTS "C:/Program Files/LLVM/bin" REQUIRED)
find_program(LLD_LINK lld-link HINTS "C:/Program Files/LLVM/bin" REQUIRED)
find_program(LD_LLD ld.lld HINTS "C:/Program Files/LLVM/bin" REQUIRED)
find_program(PYTHON py REQUIRED)

set(OUT_DIR "${CMAKE_BINARY_DIR}/out")
set(ESP_DIR "${CMAKE_BINARY_DIR}/esp")
set(EFI_BOOT_DIR "${ESP_DIR}/EFI/BOOT")
file(MAKE_DIRECTORY "${OUT_DIR}" "${EFI_BOOT_DIR}")

set(BOOTLOADER_EFI "${OUT_DIR}/BOOTX64.EFI")
set(KERNEL_ELF "${OUT_DIR}/kernel.elf")
set(USER_INIT_ELF "${OUT_DIR}/init.elf")
set(BOOTABLE_IMG "${OUT_DIR}/image/kernel.img")
set(USER_PROGRAMS hello args cat ls uname hostname free uptime date cal dmesg ps pwd env printenv sysinfo fastfetch sysctl id ids ctx echo sleep true false touch append rm cp mv dd wc grep tee mkdir rmdir err stat file whoami basename dirname head tail test sort uniq find hexdump readelf sha256sum sha512sum sha384sum sha1sum md5sum cmp cksum fold printf strings nl tr sed cut paste rev tac seq expr xargs yes od base64 which sh duptest fds lsof fdinh ln readlink realpath truncate blk mount df du lsblk pipeinfo kill killall pgrep uyield ubusy slowcat burst loop devio tty stty ttyread clear)
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(AP_TRAMPOLINE_BIN "${CMAKE_BINARY_DIR}/kernel/ap_trampoline.bin")
set(AP_TRAMPOLINE_HEADER "${GENERATED_DIR}/hk/smp/ap_trampoline_bin.hpp")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/bootloader/main.obj"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bootloader"
    COMMAND "${CLANGXX}" -target x86_64-pc-windows-msvc
            -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fshort-wchar
            -fno-stack-protector -mno-red-zone -Wall -Wextra
            -I${CMAKE_SOURCE_DIR}/common/include
            -I${CMAKE_SOURCE_DIR}/bootloader/include
            -c "${CMAKE_SOURCE_DIR}/bootloader/src/main.cpp"
            -o "${CMAKE_BINARY_DIR}/bootloader/main.obj"
    DEPENDS bootloader/src/main.cpp bootloader/include/uefi.hpp common/include/hybrid/boot_info.hpp common/include/hybrid/version.hpp
    VERBATIM
)

add_custom_command(
    OUTPUT "${BOOTLOADER_EFI}"
    COMMAND "${LLD_LINK}" /nologo /subsystem:efi_application /entry:efi_main /nodefaultlib
            "/out:${BOOTLOADER_EFI}"
            "${CMAKE_BINARY_DIR}/bootloader/main.obj"
    DEPENDS "${CMAKE_BINARY_DIR}/bootloader/main.obj"
    VERBATIM
)

add_custom_target(bootloader ALL DEPENDS "${BOOTLOADER_EFI}")

set(KERNEL_CPP
    kernel/src/main.cpp
    kernel/src/boot/bootinfo.cpp
    kernel/src/console.cpp
    kernel/src/terminal.cpp
    kernel/src/log.cpp
    kernel/src/cpu/gdt.cpp
    kernel/src/cpu/idt.cpp
    kernel/src/cpu/features.cpp
    kernel/src/cpu/topology.cpp
    kernel/src/cpu/runtime.cpp
    kernel/src/mm/pmm.cpp
    kernel/src/mm/vmm.cpp
    kernel/src/mm/address_space.cpp
    kernel/src/mm/heap.cpp
    kernel/src/interrupts/exceptions.cpp
    kernel/src/interrupts/irq.cpp
    kernel/src/acpi/acpi.cpp
    kernel/src/apic/apic.cpp
    kernel/src/apic/io_apic.cpp
    kernel/src/pci/pci.cpp
    kernel/src/block/block_device.cpp
    kernel/src/fs/vfs.cpp
    kernel/src/timer/pit.cpp
    kernel/src/time/rtc.cpp
    kernel/src/sched/scheduler.cpp
    kernel/src/smp/smp.cpp
    kernel/src/sync/spinlock.cpp
    kernel/src/syscall/syscall.cpp
    kernel/src/tests/selftests.cpp
    kernel/src/drivers/driver_manager.cpp
    kernel/src/drivers/ahci.cpp
    kernel/src/drivers/e1000.cpp
    kernel/src/drivers/vga.cpp
    kernel/src/drivers/device_inventory.cpp
    kernel/src/drivers/ps2_keyboard.cpp
    kernel/src/userspace/userspace.cpp
    kernel/src/lib/string.cpp
    kernel/src/arch/x86_64/user_entry.cpp
)

file(GLOB_RECURSE COMMON_HEADERS CONFIGURE_DEPENDS common/include/*.hpp)
file(GLOB_RECURSE KERNEL_HEADERS CONFIGURE_DEPENDS kernel/include/*.hpp)
file(GLOB_RECURSE USERLAND_HEADERS CONFIGURE_DEPENDS userland/include/*.hpp)

add_custom_command(
    OUTPUT "${AP_TRAMPOLINE_BIN}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/kernel"
    COMMAND "${CMAKE_ASM_NASM_COMPILER}" -f bin "${CMAKE_SOURCE_DIR}/kernel/src/arch/x86_64/ap_trampoline.asm" -o "${AP_TRAMPOLINE_BIN}"
    DEPENDS kernel/src/arch/x86_64/ap_trampoline.asm
    VERBATIM
)

add_custom_command(
    OUTPUT "${AP_TRAMPOLINE_HEADER}"
    COMMAND "${PYTHON}" -B "${CMAKE_SOURCE_DIR}/tools/bin2header.py" "${AP_TRAMPOLINE_BIN}" "${AP_TRAMPOLINE_HEADER}" "hk::smp::trampoline_image" "ap_trampoline"
    DEPENDS "${AP_TRAMPOLINE_BIN}" tools/bin2header.py
    VERBATIM
)

set(KERNEL_OBJECTS)
foreach(src IN LISTS KERNEL_CPP)
    get_filename_component(src_name "${src}" NAME_WE)
    string(REPLACE "/" "_" obj_name "${src}")
    set(obj "${CMAKE_BINARY_DIR}/kernel/${obj_name}.o")
    add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/kernel"
        COMMAND "${CLANGXX}" -target x86_64-unknown-none-elf
                -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
                -mno-red-zone -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}/common/include
                -I${CMAKE_SOURCE_DIR}/kernel/include
                -I${GENERATED_DIR}
                -c "${CMAKE_SOURCE_DIR}/${src}"
                -o "${obj}"
        DEPENDS "${src}" ${KERNEL_HEADERS} ${COMMON_HEADERS} "${AP_TRAMPOLINE_HEADER}"
        VERBATIM
    )
    list(APPEND KERNEL_OBJECTS "${obj}")
endforeach()

set(KERNEL_ASM_OBJECTS)
foreach(src IN ITEMS kernel/src/arch/x86_64/start.asm kernel/src/arch/x86_64/interrupt_stubs.asm kernel/src/arch/x86_64/user_entry.asm)
    string(REPLACE "/" "_" obj_name "${src}")
    set(obj "${CMAKE_BINARY_DIR}/kernel/${obj_name}.o")
    add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/kernel"
        COMMAND "${CMAKE_ASM_NASM_COMPILER}" -f elf64 "${CMAKE_SOURCE_DIR}/${src}" -o "${obj}"
        DEPENDS "${src}"
        VERBATIM
    )
    list(APPEND KERNEL_ASM_OBJECTS "${obj}")
endforeach()

add_custom_command(
    OUTPUT "${KERNEL_ELF}"
    COMMAND "${LD_LLD}" -nostdlib -z max-page-size=0x1000
            -T "${CMAKE_SOURCE_DIR}/kernel/linker.ld"
            -o "${KERNEL_ELF}"
            ${KERNEL_ASM_OBJECTS} ${KERNEL_OBJECTS}
    DEPENDS ${KERNEL_ASM_OBJECTS} ${KERNEL_OBJECTS} kernel/linker.ld
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_custom_target(kernel ALL DEPENDS "${KERNEL_ELF}")

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/userland/runtime.cpp.o"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/userland"
    COMMAND "${CLANGXX}" -target x86_64-unknown-none-elf
            -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
            -mno-red-zone -Wall -Wextra
            -I${CMAKE_SOURCE_DIR}/common/include
            -I${CMAKE_SOURCE_DIR}/userland/include
            -c "${CMAKE_SOURCE_DIR}/userland/src/runtime.cpp"
            -o "${CMAKE_BINARY_DIR}/userland/runtime.cpp.o"
    DEPENDS userland/src/runtime.cpp ${COMMON_HEADERS} ${USERLAND_HEADERS}
    VERBATIM
)

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/userland/init.cpp.o"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/userland"
    COMMAND "${CLANGXX}" -target x86_64-unknown-none-elf
            -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
            -mno-red-zone -Wall -Wextra
            -I${CMAKE_SOURCE_DIR}/common/include
            -I${CMAKE_SOURCE_DIR}/userland/include
            -c "${CMAKE_SOURCE_DIR}/userland/src/init.cpp"
            -o "${CMAKE_BINARY_DIR}/userland/init.cpp.o"
    DEPENDS userland/src/init.cpp ${COMMON_HEADERS} ${USERLAND_HEADERS}
    VERBATIM
)

add_custom_command(
    OUTPUT "${USER_INIT_ELF}"
    COMMAND "${LD_LLD}" -nostdlib -z max-page-size=0x1000
            -T "${CMAKE_SOURCE_DIR}/userland/linker.ld"
            -o "${USER_INIT_ELF}"
            "${CMAKE_BINARY_DIR}/userland/init.cpp.o"
            "${CMAKE_BINARY_DIR}/userland/runtime.cpp.o"
    DEPENDS "${CMAKE_BINARY_DIR}/userland/init.cpp.o" "${CMAKE_BINARY_DIR}/userland/runtime.cpp.o" userland/linker.ld
    VERBATIM
)

add_custom_target(userland ALL DEPENDS "${USER_INIT_ELF}")

set(USER_PROGRAM_ELFS)
set(USER_PROGRAM_TARGETS)

function(add_user_program name)
    set(obj "${CMAKE_BINARY_DIR}/userland/${name}.cpp.o")
    set(elf "${OUT_DIR}/${name}.elf")

    add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/userland"
        COMMAND "${CLANGXX}" -target x86_64-unknown-none-elf
                -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector
                -mno-red-zone -Wall -Wextra
                -I${CMAKE_SOURCE_DIR}/common/include
                -I${CMAKE_SOURCE_DIR}/userland/include
                -c "${CMAKE_SOURCE_DIR}/userland/src/${name}.cpp"
                -o "${obj}"
        DEPENDS "userland/src/${name}.cpp" ${COMMON_HEADERS} ${USERLAND_HEADERS}
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${elf}"
        COMMAND "${LD_LLD}" -nostdlib -z max-page-size=0x1000
                -T "${CMAKE_SOURCE_DIR}/userland/linker.ld"
                -o "${elf}"
                "${obj}"
                "${CMAKE_BINARY_DIR}/userland/runtime.cpp.o"
        DEPENDS "${obj}" "${CMAKE_BINARY_DIR}/userland/runtime.cpp.o" userland/linker.ld
        VERBATIM
    )

    add_custom_target("user_${name}" ALL DEPENDS "${elf}")
    list(APPEND USER_PROGRAM_ELFS "${elf}")
    list(APPEND USER_PROGRAM_TARGETS "user_${name}")
    set(USER_PROGRAM_ELFS "${USER_PROGRAM_ELFS}" PARENT_SCOPE)
    set(USER_PROGRAM_TARGETS "${USER_PROGRAM_TARGETS}" PARENT_SCOPE)
endfunction()

foreach(program IN LISTS USER_PROGRAMS)
    add_user_program("${program}")
endforeach()

set(ESP_USER_PROGRAM_COPY_COMMANDS)
foreach(program IN LISTS USER_PROGRAMS)
    list(APPEND ESP_USER_PROGRAM_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT_DIR}/${program}.elf" "${ESP_DIR}/bin/${program}.elf"
    )
endforeach()

add_custom_target(esp ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EFI_BOOT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ESP_DIR}/user"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ESP_DIR}/bin"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ESP_DIR}/boot"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${ESP_DIR}/boot/runtests"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BOOTLOADER_EFI}" "${EFI_BOOT_DIR}/BOOTX64.EFI"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${KERNEL_ELF}" "${ESP_DIR}/kernel.elf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${USER_INIT_ELF}" "${ESP_DIR}/user/init.elf"
    ${ESP_USER_PROGRAM_COPY_COMMANDS}
    COMMAND "${PYTHON}" -B "${CMAKE_SOURCE_DIR}/tools/write_manifest.py" "${OUT_DIR}/manifest.txt" "${BOOTLOADER_EFI}" "${KERNEL_ELF}" "${USER_INIT_ELF}" ${USER_PROGRAM_ELFS}
    DEPENDS bootloader kernel userland ${USER_PROGRAM_TARGETS}
    COMMAND_EXPAND_LISTS
    VERBATIM
)

add_custom_target(image ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUT_DIR}/image"
    COMMAND "${PYTHON}" -B "${CMAKE_SOURCE_DIR}/tools/make_fat_image.py" "${ESP_DIR}" "${BOOTABLE_IMG}"
    DEPENDS esp tools/make_fat_image.py
    VERBATIM
)

enable_testing()
add_test(NAME artifact_layout
    COMMAND "${PYTHON}" -B "${CMAKE_SOURCE_DIR}/tools/validate_artifacts.py" "${OUT_DIR}" "${ESP_DIR}"
)
