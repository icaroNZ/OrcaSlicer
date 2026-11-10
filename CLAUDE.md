# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Snapmaker_Orca is an open-source 3D slicer application forked from Bambu Studio, built using C++ with wxWidgets for the GUI and CMake as the build system. The project uses a modular architecture with separate libraries for core slicing functionality, GUI components, and platform-specific code.

## Build Commands

### Building on Windows
```bash
# Build everything
build_release_vs2022.bat

# Build with debug symbols
build_release_vs2022.bat debug

# Build only dependencies
build_release_vs2022.bat deps

# Build only slicer (after deps are built)
build_release_vs2022.bat slicer


```

### Building on macOS
```bash
# Build everything (dependencies and slicer)
./build_release_macos.sh

# Build only dependencies
./build_release_macos.sh -d

# Build only slicer (after deps are built)
./build_release_macos.sh -s

# Use Ninja generator for faster builds
./build_release_macos.sh -x

# Build for specific architecture
./build_release_macos.sh -a arm64    # or x86_64 or universal

# Build for specific macOS version target
./build_release_macos.sh -t 11.3
```

### Building on Linux
```bash
# First time setup - install system dependencies
./build_linux.sh -u

# Build dependencies and slicer
./build_linux.sh -dsi

# Build everything (alternative)
./build_linux.sh -dsi

# Individual options:
./build_linux.sh -d    # dependencies only
./build_linux.sh -s    # slicer only  
./build_linux.sh -i    # build AppImage

# Performance and debug options:
./build_linux.sh -j N  # limit to N cores
./build_linux.sh -1    # single core build
./build_linux.sh -b    # debug build
./build_linux.sh -c    # clean build
./build_linux.sh -r    # skip RAM/disk checks
./build_linux.sh -l    # use Clang instead of GCC
```

### Build System
- Uses CMake with minimum version 3.13 (maximum 3.31.x on Windows)
- Primary build directory: `build/`
- Dependencies are built in `deps/build/`
- The build process is split into dependency building and main application building
- Windows builds use Visual Studio generators
- macOS builds use Xcode by default, Ninja with -x flag
- Linux builds use Ninja generator

### Testing
Tests are located in the `tests/` directory and use the Catch2 testing framework. Test structure:
- `tests/libslic3r/` - Core library tests (21 test files)
  - Geometry processing, algorithms, file formats (STL, 3MF, AMF)
  - Polygon operations, clipper utilities, Voronoi diagrams
- `tests/fff_print/` - Fused Filament Fabrication tests (12 test files)
  - Slicing algorithms, G-code generation, print mechanics
  - Fill patterns, extrusion, support material
- `tests/sla_print/` - Stereolithography tests (4 test files)
  - SLA-specific printing algorithms, support generation
- `tests/libnest2d/` - 2D nesting algorithm tests
- `tests/slic3rutils/` - Utility function tests
- `tests/sandboxes/` - Experimental/sandbox test code

Run all tests after building:
```bash
cd build && ctest
```

Run tests with verbose output:
```bash
cd build && ctest --output-on-failure
```

Run individual test suites:
```bash
# From build directory
./tests/libslic3r/libslic3r_tests
./tests/fff_print/fff_print_tests
./tests/sla_print/sla_print_tests
```

## Architecture

