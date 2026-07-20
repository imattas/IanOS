from __future__ import annotations

import pathlib
import struct
import sys


BYTES_PER_SECTOR = 512


def align_up(value: int, align: int) -> int:
    return ((value + align - 1) // align) * align


def read_bpb(image: pathlib.Path) -> tuple[int, int]:
    data = image.read_bytes()[:BYTES_PER_SECTOR]
    if len(data) < BYTES_PER_SECTOR:
        raise ValueError(f"{image} is too small to contain a FAT boot sector")
    if data[510:512] != b"\x55\xAA" or data[54:62] != b"FAT16   ":
        raise ValueError(f"{image} does not have the expected FAT16 boot signature")
    sector_size = struct.unpack_from("<H", data, 11)[0]
    sectors_per_cluster = data[13]
    total16 = struct.unpack_from("<H", data, 19)[0]
    total32 = struct.unpack_from("<I", data, 32)[0]
    total_sectors = total16 or total32
    if sector_size == 0 or sectors_per_cluster == 0 or total_sectors == 0:
        raise ValueError(f"{image} does not look like a FAT image")
    return total_sectors * sector_size, sector_size * sectors_per_cluster


def collect_source(source: pathlib.Path, cluster_size: int) -> tuple[int, int, int, int]:
    file_count = 0
    directory_count = 0
    payload_bytes = 0
    allocated_bytes = 0
    for path in source.rglob("*"):
        if path.is_file():
            size = path.stat().st_size
            file_count += 1
            payload_bytes += size
            allocated_bytes += align_up(max(size, 1), cluster_size)
        elif path.is_dir():
            directory_count += 1
    return file_count, directory_count, payload_bytes, allocated_bytes


def size_of(path: pathlib.Path) -> int:
    return path.stat().st_size if path.exists() else 0


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: image_report.py <esp-dir> <kernel.img>", file=sys.stderr)
        return 2
    esp = pathlib.Path(sys.argv[1])
    image = pathlib.Path(sys.argv[2])
    if not esp.exists():
        print(f"image_report: missing ESP directory: {esp}", file=sys.stderr)
        return 1
    if not image.exists():
        print(f"image_report: missing image: {image}", file=sys.stderr)
        return 1

    capacity_bytes, cluster_size = read_bpb(image)
    file_count, directory_count, payload_bytes, allocated_bytes = collect_source(esp, cluster_size)
    image_bytes = image.stat().st_size
    free_estimate = capacity_bytes - allocated_bytes if capacity_bytes > allocated_bytes else 0

    boot = esp / "EFI" / "BOOT" / "BOOTX64.EFI"
    kernel = esp / "kernel.elf"
    init = esp / "user" / "init.elf"
    user_total = sum(path.stat().st_size for path in (esp / "bin").glob("*.elf")) if (esp / "bin").exists() else 0

    print("Image report:")
    print(f"  image: {image}")
    print(f"  image_bytes: {image_bytes}")
    print(f"  fat_capacity_bytes: {capacity_bytes}")
    print(f"  fat_cluster_bytes: {cluster_size}")
    print(f"  esp_payload_bytes: {payload_bytes}")
    print(f"  esp_allocated_estimate: {allocated_bytes}")
    print(f"  fat_free_estimate: {free_estimate}")
    print(f"  files: {file_count}")
    print(f"  directories: {directory_count}")
    print(f"  BOOTX64.EFI: {size_of(boot)}")
    print(f"  kernel.elf: {size_of(kernel)}")
    print(f"  init.elf: {size_of(init)}")
    print(f"  user_elves_total: {user_total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
