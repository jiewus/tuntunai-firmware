#!/usr/bin/env python3
"""Build and package a secure release for any board declared under main/boards."""

import argparse
import os
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

from board_config import BOARD_ROOT, load_board


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BOARD = "movecall-moji2-esp32c5"
SECURE_BOOT_SIGNING_KEY = PROJECT_ROOT / "secure_boot_signing_key.pem"
RELEASE_DEFAULTS = PROJECT_ROOT / "sdkconfig.release.defaults"


def find_idf_py() -> str:
    """Locate idf.py and report how to load an installed ESP-IDF environment."""
    executable = shutil.which("idf.py")
    if executable:
        return executable

    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        candidate = Path(idf_path) / "tools" / "idf.py"
        if candidate.exists():
            return str(candidate)

    sdk_candidates = [
        Path.home() / ".espressif" / "v5.5.4" / "esp-idf",
        Path.home() / "esp" / "esp-idf",
        Path.home() / ".espressif" / "frameworks" / "esp-idf-v5.5.2",
        Path.home() / ".espressif" / "frameworks" / "esp-idf-v5.5.3",
    ]
    installed = [path for path in sdk_candidates if (path / "tools" / "idf.py").exists()]
    if installed:
        raise RuntimeError(
            "ESP-IDF is installed but its environment is not loaded. "
            f"Run: source {installed[0] / 'export.sh'}"
        )

    raise RuntimeError(
        "ESP-IDF SDK is not installed. Install ESP-IDF 5.5.4 before building firmware."
    )


def run(*args: str) -> None:
    """Run idf.py from the project root and fail on the first build error."""
    command = list(args)
    if command[0] == "idf.py":
        command[0] = find_idf_py()
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)


def project_version() -> str:
    """Read PROJECT_VER from the root CMake file."""
    cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', cmake)
    if not match:
        raise RuntimeError("PROJECT_VER is missing from CMakeLists.txt")
    return match.group(1)


def available_boards() -> list[str]:
    """Return all valid board directory names."""
    boards: list[str] = []
    for config_path in sorted(BOARD_ROOT.glob("*/config.json")):
        board = config_path.parent.name
        load_board(board)
        boards.append(board)
    return boards


def package_firmware(board: str, build_dir: Path) -> Path:
    """Compress the board's merged image without mixing artifacts from other targets."""
    merged = build_dir / "tuntun-binary.bin"
    if not merged.exists():
        raise RuntimeError(f"Merged firmware not found: {merged}")

    releases = PROJECT_ROOT / "releases"
    releases.mkdir(exist_ok=True)
    output = releases / f"v{project_version()}_{board}.zip"
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(merged, arcname="tuntun-binary.bin")
    return output


def build(board: str) -> Path:
    """Build one board with release security defaults and create its merged image."""
    board_dir, _ = load_board(board)
    if not SECURE_BOOT_SIGNING_KEY.is_file():
        raise RuntimeError(
            "缺少 Secure Boot 签名私钥。请在 Git 之外执行："
            "espsecure.py generate_signing_key --version 2 --scheme ecdsa256 "
            "secure_boot_signing_key.pem"
        )

    build_dir = PROJECT_ROOT / "build" / board / "release"
    sdkconfig_defaults = ";".join(
        [
            str(PROJECT_ROOT / "sdkconfig.defaults"),
            str(board_dir / "sdkconfig.defaults"),
            str(RELEASE_DEFAULTS),
        ]
    )
    common_args = (
        "idf.py",
        "-B",
        str(build_dir),
        f"-DBOARD_TYPE={board}",
        f"-DSDKCONFIG={build_dir / 'sdkconfig'}",
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
    )
    run(*common_args, "build")
    run(*common_args, "merge-bin", "-o", str(build_dir / "tuntun-binary.bin"))
    return package_firmware(board, build_dir)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", nargs="?", default=DEFAULT_BOARD)
    parser.add_argument("--list-boards", action="store_true")
    args = parser.parse_args()

    try:
        boards = available_boards()
        if args.list_boards:
            print("\n".join(boards))
            return
        if args.board not in boards:
            parser.error(f"unknown board '{args.board}'; available: {', '.join(boards)}")
        output = build(args.board)
    except (RuntimeError, ValueError) as error:
        parser.error(str(error))
    print(f"Firmware package: {output}")


if __name__ == "__main__":
    main()