### Core Libraries
- **libslic3r/**: Core slicing engine and algorithms (platform-independent)
  - Main slicing logic, geometry processing, G-code generation
  - Key classes: Print, PrintObject, Layer, GCode, Config
  - Modular design with specialized subdirectories:
    - `GCode/` - G-code generation, cooling, pressure equalization, thumbnails
    - `Fill/` - Infill pattern implementations (gyroid, honeycomb, lightning, etc.)
    - `Support/` - Tree supports and traditional support generation
    - `Geometry/` - Advanced geometry operations, Voronoi diagrams, medial axis
    - `Format/` - File I/O for 3MF, AMF, STL, OBJ, STEP formats
    - `SLA/` - SLA-specific print processing and support generation
    - `Arachne/` - Advanced wall generation using skeletal trapezoidation

- **src/slic3r/**: Main application framework and GUI
  - GUI application built with wxWidgets
  - Integration between libslic3r core and user interface
  - Located in `src/slic3r/GUI/` (not shown in this directory but exists)

### Key Algorithmic Components
- **Arachne Wall Generation**: Variable-width perimeter generation using skeletal trapezoidation
- **Tree Supports**: Organic support generation algorithm  
- **Lightning Infill**: Sparse infill optimization for internal structures
- **Adaptive Slicing**: Variable layer height based on geometry
- **Multi-material**: Multi-extruder and soluble support processing
- **G-code Post-processing**: Cooling, fan control, pressure advance, conflict checking

### File Format Support
- **3MF/BBS_3MF**: Native format with extensions for multi-material and metadata
- **STL**: Standard tessellation language for 3D models
- **AMF**: Additive Manufacturing Format with color/material support  
- **OBJ**: Wavefront OBJ with material definitions
- **STEP**: CAD format support for precise geometry
- **G-code**: Output format with extensive post-processing capabilities

### External Dependencies
- **Clipper2**: Advanced 2D polygon clipping and offsetting
- **libigl**: Computational geometry library for mesh operations
- **TBB**: Intel Threading Building Blocks for parallelization
- **wxWidgets**: Cross-platform GUI framework
- **OpenGL**: 3D graphics rendering and visualization
- **CGAL**: Computational Geometry Algorithms Library (selective use)
- **OpenVDB**: Volumetric data structures for advanced operations
- **Eigen**: Linear algebra library for mathematical operations

## File Organization

### Resources and Configuration
- `resources/profiles/` - Printer and material profiles organized by manufacturer
- `submodules/Profiles` - Git submodule; source of truth for Snapmaker `filament/`, `machine/`, `process/` presets and `Snapmaker.json`
- `resources/printers/` - Printer-specific configurations and G-code templates  
- `resources/images/` - UI icons, logos, calibration images
- `resources/calib/` - Calibration test patterns and data
- `resources/handy_models/` - Built-in test models (benchy, calibration cubes)

### Internationalization and Localization  
- `localization/i18n/` - Source translation files (.pot, .po)
- `resources/i18n/` - Runtime language resources
- Translation managed via `scripts/run_gettext.sh` / `scripts/run_gettext.bat`

### Platform-Specific Code
- `src/libslic3r/Platform.cpp` - Platform abstractions and utilities
- `src/libslic3r/MacUtils.mm` - macOS-specific utilities (Objective-C++)
- Windows-specific build scripts and configurations
- Linux distribution support scripts in `scripts/linux.d/`

### Build and Development Tools
- `cmake/modules/` - Custom CMake find modules and utilities
- `scripts/` - Python utilities for profile generation and validation  
- `tools/` - Windows build tools (gettext utilities)
- `deps/` - External dependency build configurations

## Development Workflow

### Code Style and Standards
- **C++17 standard** with selective C++20 features
- **Naming conventions**: PascalCase for classes, snake_case for functions/variables
- **Header guards**: Use `#pragma once` 
- **Memory management**: Prefer smart pointers, RAII patterns
- **Thread safety**: Use TBB for parallelization, be mindful of shared state

### Common Development Tasks

#### Adding New Print Settings
1. Define setting in `PrintConfig.cpp` with proper bounds and defaults
2. Add UI controls in appropriate GUI components  
3. Update serialization in config save/load
4. Add tooltips and help text for user guidance
5. Test with different printer profiles

#### Modifying Slicing Algorithms  
1. Core algorithms live in `libslic3r/` subdirectories
2. Performance-critical code should be profiled and optimized
3. Consider multi-threading implications (TBB integration)
4. Validate changes don't break existing profiles
5. Add regression tests where appropriate

#### GUI Development
1. GUI code resides in `src/slic3r/GUI/` (not visible in current tree)
2. Use existing wxWidgets patterns and custom controls
3. Support both light and dark themes
4. Consider DPI scaling on high-resolution displays
5. Maintain cross-platform compatibility

#### Adding Printer Support
1. Create JSON profile in `resources/profiles/[manufacturer].json`
2. Add printer-specific start/end G-code templates
3. Configure build volume, capabilities, and material compatibility
4. Test thoroughly with actual hardware when possible
5. Follow existing profile structure and naming conventions

### Dependencies and Build System
- **CMake-based** with separate dependency building phase
- **Dependencies** built once in `deps/build/`, then linked to main application  
- **Cross-platform** considerations important for all changes
- **Resource files** embedded at build time, platform-specific handling

### Performance Considerations
- **Slicing algorithms** are CPU-intensive, profile before optimizing
- **Memory usage** can be substantial with complex models
- **Multi-threading** extensively used via TBB
- **File I/O** optimized for large 3MF files with embedded textures
- **Real-time preview** requires efficient mesh processing

## Important Development Notes

### Codebase Navigation
- Use search tools extensively - codebase has 500k+ lines
- Key entry points: `src/Snapmaker_Orca.cpp` for application startup
- Core slicing: `libslic3r/Print.cpp` orchestrates the slicing pipeline
- Configuration: `PrintConfig.cpp` defines all print/printer/material settings

### Compatibility and Stability
- **Backward compatibility** maintained for project files and profiles
- **Cross-platform** support essential (Windows/macOS/Linux)  
- **File format** changes require careful version handling
- **Profile migrations** needed when settings change significantly

### Quality and Testing
- **Regression testing** important due to algorithm complexity
- **Performance benchmarks** help catch performance regressions
- **Memory leak** detection important for long-running GUI application
- **Cross-platform** testing required before releases
## Snapmaker 2.0 / A250T workspace notes

Added 07/11/2026 for the multi-repo workspace `/Users/icaro/Documents/Projects/SnapMaker` (20 Snapmaker repos, all on
branch `a250t/main`). The owner has a Snapmaker 2.0 **A250T** (single/dual extruder, laser, CNC) and uses Luban plus
Snapmaker Orca on macOS. Where this section disagrees with the generic text above, this section wins.
Workspace dependency map: `../.claude/knowledge/DEPENDENCIES.md`. Facts file: `../.claude/knowledge/repos/OrcaSlicer.json`.
Specialist agent: `sm-orca`. Background: `../_snapmaker2-review/repos/OrcaSlicer.md`, `../_snapmaker2-review/integration/OrcaSlicer.md`.

### Purpose and state
- Snapmaker Orca 2.4.1 (`version.inc`), a hard fork of OrcaSlicer ~2.3.1. It slices FDM jobs into Marlin2 G-code or
  `.gcode.3mf` and ships the Snapmaker vendor presets (U1, J1, Artisan, and the A250/A350 family) from the
  `submodules/Profiles` submodule. Upstream Snapmaker work is almost all for the U1.
- `a250t/main` = `main` at `4aacbb22c7` + 7 commits (`git log --oneline 4aacbb22c7..a250t/main`):
  - `d8faaf6b7a`, `d4a6f4fd9d`, `580c7283d7`: **"Snapmaker 2.0 (HTTP)" print host** (`htSnapmaker2`, config value `snapmaker2`).
  - `81490d1415`: by-object printing retracts and raises Z **before** the tool change (`GCode.cpp:3305-3321`).
  - `1dc3a3c343`, `ae32b2091b`, `d8d9f9321a`: Profiles submodule now uses `https://github.com/icaroNZ/snapmaker-profiles.git`
    on branch `a250t/main`, pinned at `0655774` (bundle `02.03.04.06+a250t`). `scripts/orca_extra_profile_check.py` skips the non-preset rule JSONs.
  - Later: CI fixes and merge-audit gaps (`../_snapmaker2-review/integration/OrcaSlicer-CI.md`), the SM2 bed levelling dialog
    (`ccc5c2815b`..`ba78f77ed0`), and the fix pass of 07/11/2026 (`ba78f77ed0..`, listed in `../_snapmaker2-review/issues/OrcaSlicer.md`).
- **Fix pass 07/11/2026 behaviour changes** (all uncompiled until CI):
  - A250/A350 G-code (`printer_model` "Snapmaker A250*/A350*") starts with a Luban-style `;Header Start` ... `;Header End` block
    (`GCode/SnapmakerHeader.hpp`, PR #1105): `file_total_lines` (20 digits, patched by `GCodeProcessor::run_post_process`), estimated time,
    bounds, temperatures, retraction and one `;thumbnail: data:image/png;base64,...` line. The touchscreen then skips its full-file parse.
  - Profiles gitlink now `2b71728` (bundle `02.03.04.07+a250t`): Z accel 100, Dual bed `max()` on tool change, flush cap, hot-bed offset at Z1.6.
  - Cherry-picked PRs: #1150 (final purge lifts), #1074 (bed temp ignores support filament without support), #962 (Add 3MF keeps modifier
    settings), #1139 (wipe tower categories), #1067 (z_offset after unknown position); hand-port of #1154 (close vetoed while a modal dialog runs).
  - Pure Z travels are capped at `machine_max_speed_z` for Marlin/Klipper/RRF (`GCodeWriter::_travel_to_z`).
  - SM2 host: Z jog text is locale independent; the card step locks its controls while a line runs and asks once before going 2 mm
    below the probed height (app config `[snapmaker2_bed_level] confirm_deep_jog`, default 1); Cancel works during the upload approval wait;
    the keep-alive polls 1.5 s from the previous request start (3 s timeout) and re-connects once on 401 during an upload.
