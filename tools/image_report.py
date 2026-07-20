from __future__ import annotations

import pathlib
import struct
import sys
from dataclasses import dataclass


BYTES_PER_SECTOR = 512


@dataclass(frozen=True)
class ImageReport:
    image: pathlib.Path
    image_bytes: int
    fat_capacity_bytes: int
    fat_cluster_bytes: int
    esp_payload_bytes: int
    esp_allocated_estimate: int
    fat_free_estimate: int
    files: int
    directories: int
    bootx64_efi: int
    kernel_elf: int
    init_elf: int
    user_elves_total: int


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


def build_report(esp: pathlib.Path, image: pathlib.Path) -> ImageReport:
    if not esp.exists():
        raise FileNotFoundError(f"missing ESP directory: {esp}")
    if not image.exists():
        raise FileNotFoundError(f"missing image: {image}")

    capacity_bytes, cluster_size = read_bpb(image)
    file_count, directory_count, payload_bytes, allocated_bytes = collect_source(esp, cluster_size)
    image_bytes = image.stat().st_size
    free_estimate = capacity_bytes - allocated_bytes if capacity_bytes > allocated_bytes else 0

    boot = esp / "EFI" / "BOOT" / "BOOTX64.EFI"
    kernel = esp / "kernel.elf"
    init = esp / "user" / "init.elf"
    user_total = sum(path.stat().st_size for path in (esp / "bin").glob("*.elf")) if (esp / "bin").exists() else 0

    return ImageReport(
        image=image,
        image_bytes=image_bytes,
        fat_capacity_bytes=capacity_bytes,
        fat_cluster_bytes=cluster_size,
        esp_payload_bytes=payload_bytes,
        esp_allocated_estimate=allocated_bytes,
        fat_free_estimate=free_estimate,
        files=file_count,
        directories=directory_count,
        bootx64_efi=size_of(boot),
        kernel_elf=size_of(kernel),
        init_elf=size_of(init),
        user_elves_total=user_total,
    )


def print_report(report: ImageReport) -> None:
    print("Image report:")
    print(f"  image: {report.image}")
    print(f"  image_bytes: {report.image_bytes}")
    print(f"  fat_capacity_bytes: {report.fat_capacity_bytes}")
    print(f"  fat_cluster_bytes: {report.fat_cluster_bytes}")
    print(f"  esp_payload_bytes: {report.esp_payload_bytes}")
    print(f"  esp_allocated_estimate: {report.esp_allocated_estimate}")
    print(f"  fat_free_estimate: {report.fat_free_estimate}")
    print(f"  files: {report.files}")
    print(f"  directories: {report.directories}")
    print(f"  BOOTX64.EFI: {report.bootx64_efi}")
    print(f"  kernel.elf: {report.kernel_elf}")
    print(f"  init.elf: {report.init_elf}")
    print(f"  user_elves_total: {report.user_elves_total}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: image_report.py <esp-dir> <kernel.img>", file=sys.stderr)
        return 2
    try:
        print_report(build_report(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])))
    except (FileNotFoundError, ValueError) as error:
        print(f"image_report: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
