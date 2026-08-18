import sys
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "scripts"))

import release  # noqa: E402


DISABLED_CONFIG = """\
# CONFIG_SECURE_BOOT is not set
# CONFIG_SECURE_FLASH_ENC_ENABLED is not set
# CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is not set
"""


class ReleaseConfigurationTest(unittest.TestCase):
    def test_accepts_non_secure_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            sdkconfig = Path(directory) / "sdkconfig"
            sdkconfig.write_text(DISABLED_CONFIG, encoding="utf-8")
            release.validate_non_secure_sdkconfig(sdkconfig)

    def test_rejects_enabled_or_missing_security_switches(self) -> None:
        for option in release.REQUIRED_DISABLED_OPTIONS:
            with self.subTest(option=option), tempfile.TemporaryDirectory() as directory:
                sdkconfig = Path(directory) / "sdkconfig"
                sdkconfig.write_text(
                    DISABLED_CONFIG.replace(f"# {option} is not set", f"{option}=y"),
                    encoding="utf-8",
                )
                with self.assertRaises(RuntimeError):
                    release.validate_non_secure_sdkconfig(sdkconfig)

    def test_reset_removes_only_generated_sdkconfig_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_dir = Path(directory)
            for filename in ("sdkconfig", "sdkconfig.old", "keep.txt"):
                (build_dir / filename).write_text(filename, encoding="utf-8")

            release.reset_release_sdkconfig(build_dir)

            self.assertFalse((build_dir / "sdkconfig").exists())
            self.assertFalse((build_dir / "sdkconfig.old").exists())
            self.assertTrue((build_dir / "keep.txt").exists())


if __name__ == "__main__":
    unittest.main()
