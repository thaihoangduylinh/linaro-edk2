"""Host tests: python -m unittest discover -s <EmmcDump-directory>."""

import contextlib
import io
from pathlib import Path
import tempfile
import unittest
import zlib

from verify_dump import verify


class VerifyDumpTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.dump = self.base / "dump"
        self.dump.mkdir()
        self.data = [bytes(range(256)) * 2, b"Z" * 512]
        records = ["EmmcDump format=1", "total_bytes=1024 block_size=512 last_lba=1"]
        for index, data in enumerate(self.data):
            name = "emmc-%04d.bin" % index
            (self.dump / name).write_bytes(data)
            records.append("part=%s bytes=%d crc32=%08x" %
                           (name, len(data), zlib.crc32(data) & 0xFFFFFFFF))
        records.append("COMPLETE bytes=1024 parts=2")
        self.manifest = self.dump / "manifest.txt"
        self.manifest.write_text("\n".join(records), encoding="ascii")

    def run_verify(self, merge=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return verify(self.dump, merge)

    def test_complete_and_merge_exact_bytes(self):
        output = self.base / "emmc.bin"
        self.assertEqual(self.run_verify(output), 1024)
        self.assertEqual(output.read_bytes(), b"".join(self.data))
        self.assertFalse((self.base / "emmc.bin.partial").exists())

    def test_same_size_corruption_not_published(self):
        (self.dump / "emmc-0001.bin").write_bytes(b"Y" * 512)
        output = self.base / "emmc.bin"
        with self.assertRaisesRegex(ValueError, "CRC32"):
            self.run_verify(output)
        self.assertFalse(output.exists())

    def test_truncated_part(self):
        (self.dump / "emmc-0001.bin").write_bytes(b"Z" * 511)
        with self.assertRaisesRegex(ValueError, "size mismatch"):
            self.run_verify()

    def test_missing_complete(self):
        self.manifest.write_text(self.manifest.read_text().replace("COMPLETE", "INTERRUPTED"))
        with self.assertRaisesRegex(ValueError, "COMPLETE"):
            self.run_verify()

    def test_failed_dump_rejected(self):
        self.manifest.write_text(self.manifest.read_text() + "\nFAILED status=Device Error\n")
        with self.assertRaisesRegex(ValueError, "failed dump"):
            self.run_verify()

    def test_missing_part_record(self):
        self.manifest.write_text("\n".join(line for line in self.manifest.read_text().splitlines()
                                           if not line.startswith("part=emmc-0000")))
        with self.assertRaisesRegex(ValueError, "missing"):
            self.run_verify()

    def test_geometry_mismatch(self):
        self.manifest.write_text(self.manifest.read_text().replace("last_lba=1", "last_lba=2"))
        with self.assertRaisesRegex(ValueError, "geometry"):
            self.run_verify()

    def test_never_overwrite_output(self):
        output = self.base / "emmc.bin"
        output.write_bytes(b"keep me")
        with self.assertRaises(FileExistsError):
            self.run_verify(output)
        self.assertEqual(output.read_bytes(), b"keep me")

    def test_missing_part_file(self):
        (self.dump / "emmc-0001.bin").unlink()
        with self.assertRaises(FileNotFoundError):
            self.run_verify()


if __name__ == "__main__":
    unittest.main()