- No A250T-specific preset exists (nor A150). The A250T uses the `Snapmaker A250` chain:
  `Snapmaker A250 (0.4 nozzle).json` → `fdm_a250` (230x250 bed, height 230, `PRINTER_MODEL_SNAPMAKER_A250`) → `fdm_linear2`
  → `fdm_common` (`gcode_flavor marlin2`). Dual: `fdm_a250_dual.json` (toolchange `T<n>` + `M109 T<n> S.. C3 W1`).

### Build and test on this Mac (verified 07/11/2026)
- **A full build is not possible here.** There is no `cmake` or `ninja`, only about 6.7 GB of disk is free, and the deps need more than 10 GB and several hours.
  Real builds run in GitHub Actions on `icaroNZ/OrcaSlicer` (`build_all.yml`, `test-linux.yml`, `check_profiles.yml`):
  `gh run list -R icaroNZ/OrcaSlicer -b a250t/main`.
- Recipe for CI or a bigger Mac: `./build_release_macos.sh -dx -a arm64 -t 11.3 -1` (deps), then `./build_release_macos.sh -s -n -x -a arm64 -t 11.3 -1`.
  Headless tests: `cmake -S . -B build_tests -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<deps prefix> -DSLIC3R_STATIC=ON
  -DOPENVDB_USE_STATIC_LIBS=ON -DSLIC3R_GUI=OFF -DBUILD_TESTS=ON && cmake --build build_tests --target snapmaker2_protocol_tests
  && ctest --test-dir build_tests --output-on-failure -R snapmaker2_protocol -E '\[trace\]'`.
