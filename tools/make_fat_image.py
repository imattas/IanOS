from __future__ import annotations

import math
import pathlib
import struct
import sys


BYTES_PER_SECTOR = 512
SECTORS_PER_CLUSTER = 4
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 512
MIN_TOTAL_SECTORS = 32768
IMAGE_MARGIN_BYTES = 4 * 1024 * 1024
IMAGE_ALIGN_SECTORS = 2048
MEDIA_DESCRIPTOR = 0xF8
EOC = 0xFFFF
LFN_ATTR = 0x0F
LFN_LAST = 0x40
LFN_CHARS_PER_ENTRY = 13


def short_name(name: str) -> bytes:
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = base.upper()
    ext = ext.upper()
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError(f"{name!r} is not representable as FAT 8.3")
    if any(ch not in allowed for ch in base + ext):
        raise ValueError(f"{name!r} contains unsupported FAT 8.3 characters")
    return base.encode("ascii").ljust(8, b" ") + ext.encode("ascii").ljust(3, b" ")


def is_short_name(name: str) -> bool:
    try:
        short_name(name)
        return True
    except ValueError:
        return False


def sanitize_short_part(text: str) -> str:
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
    out = "".join(ch for ch in text.upper() if ch in allowed)
    return out or "FILE"


def make_short_alias(name: str, used: set[bytes]) -> bytes:
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    clean_base = sanitize_short_part(base)
    clean_ext = sanitize_short_part(ext)[:3] if ext else ""
    for index in range(1, 100):
        suffix = f"~{index}"
        alias_base = (clean_base[:8 - len(suffix)] + suffix).ljust(8, " ")
        candidate = alias_base.encode("ascii") + clean_ext.encode("ascii").ljust(3, b" ")
        if candidate not in used:
            return candidate
    raise ValueError(f"could not generate unique FAT 8.3 alias for {name!r}")


def lfn_checksum(short: bytes) -> int:
    checksum = 0
    for value in short:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 0xFF
    return checksum


def lfn_entry(sequence: int, chars: list[int], checksum: int) -> bytes:
    entry = bytearray(32)
    entry[0] = sequence
    entry[11] = LFN_ATTR
    entry[13] = checksum
    for i, value in enumerate(chars):
        if i < 5:
            offset = 1 + i * 2
        elif i < 11:
            offset = 14 + (i - 5) * 2
        else:
            offset = 28 + (i - 11) * 2
        struct.pack_into("<H", entry, offset, value)
    return bytes(entry)


def lfn_entries(name: str, short: bytes) -> list[bytes]:
    chars = [ord(ch) for ch in name]
    chunks = [chars[i:i + LFN_CHARS_PER_ENTRY] for i in range(0, len(chars), LFN_CHARS_PER_ENTRY)]
    if not chunks:
        return []
    checksum = lfn_checksum(short)
    entries: list[bytes] = []
    for sequence, chunk in enumerate(chunks, start=1):
        values = chunk[:]
        if len(values) < LFN_CHARS_PER_ENTRY:
            values.append(0)
            while len(values) < LFN_CHARS_PER_ENTRY:
                values.append(0xFFFF)
        marker = sequence | (LFN_LAST if sequence == len(chunks) else 0)
        entries.append(lfn_entry(marker, values, checksum))
    return list(reversed(entries))


