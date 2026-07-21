#!/usr/bin/env python3
"""为唯一保留的 Tuntun Moji2 ESP32-C5 板型构建并打包发布固件。"""

import argparse
import json
import os
import re
import shutil
import subprocess
import zipfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
BOARD = "tuntun-moji2-esp32c5"
BOARD_DIR = PROJECT_ROOT / "main" / "boards" / BOARD


def find_idf_py() -> str:
    """查找当前终端可用的 idf.py；只发现 SDK 未加载时给出对应 export.sh 命令。"""
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
        export_script = installed[0] / "export.sh"
        raise RuntimeError(
            "ESP-IDF is installed but its environment is not loaded. "
            f"Run: source {export_script}"
        )

    raise RuntimeError(
        "ESP-IDF SDK is not installed. In VS Code run "
        "'ESP-IDF: Open ESP-IDF Installation Manager' and install ESP-IDF 5.5.4."
    )


def run(*args: str) -> None:
    """在项目根目录执行命令；参数按独立 argv 传递，遇到非零退出码立即抛出异常。"""
    command = list(args)
    if command[0] == "idf.py":
        command[0] = find_idf_py()
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)


def project_version() -> str:
    """从根 CMakeLists.txt 读取 PROJECT_VER；缺少版本声明时拒绝生成错误命名的包。"""
    cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', cmake)
    if not match:
        raise RuntimeError("PROJECT_VER is missing from CMakeLists.txt")
    return match.group(1)


def load_board_config() -> dict:
    """读取板级 config.json，返回目标芯片和 sdkconfig 追加项组成的字典。"""
    with (BOARD_DIR / "config.json").open(encoding="utf-8") as file:
        return json.load(file)


def append_sdkconfig(entries: list[str]) -> None:
    """把板型选择和额外 Kconfig 项追加到 sdkconfig；entries 中每项必须是完整 CONFIG_ 行。"""
    sdkconfig = PROJECT_ROOT / "sdkconfig"
    with sdkconfig.open("a", encoding="utf-8") as file:
        file.write("\n# Tuntun Moji2 build configuration\n")
        file.write("CONFIG_BOARD_TYPE_TUNTUN_MOJI2_ESP32C5=y\n")
        for entry in entries:
            file.write(f"{entry}\n")


def package_firmware(name: str) -> Path:
    """将 tuntun-binary.bin 压缩到 releases；name 用于输出文件名，返回 ZIP 完整路径。"""
    merged = PROJECT_ROOT / "build" / "tuntun-binary.bin"
    if not merged.exists():
        raise RuntimeError(f"Merged firmware not found: {merged}")

    releases = PROJECT_ROOT / "releases"
    releases.mkdir(exist_ok=True)
    output = releases / f"v{project_version()}_{name}.zip"
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(merged, arcname="tuntun-binary.bin")
    return output


def build() -> Path:
    """设置 ESP32-C5 目标、编译、合并 0x0 完整镜像并返回最终发布包路径。"""
    config = load_board_config()
    target = config["target"]
    build_config = config["builds"][0]
    name = build_config["name"]

    run("idf.py", "set-target", target)
    append_sdkconfig(build_config.get("sdkconfig_append", []))
    run("idf.py", f"-DBOARD_NAME={name}", f"-DBOARD_TYPE={BOARD}", "build")
    run("idf.py", "merge-bin", "-o", "build/tuntun-binary.bin")
    return package_firmware(name)


def main() -> None:
    """解析命令行；支持列出唯一板型，或执行完整发布构建。"""
    parser = argparse.ArgumentParser(description="Build Tuntun Moji2 ESP32-C5 firmware")
    parser.add_argument("board", nargs="?", default=BOARD)
    parser.add_argument("--list-boards", action="store_true")
    args = parser.parse_args()

    if args.list_boards:
        print(BOARD)
        return
    if args.board != BOARD:
        parser.error(f"only {BOARD} is supported")

    try:
        output = build()
    except RuntimeError as error:
        parser.error(str(error))
    print(f"Firmware package: {output}")


if __name__ == "__main__":
    main()