- Checks you can run locally (full commands are in the workspace skill `orca-build-test`):
  - SM2 protocol unit test: compile `src/slic3r/Utils/Snapmaker2Protocol.cpp` and `tests/slic3rutils/Snapmaker2Protocol_test.cpp`
    with `clang++ -std=c++17 -Wall -Wextra -Werror` against Catch2 v3.7.1 from `../_snapmaker2-review/integration/tools/catch2-3.7.1/`.
    Expected result: `All tests passed (435 assertions in 30 test cases)` (08/11/2026, after O1) (the test uses `catch2/catch_approx.hpp`; the local Catch2 copy has a forwarding shim for it).
  - SM2 header unit test: `tests/libslic3r/test_snapmaker_header.cpp` only needs `src/libslic3r/GCode/SnapmakerHeader.hpp`; compile it the same way
    plus a shim dir whose `catch2/generators/catch_generators.hpp` includes `catch_amalgamated.hpp`. Expected: `All tests passed (63 assertions in 5 test cases)` (10/11/2026, after a250t F2; was 59/4).
  - Host syntax check: `clang++ -std=c++17 -fsyntax-only -I../_snapmaker2-review/integration/tools/orcaslicer-sm2-syntax-stubs -Isrc -isystem deps_src src/slic3r/Utils/Snapmaker2.cpp`.
    Expected: exit 0, with 54 unused-parameter warnings from upstream `PrintHost.hpp`.
  - Profiles: `python3 -I ../snapmaker-profiles/scripts/validate_profiles.py --root submodules/Profiles` should print `PASS (A250/A350 subset)`.
    `scripts/orca_extra_profile_check.py` must run on a scratch copy that has the profiles synced into it, and should report 0 errors.
