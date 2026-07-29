#!/usr/bin/env python3
"""Validate TuntunLife board metadata and expose it to CMake/release tooling."""

import argparse
import json
import re
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
BOARD_ROOT = PROJECT_ROOT / "main" / "boards"
BOARD_NAME_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]*$")
TARGET_PATTERN = re.compile(r"^esp32[a-z0-9]*$")
REQUIRED_BOARD_FILES = (
    "CMakeLists.txt",
    "config.h",
    "idf_component.yml",
    "sdkconfig.defaults",
)


def fail(message: str) -> None:
    raise ValueError(message)


def require_string(config: dict, key: str) -> str:
    value = config.get(key)
    if not isinstance(value, str) or not value:
        fail(f"'{key}' must be a non-empty string")
    return value


def quote_cmake(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def load_board(board: str) -> tuple[Path, dict]:
    if not BOARD_NAME_PATTERN.fullmatch(board):
        fail("board name may only contain lowercase letters, digits, and hyphens")

    board_dir = BOARD_ROOT / board
    config_path = board_dir / "config.json"
    if not config_path.is_file():
        fail(f"board metadata not found: {config_path}")

    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        fail(f"invalid JSON in {config_path}: {error.msg}")

    if not isinstance(config, dict):
        fail("board metadata must be a JSON object")

    target = require_string(config, "target")
    if not TARGET_PATTERN.fullmatch(target):
        fail(f"invalid ESP-IDF target: {target}")

    require_string(config, "name")
    require_string(config, "builtin_text_font")
    require_string(config, "builtin_icon_font")
    require_string(config, "emoji_collection")
    component = require_string(config, "component")
    if not BOARD_NAME_PATTERN.fullmatch(component):
        fail("'component' must be an ESP-IDF component directory name")
    if component != board:
        fail("'component' must match the board directory name")

    device_model = config.get("device_model")
    if not isinstance(device_model, int) or isinstance(device_model, bool) or device_model <= 0:
        fail("'device_model' must be a positive integer registered by the backend")

    for filename in REQUIRED_BOARD_FILES:
        if not (board_dir / filename).is_file():
            fail(f"board is missing required file: {board_dir / filename}")

    assets_dir = board_dir / "assets"
    if not assets_dir.is_dir():
        fail(f"board is missing assets directory: {assets_dir}")

    partition = config.get("partition_table")
    if partition is not None:
        if not isinstance(partition, str) or not partition:
            fail("'partition_table' must be a non-empty relative path when provided")
        partition_path = PROJECT_ROOT / partition
        if not partition_path.is_file():
            fail(f"partition table not found: {partition_path}")

    return board_dir, config


def emit_cmake(board: str) -> None:
    board_dir, config = load_board(board)
    values = {
        "TUNTUN_BOARD_TYPE": board,
        "TUNTUN_BOARD_DIR": board_dir.as_posix(),
        "TUNTUN_BOARD_TARGET": config["target"],
        "TUNTUN_BOARD_NAME": config["name"],
        "TUNTUN_BOARD_COMPONENT": config["component"],
        "TUNTUN_BOARD_DEVICE_MODEL": str(config["device_model"]),
        "TUNTUN_BOARD_BUILTIN_TEXT_FONT": config["builtin_text_font"],
        "TUNTUN_BOARD_BUILTIN_ICON_FONT": config["builtin_icon_font"],
        "TUNTUN_BOARD_EMOJI_COLLECTION": config["emoji_collection"],
        "TUNTUN_BOARD_ASSETS_DIR": (board_dir / "assets").as_posix(),
        "TUNTUN_BOARD_SDKCONFIG_DEFAULTS": (board_dir / "sdkconfig.defaults").as_posix(),
    }
    if "partition_table" in config:
        values["TUNTUN_BOARD_PARTITION_TABLE"] = (PROJECT_ROOT / config["partition_table"]).as_posix()

    for key, value in values.items():
        print(f'set({key} "{quote_cmake(value)}")')


def list_boards() -> None:
    for config_path in sorted(BOARD_ROOT.glob("*/config.json")):
        board = config_path.parent.name
        try:
            _, config = load_board(board)
        except ValueError as error:
            fail(f"{board}: {error}")
        print(f"{board}\t{config['target']}\t{config['name']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    emit_parser = subparsers.add_parser("emit-cmake", help="print CMake variables for a board")
    emit_parser.add_argument("board")
    subparsers.add_parser("list", help="list valid board directories")
    args = parser.parse_args()

    try:
        if args.command == "emit-cmake":
            emit_cmake(args.board)
        else:
            list_boards()
    except ValueError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
