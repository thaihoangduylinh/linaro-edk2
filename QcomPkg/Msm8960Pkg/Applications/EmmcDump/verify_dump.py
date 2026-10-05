r"""Verify EmmcDump parts and optionally concatenate them. Python 3, stdlib only.

python verify_dump.py X:\EmmcDump-0000
python verify_dump.py X:\EmmcDump-0000 --merge D:\Backups\emmc.bin
"""

import argparse
import os
from pathlib import Path
import re
import zlib


def read_manifest(directory):
    text = (directory / "manifest.txt").read_text(encoding="ascii")
    lines = text.splitlines()
    if "EmmcDump format=1" not in lines:
        raise ValueError("Unsupported manifest format")
    if any(line.startswith(("INCOMPLETE", "FAILED")) for line in lines):
        raise ValueError("The application reported an incomplete/failed dump")
    complete = [re.fullmatch(r"COMPLETE bytes=(\d+) parts=(\d+)", line)
                for line in lines if line.startswith("COMPLETE")]
    if len(complete) != 1 or complete[0] is None:
        raise ValueError("Missing or invalid COMPLETE record")
    expected_bytes, expected_parts = map(int, complete[0].groups())
    geometry = [re.fullmatch(r"total_bytes=(\d+) block_size=(\d+) last_lba=(\d+)", line)
                for line in lines if line.startswith("total_bytes=")]
    if len(geometry) != 1 or geometry[0] is None:
        raise ValueError("Missing or invalid source geometry")
    total, block_size, last_lba = map(int, geometry[0].groups())
    if block_size <= 0 or total != (last_lba + 1) * block_size or total != expected_bytes:
        raise ValueError("Source geometry and COMPLETE byte count disagree")
    parts = []
    for line in lines:
        if not line.startswith("part="):
            continue
        match = re.fullmatch(r"part=(emmc-(\d+)\.bin) bytes=(\d+) crc32=([0-9a-fA-F]{8})", line)
        if match is None:
            raise ValueError("Invalid part record")
        name, index, size, crc = match.groups()
        index, size = int(index), int(size)
        if index != len(parts) or name != "emmc-%04d.bin" % index:
            raise ValueError("Parts are missing, duplicated or out of order")
        if size <= 0 or size > 1024**3 or size % block_size:
            raise ValueError("Invalid part size: " + name)
        parts.append((name, size, int(crc, 16)))
    if not parts or len(parts) != expected_parts or sum(p[1] for p in parts) != total:
        raise ValueError("Manifest part counts/sizes do not cover the entire source")
    return parts, total


def verify(directory, merge=None):
    directory = Path(directory)
    if "mode=partitions" in (directory / "manifest.txt").read_text(encoding="ascii").splitlines():
        if merge is not None:
            raise ValueError("A partition set is not a full disk image. Use --merge on one pNNNN-name directory at a time.")
        return verify_partition_set(directory)
    parts, total = read_manifest(directory)
    output = None
    partial = None
    if merge is not None:
        merge = Path(merge)
        # Keep output outside the dump folder to avoid modifying input artifacts.
        if merge.resolve().parent == directory.resolve():
            raise ValueError("Choose a merge destination outside the dump directory")
        if merge.exists():
            raise FileExistsError("Merge destination already exists: " + str(merge))
        partial = merge.with_name(merge.name + ".partial")
        output = partial.open("xb")
    try:
        for name, expected_size, expected_crc in parts:
            path = directory / name
            if path.stat().st_size != expected_size:
                raise ValueError("File size mismatch: " + name)
            size = 0
            crc = 0
            with path.open("rb") as source:
                while True:
                    data = source.read(4 * 1024 * 1024)
                    if not data:
                        break
                    size += len(data)
                    crc = zlib.crc32(data, crc)
                    if output is not None:
                        output.write(data)
            if size != expected_size or (crc & 0xFFFFFFFF) != expected_crc:
                raise ValueError("CRC32/size mismatch: " + name)
            print("OK %s: %d bytes, CRC32 %08x" % (name, size, crc & 0xFFFFFFFF))
        if output is not None:
            output.flush()
            os.fsync(output.fileno())
            output.close()
            output = None
            # Exclusive destination creation: do not replace a pre-existing file.
            # Hard-link publication keeps the verified bytes and fails on collision.
            os.link(str(partial), str(merge))
            partial.unlink()
        print("Verified complete dump: %d bytes, %d parts" % (total, len(parts)))
        return total
    finally:
        if output is not None:
            output.close()
        # On failure keep .partial for diagnosis, never present it as complete.