- Old logs: `../_snapmaker2-review/integration/logs/OrcaSlicer-*.log`.

### Where the Snapmaker 2.0 code is
- `src/slic3r/Utils/Snapmaker2.{hpp,cpp}`: `PrintHost` subclass.
  - `open_session` POSTs `api/v1/connect` with `token=<t>`. `wait_for_approval` polls `api/v1/status` every 1.5 s, up to 60 s for Test and 120 s for upload;
    upload passes a cancel probe (progress_fn) so the queue's Cancel ends the wait.
  - The `KeepAlive` thread keeps the session open during uploads and interactive sessions, because the touchscreen drops a session after 5 s idle.
    It polls every 1.5 s measured from the previous request start (3 s request cap) and logs its longest gap when it stops.
  - `upload` refuses 3MF, a non-3DP head (headType other than 1 or 5) and a RUNNING/PAUSED machine. It then calls `prepare_print` (multipart `token`, `type=3DP`, `file`), optionally `start_print`, and finally `disconnect`.
  - `get_auth_info()` returns the approved token. `PhysicalPrinterDialog.cpp:197-211` stores it in `printhost_apikey`.
- `src/slic3r/Utils/Snapmaker2Protocol.{hpp,cpp}`: pure helpers with no wx, curl or boost dependency, and the only unit-tested part.
- `src/slic3r/GUI/Snapmaker2MonitorPanel.{hpp,cpp}` (O1, 08/11/2026, uncompiled until CI): Device tab for the SM2 host, shown by
  `PrinterWebView::show_snapmaker2_monitor` instead of the browser. Connect on click only; disconnects on tab change
  (`MainFrame` -> `PrinterWebView::on_page_hidden`), printer change, `[snapmaker2_monitor] idle_disconnect_min` (default 10) and hands the
  session to an upload (`Snapmaker2::upload_in_progress`); never reconnects by itself. Status from the keep-alive body, commands through
  `Snapmaker2::run_command` on a worker. `override_z_offset` is absolute (Orca sends the sum of its clicks). Stub check:
  `../_snapmaker2-review/integration/tools/orcaslicer-sm2-syntax-stubs/monitor-panel/check.sh <repo> <scratch>`.
- Registration: `PrintConfig.hpp:60` (enum), `PrintConfig.cpp:93` (key map), `PrintConfig.cpp:4068-4084` (enum value and label; **the index must equal the enum value**),
  `PrintHost.cpp:74` (factory), `src/slic3r/CMakeLists.txt:670-673` (sources), `tests/slic3rutils/CMakeLists.txt:1-13` (test target, defined before the GUI-only `return()`).
- Sending to the printer: use the **Plater connection button**, which opens `PhysicalPrinterDialog` (`Plater.cpp:2309-2310`). Then use the **"Print" (send G-code)** action (`eSendGcode`, `MainFrame.cpp:1882`).
  "Print plate" sends `.gcode.3mf`, which the host refuses.
  `SMPhysicalPrinterDialog`'s OK path (`:726-729`, `set_connect_host`) switches to the U1/Moonraker flow, so do not use it for the 2.0.
- The other connectivity code is U1-only. `ConnectionProcessManager` spawns `snapmaker_control_<os>_<arch> --locale=xx-YY --orca`, reads `PORT:<n>`, and then
  `Gateway*` talks to `127.0.0.1:<n>` (`/api/health`, `/ws`, `/api/cache/all`, ...). Other U1-only parts: `SSWCP` (web-view bridge, `sw_*` commands), `MoonRaker*`, `src/mqtt/`.
  If the binary is missing, slicing still works and only the device panel fails.

### Conventions seen in the code
- Logging uses `BOOST_LOG_TRIVIAL(trace|debug|info|warning|error)`.
  - The SM2 host prefixes every line with `"Snapmaker 2.0: "` and never logs a full token (`mask_token`). It truncates bodies with `proto::truncate` (300 characters).
  - U1 code uses tags such as `[FLUTTER_FLOW]` and `[Moonraker_Mqtt]`, plus `wcp_loger.add_log(...)`.
