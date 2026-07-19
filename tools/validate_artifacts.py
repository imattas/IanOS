from __future__ import annotations

import pathlib
import struct
import sys


def fail(message: str) -> int:
    print(f"artifact_layout: {message}", file=sys.stderr)
    return 1


def check_elf(path: pathlib.Path, name: str, minimum_entry: int) -> int:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        return fail(f"{name} is not ELF")
    elf_class, endian = data[4], data[5]
    if elf_class != 2 or endian != 1:
        return fail(f"{name} is not little-endian ELF64")
    machine = struct.unpack_from("<H", data, 18)[0]
    entry = struct.unpack_from("<Q", data, 24)[0]
    if machine != 62:
        return fail(f"{name} machine is {machine}, expected x86_64")
    if entry < minimum_entry:
        return fail(f"{name} entry {entry:#x} is below expected base {minimum_entry:#x}")
    return 0


def elf_section_name(data: bytes, shstr: bytes, index: int) -> str:
    if index >= len(shstr):
        return ""
    end = shstr.find(b"\0", index)
    if end < 0:
        end = len(shstr)
    return shstr[index:end].decode("ascii", errors="replace")


def check_no_undefined_symbols(path: pathlib.Path, name: str) -> int:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return fail(f"{name} is not little-endian ELF64")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize = struct.unpack_from("<H", data, 58)[0]
    shnum = struct.unpack_from("<H", data, 60)[0]
    shstrndx = struct.unpack_from("<H", data, 62)[0]
    if shoff == 0 or shentsize < 64 or shnum == 0 or shstrndx >= shnum:
        return 0
    sections = []
    for i in range(shnum):
        off = shoff + i * shentsize
        sections.append(struct.unpack_from("<IIQQQQIIQQ", data, off))
    shstr = b""
    shstr_section = sections[shstrndx]
    if shstr_section[4] + shstr_section[5] <= len(data):
        shstr = data[shstr_section[4]:shstr_section[4] + shstr_section[5]]
    for section in sections:
        sh_name, sh_type, _sh_flags, _sh_addr, sh_offset, sh_size, sh_link, _sh_info, _sh_addralign, sh_entsize = section
        if sh_type != 2:
            continue
        if sh_entsize == 0 or sh_offset + sh_size > len(data) or sh_link >= len(sections):
            return fail(f"{name} has malformed symbol table")
        strtab_section = sections[sh_link]
        strtab_offset, strtab_size = strtab_section[4], strtab_section[5]
        if strtab_offset + strtab_size > len(data):
            return fail(f"{name} has malformed string table")
        strings = data[strtab_offset:strtab_offset + strtab_size]
        table_name = elf_section_name(data, shstr, sh_name)
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize):
            st_name, st_info, _st_other, st_shndx, st_value, _st_size = struct.unpack_from("<IBBHQQ", data, off)
            if st_shndx != 0 or st_name == 0:
                continue
            end = strings.find(b"\0", st_name)
            if end < 0:
                return fail(f"{name} has malformed symbol name in {table_name}")
            symbol = strings[st_name:end].decode("ascii", errors="replace")
            return fail(f"{name} has undefined symbol {symbol}")
    return 0


def check_pe(path: pathlib.Path) -> int:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        return fail("BOOTX64.EFI is not a PE image")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        return fail("BOOTX64.EFI has no PE signature")
    machine = struct.unpack_from("<H", data, pe_off + 4)[0]
    subsystem = struct.unpack_from("<H", data, pe_off + 4 + 20 + 68)[0]
    if machine != 0x8664:
        return fail(f"BOOTX64.EFI machine is {machine:#x}, expected x64")
    if subsystem != 10:
        return fail(f"BOOTX64.EFI subsystem is {subsystem}, expected EFI application")
    return 0


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1])
    esp_dir = pathlib.Path(sys.argv[2])
    user_programs = ("hello", "args", "cat", "ls", "uname", "hostname", "free", "uptime", "date", "cal", "dmesg", "ps", "pwd", "env", "printenv", "sysinfo", "fastfetch", "sysctl", "id", "ids", "groups", "ctx", "echo", "sleep", "true", "false", "touch", "append", "rm", "cp", "mv", "dd", "wc", "grep", "tee", "mkdir", "rmdir", "err", "stat", "statfs", "file", "lsattr", "namei", "tree", "whoami", "basename", "dirname", "head", "tail", "test", "sort", "uniq", "find", "hexdump", "readelf", "sha256sum", "sha224sum", "sha512sum", "sha384sum", "sha1sum", "md5sum", "cmp", "cksum", "fold", "printf", "strings", "nl", "tr", "sed", "cut", "paste", "rev", "tac", "seq", "expr", "xargs", "yes", "od", "base64", "which", "sh", "duptest", "fds", "lsof", "fdinh", "ln", "readlink", "realpath", "truncate", "blk", "mount", "df", "du", "lsblk", "pipeinfo", "kill", "killall", "pgrep", "pidof", "nproc", "lscpu", "schedstat", "uyield", "ubusy", "slowcat", "burst", "loop", "devio", "tty", "stty", "ttyread", "clear")
    boot = out_dir / "BOOTX64.EFI"
    kernel = out_dir / "kernel.elf"
    init = out_dir / "init.elf"
    image = out_dir / "image" / "kernel.img"
    user_elves = [out_dir / f"{name}.elf" for name in user_programs]
    esp_boot = esp_dir / "EFI" / "BOOT" / "BOOTX64.EFI"
    esp_kernel = esp_dir / "kernel.elf"
    esp_init = esp_dir / "user" / "init.elf"
    esp_user_elves = [esp_dir / "bin" / f"{name}.elf" for name in user_programs]
    for path in (
        boot, kernel, init, image, *user_elves,
        esp_boot, esp_kernel, esp_init, *esp_user_elves,
    ):
        if not path.exists() or path.stat().st_size == 0:
            return fail(f"missing or empty artifact: {path}")
    result = check_pe(boot) or check_elf(kernel, "kernel.elf", 0x100000) or check_elf(init, "init.elf", 0x400000)
    if result:
        return result
    image_data = image.read_bytes()[:512]
    if len(image_data) != 512 or image_data[510:512] != b"\x55\xAA" or image_data[54:62] != b"FAT16   ":
        return fail("kernel.img is not a FAT16 bootable disk image")
    for name, elf in zip(user_programs, user_elves):
        result = check_elf(elf, f"{name}.elf", 0x400000)
        if result:
            return result
    result = check_no_undefined_symbols(init, "init.elf")
    if result:
        return result
    for name, elf in zip(user_programs, user_elves):
        result = check_no_undefined_symbols(elf, f"{name}.elf")
        if result:
            return result
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
