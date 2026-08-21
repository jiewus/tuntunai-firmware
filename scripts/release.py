#!/usr/bin/env python3
"""Build and package a non-secure release for any board declared under main/boards.

本项目为开发版固件，所有发布均为非安全加固版本：
- 不启用 Secure Boot / Flash Encryption / 防降级，不写入任何 eFuse；
- 任何设备均可直接烧录，可随时重刷。
"""

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
RELEASE_DIRECTORIES = {
    "echoear": "EchoEar",
}
REQUIRED_DISABLED_OPTIONS = (
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_FLASH_ENC_ENABLED",
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
)


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
        Path.home() / ".espressif" / "frameworks" / "esp-idf-v6.0.2",
        Path.home() / ".espressif" / "v6.0.2" / "esp-idf",
        Path.home() / "esp" / "esp-idf",
    ]
    installed = [path for path in sdk_candidates if (path / "tools" / "idf.py").exists()]
    if installed:
        raise RuntimeError(
            "ESP-IDF is installed but its environment is not loaded. "
            f"Run: source {installed[0] / 'export.sh'}"
        )

    raise RuntimeError(
        "ESP-IDF SDK is not installed. Install ESP-IDF 6.0.2 before building firmware "
        "(IDF 5.5.x 与本项目组件和结构体不兼容，无法编译)。"
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


def reset_release_sdkconfig(build_dir: Path) -> None:
    """Remove generated configuration files so defaults are applied from scratch."""
    for filename in ("sdkconfig", "sdkconfig.old"):
        path = build_dir / filename
        if path.exists():
            path.unlink()


def validate_non_secure_sdkconfig(sdkconfig: Path) -> None:
    """Fail unless the generated release configuration keeps security eFuse features off."""
    if not sdkconfig.is_file():
        raise RuntimeError(f"Generated sdkconfig not found: {sdkconfig}")

    values: dict[str, str] = {}
    for raw_line in sdkconfig.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            values[name] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"

    invalid = [option for option in REQUIRED_DISABLED_OPTIONS if values.get(option) != "n"]
    if invalid:
        raise RuntimeError(
            "Non-secure release configuration check failed; expected disabled: "
            + ", ".join(invalid)
        )


def package_firmware(board: str, build_dir: Path) -> Path:
    """Publish the merged image without mixing artifacts from other targets."""
    merged = build_dir / "tuntun-binary.bin"
    if not merged.exists():
        raise RuntimeError(f"Merged firmware not found: {merged}")

    releases = PROJECT_ROOT / "releases"
    releases.mkdir(exist_ok=True)
    release_directory = RELEASE_DIRECTORIES.get(board)
    if release_directory:
        output_directory = releases / release_directory
        output_directory.mkdir(parents=True, exist_ok=True)
        output = output_directory / "tuntun-binary.bin"
        shutil.copy2(merged, output)
        return output

    output = releases / f"v{project_version()}_{board}.zip"
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(merged, arcname="tuntun-binary.bin")
    return output


def build(board: str) -> Path:
    """构建非安全发布固件并创建完整合并镜像。

    使用项目根与板型的 sdkconfig.defaults 构建（无 Secure Boot / Flash 加密 / 防回滚），
    merge 成可完整烧录的 tuntun-binary.bin 并打包。
    """
    board_dir, _ = load_board(board)
    build_dir = PROJECT_ROOT / "build" / board / "release"
    sdkconfig_defaults = ";".join(
        [
            str(PROJECT_ROOT / "sdkconfig.defaults"),
            str(board_dir / "sdkconfig.defaults"),
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
    reset_release_sdkconfig(build_dir)
    run(*common_args, "reconfigure")
    validate_non_secure_sdkconfig(build_dir / "sdkconfig")
    run(*common_args, "build")
    validate_non_secure_sdkconfig(build_dir / "sdkconfig")
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
