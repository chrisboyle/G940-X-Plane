# G940-X-Plane

Two X-Plane 11.20+/12 plugins for Logitech Flight System G940: **g940FF**
provides airspeed-dependent force feedback with trim and angle-of-attack spring
centers; **g940LEDs** displays aircraft state on the eight throttle button LEDs.

| Platform | Architecture | Force feedback | LEDs |
| --- | --- | --- | --- |
| Linux | x86-64 | evdev spring or constant-force fallback | G940 sysfs driver |
| Windows | x86-64 | Native G940 HID | Native G940 HID |
| macOS 11+ | Intel/Apple Silicon universal | Native G940 HID | Native G940 HID |

Both plugins build on all three platforms. macOS hardware and X-Plane 12 flight
operation are confirmed; Windows/Linux hardware validation remains pending.
Trim changes and the one-second pause release are smooth. Matched hands-off
centering holds the trimmed stick position; the latest flight still reported
some early grip-release kicks near center, disappearing later. The force model
remains a work in progress. See [hardware findings](tools/HARDWARE_TESTS.md)
and the [report protocol](tools/PROTOCOL.md).

## Build

Use GNU make, a C++20 compiler with `std::span`, curl, unzip and Python 3.8+.
On macOS install Xcode Command Line Tools (`xcode-select --install`); Linux
needs GCC/G++ and the Linux input headers. On Windows use the MSYS2 **UCRT64**
shell and install:

```sh
pacman -S --needed make curl unzip mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-python
```

```sh
make -j2
make test
```

Plain `make` selects `BUILD_TYPE=release`: optimized plugins without assertions,
debug information or force traces. Use `make BUILD_TYPE=debug` for an unoptimized
build with assertions and debug information, and pass the same `BUILD_TYPE`
to `make install` when installing it. CI packages only release plugins and
also checks that debug plugins compile on all three OSes.

The Makefile detects the host; `PLATFORM=mac|linux|windows` selects an explicit
target. The SDK calls its platform macros `APL` (macOS), `IBM` (Windows) and
`LIN` (Linux); the Makefile sets the selected macro to 1 and the others to 0.
`IBM` is a historical Windows platform name, not a CPU architecture selection.
Cross builds also require an appropriate compiler, for example:

```sh
make PLATFORM=linux CXX="zig c++ -target x86_64-linux-gnu.2.31" LDFLAGS=
make PLATFORM=windows CXX="zig c++ -target x86_64-windows-gnu" LDFLAGS=
```