def verify_partition_set(directory):
    lines = (directory / "manifest.txt").read_text(encoding="ascii").splitlines()
    if "EmmcDump format=1" not in lines:
        raise ValueError("Unsupported partition set format")
    if any(line.startswith(("INCOMPLETE", "FAILED")) for line in lines):
        raise ValueError("The application reported an incomplete/failed partition set")
    complete = [re.fullmatch(r"PARTITION_SET_COMPLETE bytes=(\d+) partitions=(\d+)", line)
                for line in lines if line.startswith("PARTITION_SET_COMPLETE")]
    geometry = [re.fullmatch(r"partition_set block_size=(\d+) last_lba=(\d+)", line)
                for line in lines if line.startswith("partition_set ")]
    if len(complete) != 1 or complete[0] is None or len(geometry) != 1 or geometry[0] is None:
        raise ValueError("Missing partition set completion or source geometry")
    expected_bytes, expected_count = map(int, complete[0].groups())
    block_size, disk_last = map(int, geometry[0].groups())
    if block_size == 0 or not 1 <= expected_count <= 64:
        raise ValueError("Invalid partition set geometry/count")
    records = []
    for line in lines:
        if not line.startswith("partition_dir="):
            continue
        match = re.fullmatch(r"partition_dir=(p(\d{4})-[A-Za-z0-9_-]+) start_lba=(\d+) end_lba=(\d+) bytes=(\d+) name=(.+)", line)
        if match is None:
            raise ValueError("Invalid partition directory record")
        folder, index, start, end, size, name = match.groups()
        index, start, end, size = map(int, (index, start, end, size))
        if index != len(records) or start > end or end > disk_last or size != (end - start + 1) * block_size:
            raise ValueError("Partition range/size/order mismatch: " + name)
        if any(start <= previous[2] and previous[1] <= end for previous in records):
            raise ValueError("Overlapping partition ranges")
        records.append((folder, start, end, size, name))
    if len(records) != expected_count or sum(record[3] for record in records) != expected_bytes:
        raise ValueError("Partition set count/bytes mismatch")
    for folder, start, end, size, name in records:
        child = directory / folder
        if child.resolve().parent != directory.resolve():
            raise ValueError("Partition directory points outside the dump")
        child_lines = (child / "manifest.txt").read_text(encoding="ascii").splitlines()
        expected_range = "partition_name=%s source_start_lba=%d source_end_lba=%d" % (name, start, end)
        if expected_range not in child_lines:
            raise ValueError("Partition source offset/name mismatch: " + name)
        expected_geometry = "total_bytes=%d block_size=%d last_lba=%d" % (size, block_size, end - start)
        if expected_geometry not in child_lines:
            raise ValueError("Partition geometry mismatch: " + name)
        print("Partition: " + name)
        # These are individual range dumps; never merge them into a fake raw disk.
        read_manifest(child)
        if "mode=partitions" in child_lines:
            raise ValueError("Nested partition sets are not supported")
        if verify(child) != size:
            raise ValueError("Partition byte count mismatch: " + name)
    print("Verified partition set: %d bytes, %d partitions" % (expected_bytes, expected_count))
    return expected_bytes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--merge", type=Path, help="New output file on NTFS/ext4; needs enough space")
    args = parser.parse_args()
    try:
        verify(args.directory, args.merge)
    except (OSError, ValueError) as error:
        parser.exit(1, "ERROR: %s\n" % error)


if __name__ == "__main__":
    main()
