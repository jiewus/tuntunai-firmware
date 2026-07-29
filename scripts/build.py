#!/usr/bin/env python3
"""Run an ESP-IDF development action in an isolated per-board build directory."""

import argparse
import shutil
import subprocess
from pathlib import Path

from board_config import BOARD_ROOT, load_board
from release import DEFAULT_BOARD, find_idf_py


PROJECT_ROOT = Path(__file__).resolve().parent.parent
SUPPORTED_ACTIONS = (
    "build",
    "reconfigure",
    "menuconfig",
    "fullclean",
    "flash",
    "monitor",
    "flash-monitor",
)


def available_boards() -> list[str]:
    boards: list[str] = []
    for config_path in sorted(BOARD_ROOT.glob("*/config.json")):
        board = config_path.parent.name
        load_board(board)
        boards.append(board)
    return boards


def run_action(board: str, action: str, port: str | None) -> None:
    board_dir, _ = load_board(board)
    build_dir = PROJECT_ROOT / "build" / board / "debug"
    if action == "fullclean" and build_dir.exists() and not (build_dir / "CMakeCache.txt").is_file():
        # idf.py refuses to clean an incomplete configure directory. The path is
        # derived from a validated board name and is always scoped below build/.
        shutil.rmtree(build_dir)
        return
    sdkconfig_defaults = ";".join(
        [str(PROJECT_ROOT / "sdkconfig.defaults"), str(board_dir / "sdkconfig.defaults")]
    )
    command = [
        find_idf_py(),
        "-B",
        str(build_dir),
        f"-DBOARD_TYPE={board}",
        f"-DSDKCONFIG={build_dir / 'sdkconfig'}",
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
    ]
    if port:
        command.extend(["-p", port])
    command.extend(("flash", "monitor") if action == "flash-monitor" else (action,))
    subprocess.run(command, cwd=PROJECT_ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board", nargs="?", default=DEFAULT_BOARD)
    parser.add_argument("action", nargs="?", choices=SUPPORTED_ACTIONS, default="build")
    parser.add_argument("-p", "--port")
    parser.add_argument("--list-boards", action="store_true")
    args = parser.parse_args()

    try:
        boards = available_boards()
        if args.list_boards:
            print("\n".join(boards))
            return
        if args.board not in boards:
            parser.error(f"unknown board '{args.board}'; available: {', '.join(boards)}")
        run_action(args.board, args.action, args.port)
    except (RuntimeError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