The build downloads the [official X-Plane SDK](https://developer.x-plane.com/sdk/plugin-sdk-downloads/)
4.3.0 if missing, retaining the XPLM 3.0.1 API target. Use `SDK_DIR=/path/to/SDK`
or symlink `SDK` to an existing official SDK. macOS links the universal XPLM
framework, Windows its import library; Linux resolves SDK symbols in X-Plane.
No handwritten SDK replacements or XPLM stubs are linked.

Outputs are `build/{g940FF,g940LEDs}/64/{mac,win,lin}.xpl`; objects are separated
by OS, architecture and build type. Switching release/debug rebuilds or selects
the corresponding objects automatically. `MAC_ARCHS=arm64` or `MAC_ARCHS=x86_64`
selects one Mac architecture; plain `make` restores universal output. Run
`make clean` when changing compilers, SDK paths or flags. Clean preserves
`build/previous-sdk-stubs`.

## Install

GitHub Actions builds and tests all three OSes on pushes and pull requests.
Open a successful run on the repository's **Actions** tab and download
`G940-X-Plane-linux-x86_64.zip`, `G940-X-Plane-windows-x86_64.zip`, or
`G940-X-Plane-mac-universal.zip` from **Artifacts**. Each ZIP contains both
plugins under `Resources/plugins/`, plus a license and installation instructions.
With X-Plane closed, back up existing G940 plugins, then merge the archive's
`Resources` folder into your X-Plane installation. The Mac build contains both
Intel and Apple Silicon binaries. Linux device permissions and LED driver
requirements below still apply.

For installation from source, close X-Plane, then run:

```sh
make install XP_INSTALL_PATH="/path/to/X-Plane 12"
```

Windows paths such as `C:/X-Plane 12` work from UCRT64. Both plugins are copied
to `Resources/plugins/<plugin>/64/`, backing up existing binaries and retaining
other platforms' binaries. Without an explicit path the installer searches
X-Plane 12 hints, then 11 hints, skips stale paths and accepts multiple lines:

| Platform | Primary hint directory |
| --- | --- |
| macOS | `~/Library/Preferences/` |
| Linux | `~/.x-plane/` |
| Windows | `%LOCALAPPDATA%/` |

Hint filenames are `x-plane_install_12.txt` and `x-plane_install_11.txt`; home
folder hints are also accepted. Use `XP_INSTALL_PATH` for multiple valid
installations or `HINTFILE=/path/to/hint.txt` for a custom hint. Load a flight
and check `Log.txt` for both loaded plugins and `G940 FF: force-feedback device
connected` / `G940 LEDs: LED device connected`. Device failures retry after
five seconds. Disable/re-enable through Plugin Admin to check cleanup.

Normal macOS use and the native HID probe require **no administrator password**.
Avoid another application writing G940 reports concurrently. Linux force
feedback needs read/write access to `/dev/input/event*`; LEDs need the original
[G940 kernel patches](https://github.com/chrisboyle/G940-linux) or equivalent
`/sys/class/leds/g940:*` driver support and write permission.

## Test and diagnose

`make test` checks packet encoding, trim filtering, invalid inputs, pause fades,
dataref types, lifecycle/reconnection, native backup/rollback/restoration and
installation without moving hardware. Only the test runners keep assertions
enabled when testing release builds. CI runs these checks and validates the
installable ZIP layout on all three OSes. Create the same archive locally with:

```sh
python3 -m tools.package --platform mac --output build/dist/G940-X-Plane-mac-universal.zip
```

Use `--platform linux` or `--platform windows` for those builds.
For optional force traces every two seconds, use a separate build directory:

```sh
make BUILD_TYPE=debug BUILDDIR=build/force-debug CPPFLAGS=-DG940_DEBUG_FORCE=1
make install BUILD_TYPE=debug BUILDDIR=build/force-debug CPPFLAGS=-DG940_DEBUG_FORCE=1 XP_INSTALL_PATH="/path/to/X-Plane 12"
```

Traces include airspeed/Vne, yoke inputs, trim, AoA, centers, saturation and
pause scale. Installing the ordinary `build` output removes diagnostic logging.

With X-Plane closed, `make probe` on macOS/Windows only reads hardware. Optional
comparisons warn and wait for Enter; force stages start after two seconds of
grip coverage and stop on release. Motor power must be connected:

```sh
build/tools/g940_probe --led-test --seconds 10
build/tools/g940_probe --force-test --seconds 10
build/tools/g940_probe --pitch-test --seconds 5 --magnitude 16000
build/tools/g940_probe --pitch-test --reverse --seconds 10
```

`--roll-test` selects sideways constant force. Duration is 1–30 seconds;
constant magnitude is 1–16384 (default 4000). `--force-test` uses a centered
spring at 10% of each axis cap, independent of flight gains. Both include an
equal-duration zero-force stage. Low-level forces may be imperceptible; USB
success alone does not prove physical effect. LEDs display red/green/amber/off
and restore their original state. Never test forces without a preparation warning.

The fixed LED mapping is P1/P5 speed brakes, P2/P6 flaps, P3 carburetor heat,
P4 autopilot, P7 landing lights, P8 gear; absent equipment shows off. Force is
centered gently on the ground, ramps with airspeed, fades on pause in both
native grip modes and restores original idle settings on normal disable/exit.
Invalid airspeed or Vne stops immediately. For load/access failures inspect
`Log.txt`; `lipo -archs build/g940FF/64/mac.xpl` checks Mac architectures.
