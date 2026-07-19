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
TOTAL_SECTORS = 131072
MEDIA_DESCRIPTOR = 0xF8
EOC = 0xFFFF


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


def dir_entry(name: str, attr: int, cluster: int, size: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = short_name(name)
    entry[11] = attr
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return bytes(entry)


def dot_entry(name: str, cluster: int) -> bytes:
    entry = bytearray(32)
    entry[0:11] = name.encode("ascii").ljust(11, b" ")
    entry[11] = 0x10
    struct.pack_into("<H", entry, 26, cluster)
    return bytes(entry)


def fat_geometry() -> tuple[int, int, int, int]:
    root_dir_sectors = (ROOT_ENTRIES * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR
    fat_sectors = 1
    while True:
        data_sectors = TOTAL_SECTORS - RESERVED_SECTORS - FAT_COUNT * fat_sectors - root_dir_sectors
        clusters = data_sectors // SECTORS_PER_CLUSTER
        needed = math.ceil((clusters + 2) * 2 / BYTES_PER_SECTOR)
        if needed == fat_sectors:
            return fat_sectors, root_dir_sectors, RESERVED_SECTORS + FAT_COUNT * fat_sectors, clusters
        fat_sectors = needed


class ImageBuilder:
    def __init__(self, source: pathlib.Path, output: pathlib.Path) -> None:
        self.source = source
        self.output = output
        self.fat_sectors, self.root_dir_sectors, self.root_sector, self.cluster_count = fat_geometry()
        self.data_sector = self.root_sector + self.root_dir_sectors
        self.image = bytearray(TOTAL_SECTORS * BYTES_PER_SECTOR)
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
        if self_cluster != 0:
            entries.append(dot_entry(".", self_cluster))
            entries.append(dot_entry("..", parent_cluster))
        for child in sorted(path.iterdir(), key=lambda p: (not p.is_dir(), p.name.upper())):
            if child.is_dir():
                cluster, _ = self.build_directory(child, -1, self_cluster)
                entries.append(dir_entry(child.name, 0x10, cluster, 0))
            elif child.is_file():
                data = child.read_bytes()
                cluster = self.allocate_chain(data)
                entries.append(dir_entry(child.name, 0x20, cluster, len(data)))
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
        struct.pack_into("<I", bs, 32, TOTAL_SECTORS)
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
        for child in sorted(self.source.iterdir(), key=lambda p: (not p.is_dir(), p.name.upper())):
            if child.is_dir():
                cluster, _ = self.build_directory(child, -1, 0)
                root_entries.append(dir_entry(child.name, 0x10, cluster, 0))
            elif child.is_file():
                data = child.read_bytes()
                cluster = self.allocate_chain(data)
                root_entries.append(dir_entry(child.name, 0x20, cluster, len(data)))
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