def dir_entry(name: str, attr: int, cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = short_name(name)
    entry[11] = attr
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def directory_record(name: str, attr: int, cluster: int, size: int, used_short_names: set[bytes]) -> bytes:
    if is_short_name(name):
        short = short_name(name)
        if short not in used_short_names:
            used_short_names.add(short)
            return dir_entry(name, attr, cluster, size)
    else:
        short = make_short_alias(name, used_short_names)
    used_short_names.add(short)
    entry = bytearray(32)
    entry[0:11] = short
    entry[11] = attr
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return b"".join(lfn_entries(name, short)) + bytes(entry)


def dot_entry(name: str, cluster: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name.encode("ascii").ljust(11, b" ")
    entry[11] = 0x10
    struct.pack_into("<H", entry, 26, cluster)
    return bytes(entry)


def align_up(value: int, align: int) -> int:
    return ((value + align - 1) // align) * align


def estimate_source_bytes(source: pathlib.Path) -> int:
    cluster_size = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER
    file_bytes = 0
    entry_bytes = 0
    directory_count = 0
    for path in source.rglob("*"):
        if path.is_file():
            file_bytes += align_up(path.stat().st_size, cluster_size)
            entries = 1 if is_short_name(path.name) else 1 + math.ceil(len(path.name) / LFN_CHARS_PER_ENTRY)
            entry_bytes += entries * 32
        elif path.is_dir():
            directory_count += 1
            entries = 1 if is_short_name(path.name) else 1 + math.ceil(len(path.name) / LFN_CHARS_PER_ENTRY)
            entry_bytes += entries * 32
    directory_bytes = align_up(entry_bytes + directory_count * 64, cluster_size)
    return file_bytes + directory_bytes + IMAGE_MARGIN_BYTES


def fat_geometry(total_sectors: int) -> tuple[int, int, int, int]:
    root_dir_sectors = (ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
    fat_sectors = 1
    while True:
        data_sectors = total_sectors - RESERVED_SECTORS - FAT_COUNT * fat_sectors - root_dir_sectors
        clusters = data_sectors // SECTORS_PER_CLUSTER
        needed = math.ceil((clusters + 2) * 2 / BYTES_PER_SECTOR)
        if needed == fat_sectors:
            return fat_sectors, root_dir_sectors, RESERVED_SECTORS + FAT_COUNT * fat_sectors, clusters
        fat_sectors = needed


def choose_total_sectors(source: pathlib.Path) -> int:
    required_data_sectors = math.ceil(estimate_source_bytes(source) / BYTES_PER_SECTOR)
    total = MIN_TOTAL_SECTORS
    while True:
        fat_sectors, root_dir_sectors, _, clusters = fat_geometry(total)
        data_sectors = total - RESERVED_SECTORS - FAT_COUNT * fat_sectors - root_dir_sectors
        if data_sectors >= required_data_sectors and clusters > 4085:
            return total
        total = align_up(total + IMAGE_ALIGN_SECTORS, IMAGE_ALIGN_SECTORS)


class ImageBuilder:
    def __init__(self, source: pathlib.Path, output: pathlib.Path) -> None:
        self.source = source
        self.output = output
        self.total_sectors = choose_total_sectors(source)
        self.fat_sectors, self.root_dir_sectors, self.root_sector, self.cluster_count = fat_geometry(self.total_sectors)
        self.data_sector = self.root_sector + self.root_dir_sectors
        self.image = bytearray(self.total_sectors * BYTES_PER_SECTOR)
        self.fat = [0] * (self.cluster_count + 2)
        self.fat[0] = 0xFFF0 | MEDIA_DESCRIPTOR
        self.fat[1] = EOC
        self.next_cluster = 2

    def allocate_chain(self, data: bytes) -> int:
        cluster_size = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER
        count = max(1, math.ceil(len(data) / cluster_size))
        first = self.next_cluster
        for i in range(count):
            cluster = self.next_cluster
            self.next_cluster += 1
            self.fat[cluster] = self.next_cluster if i + 1 < count else EOC
            start = self.cluster_offset(cluster)
            chunk = data[i * cluster_size:(i + 1) * cluster_size]
            self.image[start:start + len(chunk)] = chunk
        return first

    def write_chain(self, first: int, data: bytes) -> None:
        cluster_size = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER
        count = max(1, math.ceil(len(data) / cluster_size))
        cluster = first
        for i in range(count):
            if i > 0:
                next_cluster = self.next_cluster
                self.next_cluster += 1
                self.fat[cluster] = next_cluster
                cluster = next_cluster
            self.fat[cluster] = EOC
            start = self.cluster_offset(cluster)
            chunk = data[i * cluster_size:(i + 1) * cluster_size]
            self.image[start:start + cluster_size] = b"\0" * cluster_size
            self.image[start:start + len(chunk)] = chunk

    def reserve_cluster(self) -> int:
        cluster = self.next_cluster
        self.next_cluster += 1
        self.fat[cluster] = EOC
        return cluster

    def cluster_offset(self, cluster: int) -> int:
        sector = self.data_sector + (cluster - 2) * SECTORS_PER_CLUSTER
        return sector * BYTES_PER_SECTOR

    def build_directory(self, path: pathlib.Path, self_cluster: int, parent_cluster: int) -> tuple[int, bytes]:
        if self_cluster == -1:
            self_cluster = self.reserve_cluster()
        entries: list[bytes] = []
        used_short_names: set[bytes] = set()
        if self_cluster != 0:
            entries.append(dot_entry(".", self_cluster))
            entries.append(dot_entry("..", parent_cluster))
            used_short_names.add(b".          ")
            used_short_names.add(b"..         ")
        for child in sorted(path.iterdir(), key=lambda p: (not p.is_dir(), p.name.upper())):
            if child.is_dir():
                cluster, _ = self.build_directory(child, -1, self_cluster)
                entries.append(directory_record(child.name, 0x10, cluster, 0, used_short_names))
            elif child.is_file():
                data = child.read_bytes()
                cluster = self.allocate_chain(data)
                entries.append(directory_record(child.name, 0x20, cluster, len(data), used_short_names))
        directory = b"".join(entries)
        directory = directory + b"\0" * ((BYTES_PER_SECTOR * SECTORS_PER_CLUSTER) - len(directory) % (BYTES_PER_SECTOR * SECTORS_PER_CLUSTER))
        self.write_chain(self_cluster, directory)
        return self_cluster, directory

    def write_boot_sector(self) -> None:
        bs = bytearray(BYTES_PER_SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = b"HYBRIDOS"
        struct.pack_into("<H", bs, 11, BYTES_PER_SECTOR)
        bs[13] = SECTORS_PER_CLUSTER
        struct.pack_into("<H", bs, 14, RESERVED_SECTORS)
        bs[16] = FAT_COUNT
        struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, 0)
        bs[21] = MEDIA_DESCRIPTOR
        struct.pack_into("<H", bs, 22, self.fat_sectors)
        struct.pack_into("<H", bs, 24, 63)
        struct.pack_into("<H", bs, 26, 255)
        struct.pack_into("<I", bs, 28, 0)
        struct.pack_into("<I", bs, 32, self.total_sectors)
        bs[36] = 0x80
        bs[38] = 0x29
        struct.pack_into("<I", bs, 39, 0x484B0001)
        bs[43:54] = b"HYBRIDKERN "
        bs[54:62] = b"FAT16   "
        bs[510:512] = b"\x55\xAA"
        self.image[:BYTES_PER_SECTOR] = bs

    def write_fats(self) -> None:
        fat_bytes = bytearray(self.fat_sectors * BYTES_PER_SECTOR)
        for index, value in enumerate(self.fat):
            if index * 2 + 2 <= len(fat_bytes):
                struct.pack_into("<H", fat_bytes, index * 2, value)
        for copy in range(FAT_COUNT):
            start = (RESERVED_SECTORS + copy * self.fat_sectors) * BYTES_PER_SECTOR
            self.image[start:start + len(fat_bytes)] = fat_bytes

    def build(self) -> None:
        self.write_boot_sector()
        root_entries: list[bytes] = []
        used_short_names: set[bytes] = set()
        for child in sorted(self.source.iterdir(), key=lambda p: (not p.is_dir(), p.name.upper())):
            if child.is_dir():
                cluster, _ = self.build_directory(child, -1, 0)
                root_entries.append(directory_record(child.name, 0x10, cluster, 0, used_short_names))
            elif child.is_file():
                data = child.read_bytes()
                cluster = self.allocate_chain(data)
                root_entries.append(directory_record(child.name, 0x20, cluster, len(data), used_short_names))
        root = b"".join(root_entries).ljust(self.root_dir_sectors * BYTES_PER_SECTOR, b"\0")
        start = self.root_sector * BYTES_PER_SECTOR
        self.image[start:start + len(root)] = root
        self.write_fats()
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(self.image)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_fat_image.py <esp-dir> <output.img>", file=sys.stderr)
        return 2
    ImageBuilder(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])).build()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