- User strings use `_L("...")`, with `GUI::format_wxstr(_L("... %1%"), x)` for arguments. Print hosts report errors by returning `bool` and filling a `wxString& msg` or calling `ErrorFn`; they do not throw.
- `#pragma once` or classic include guards. 4-space indent, 140 columns (`.clang-format`). Classes are PascalCase, functions and variables snake_case, members `m_*`.
  A Snapmaker change carries a comment that names its source (for example `Ported from kbaker827/OrcaSlicer@610069f89`).

### How to extend (common tasks)
- **New print host**: copy the `htSnapmaker2` wiring above. Append the enum before `htMoonRaker_mqtt`, keep the enum and `enum_values` indices identical,
  and put pure protocol code in a separate `*Protocol.cpp` with a Catch2 suite that is defined before the GUI-only `return()`. Workspace skill: `orca-add-print-host`.
- **A250/A350 preset change**: edit `../snapmaker-profiles` (branch `a250t/main`) and validate it, then (with the owner's OK) move the
  `submodules/Profiles` gitlink and commit it. Skill: `orca-bump-profiles-submodule`. **Never edit
  `resources/profiles/Snapmaker/{machine,process,filament}`.** `cmake/SyncSnapmakerProfiles.cmake` overwrites those folders and deletes extra files
  on every configure and build. It also fails if `Snapmaker.json` or any of the three `filament_*` rule files is missing (`:50-57`).
  Bed and cover assets (`resources/profiles/Snapmaker/Snapmaker A250_*.{stl,svg,png}`) are tracked in this repo.
- **Port a community fork commit**: 65 fork refs are already fetched under `refs/forks/<owner>/<branch>`. Reviews are in
  `../_snapmaker2-review/fork-reviews/OrcaSlicer/`. Check that a commit applies with `git merge-tree --write-tree --merge-base <c>^ HEAD <c>`. Skill: `orca-port-fork-commit`.
- **New setting**: use `PrintConfig.cpp` and the tab UI, as described in the upstream text above. Snapmaker flow-variant settings are at `PrintConfig.cpp:4098-4182` and `7379-7611`.

### Cross-repo interfaces (details in DEPENDENCIES.md / OrcaSlicer.json)
- `snapmaker-profiles` (sm-profiles) → `submodules/Profiles` bundle. `Orca_Presets` (sm-orca-presets) is the stale predecessor and is not used.
- `Snapmaker-SM2-Screen-App` (sm-screen): touchscreen HTTP server on port 8080 (`/api/v1/connect|status|prepare_print|start_print|disconnect`).
  There is also an OctoPrint-shaped `/api/files/local` there, which is untested from Orca.
- `Snapmaker2-Controller` (sm-controller) executes the G-code. Snapmaker-specific parts: `M109 C/W` in the Dual templates, and `T0/T1`.
- `snapmaker_control` (sm-control-daemon): the proprietary binary pinned in `.github/connect-bundle.lock.json` (v1.8.5), used for the U1 only.
- `snapmaker-version-center` (sm-version-center): `meta-cfg.snapmaker.com/upgrade/{orca,profile}/...` manifests (`AppConfig.cpp:40-63,1443-1466`).
- `Luban` (sm-luban): the SM2 protocol was modelled on Luban's `SstpHttpChannel.ts`. Luban also has an OctoPrint-shaped relay on port 5000 (`adaptor/Octo.ts`).
  `3mf-standards-wiki` (sm-3mf-wiki) documents the `.3mf`/`.gcode.3mf` files that `Format/bbs_3mf.cpp` writes.

### Do not
- Push (remotes: `origin` Snapmaker, `icaro` icaroNZ fork, `upstream` OrcaSlicer). Change remotes or `.gitmodules`. Move the
  `submodules/Profiles` gitlink, or commit inside the submodule, without the owner's explicit OK.
- Edit the generated profile copies (see above), `deps/`/`deps_src/` (vendored), or commit `build*/`, `deps/build/` or `resources/snapmaker_control_*`.
- Run tools that read every file in the tree. This is a blobless partial clone and each read can download blobs. Use `git grep` and `rg` on specific dirs.
- Reorder `PrintHostType` or `host_type` `enum_values`. The dialogs map the choice index to the enum value.
- Claim the SM2 host is proven. It has not yet been run against a real touchscreen. The test plan is in `../_snapmaker2-review/integration/OrcaSlicer.md`.
