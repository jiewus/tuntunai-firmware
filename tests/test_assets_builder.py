import struct
import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

import build_default_assets  # noqa: E402


class AssetsBuilderTest(unittest.TestCase):
    def test_generated_index_has_terminated_names_and_bounded_data(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            include = root / "include"
            output = root / "assets.bin"
            assets.mkdir()
            (assets / "index.json").write_bytes(b"{}")
            (assets / "image.png").write_bytes(b"png-data")

            build_default_assets.pack_assets_simple(
                str(assets), str(include), str(output), "assets")

            data = output.read_bytes()
            file_count, checksum, stored_length = struct.unpack_from("<III", data)
            self.assertEqual(file_count, 2)
            self.assertEqual(stored_length, len(data) - 12)
            self.assertEqual(checksum, sum(data[12:]) & 0xFFFF)

            table_size = file_count * 44
            payload_size = stored_length - table_size
            for index in range(file_count):
                entry_offset = 12 + index * 44
                name, size, offset = struct.unpack_from("<32sII", data, entry_offset)
                self.assertIn(0, name)
                self.assertLessEqual(offset + 2 + size, payload_size)

    def test_rejects_name_without_terminator_space(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            (assets / ("a" * 28 + ".bin")).write_bytes(b"data")

            with self.assertRaises(ValueError):
                build_default_assets.pack_assets_simple(
                    str(assets), str(root / "include"), str(root / "assets.bin"), "assets")


if __name__ == "__main__":
    unittest.main()
