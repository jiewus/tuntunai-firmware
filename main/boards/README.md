# Board Porting Contract

Each directory directly below `main/boards/` is a self-contained hardware
adapter. Adding a board must not require changes to the project root, shared
`main` sources, or build/release scripts.

## Required files

    main/boards/<board>/
    ├── CMakeLists.txt
    ├── config.json
    ├── dependencies.lock       generated after the first configure
    ├── idf_component.yml
    ├── sdkconfig.defaults
    ├── config.h
    ├── assets/                  files added to the default assets partition
    └── <board implementation>.cc

The directory name uses lowercase letters, digits, and hyphens. It is also the
value passed to `BOARD_TYPE`.

`config.json` is the single metadata source used by CMake and Python tooling:

    {
      "target": "esp32s3",
      "name": "Example ESP32-S3 Board",
      "component": "example-esp32s3",
      "device_model": 2,
      "builtin_text_font": "font_puhui_basic_30_4",
      "builtin_icon_font": "font_awesome_20_4",
      "emoji_collection": "twemoji_64",
      "partition_table": "partitions/v2/16m.csv"
    }

- `target`: ESP-IDF chip target.
- `name`: user-visible board/SKU name.
- `component`: must equal the board directory name.
- `device_model`: positive backend device model number.
- `builtin_text_font`, `builtin_icon_font`, and `emoji_collection`: resources
  selected by this board.
- `partition_table`: optional project-relative partition table checked by the
  board metadata validator. Select it in the board's `sdkconfig.defaults`.

`CMakeLists.txt` registers all board implementation sources. Put LCD, Codec,
battery, touch, camera, or other hardware-specific sources here rather than in
the shared `main/CMakeLists.txt`. Register the component with
`WHOLE_ARCHIVE` so the board's `DECLARE_BOARD` factory is retained.
Files placed in `assets/` are included in that board's generated default
`assets.bin`; display artwork and compiled fonts should stay inside the board
directory.

`idf_component.yml` owns dependencies used only by that board. A board that
changes LCD or audio Codec can therefore replace those dependencies without
editing the shared component manifest. Shared interfaces exported by `main`
provide network, display, audio, backlight, and LED contracts. Concrete
LCD/LVGL, Codec, button, battery, backlight, and LED dependencies belong to the
board manifest.
The Component Manager writes `dependencies.lock` beside the manifest after the
first successful configure. Commit it so every target keeps its own resolved
dependency graph; it does not need to exist before the first configure.

`sdkconfig.defaults` owns chip, Flash, PSRAM, partition, wake-word, and
board-resource limits. Shared protocol/UI defaults remain in the project-level
`sdkconfig.defaults`.

The board implementation must derive from `Board` or `WifiBoard` and
register exactly one concrete type using `DECLARE_BOARD`.

## Commands

List discovered boards:

    python scripts/build.py --list-boards

Build a board in an isolated directory:

    python scripts/build.py <board> build

Direct ESP-IDF commands are also supported:

    idf.py -B build/<board>/debug -DBOARD_TYPE=<board> build

The target is read from `config.json`; do not run `idf.py set-target`
when using these per-board build directories.

## Adding a board

1. Copy an existing board directory with similar peripherals.
2. Replace `config.json`, GPIO definitions, component dependencies, and
   `sdkconfig.defaults`.
3. Implement the board factory and hardware initialization inside the new
   directory.
4. Register the new `device_model` in the backend if it is a distinct
   commercial model. This is a server-side model registration, not a firmware
   build-system change.
5. Run `python scripts/board_config.py list`, then build the new board.
