#!/usr/bin/env bash
# BootAz command line (Linux). Builds on first use. Without arguments it starts an interactive
# wizard; "help" lists every command. Writing to drives needs sudo:
#   ./run_cli.sh devices
#   sudo ./run_cli.sh write --device /dev/sdX --image file.iso
set -e
cd "$(dirname "$(readlink -f "$0")")"
BUILD=build
if [ ! -x "$BUILD/bootaz-cli" ] || [ -n "$(find core cli CMakeLists.txt -newer "$BUILD/bootaz-cli" -print -quit 2>/dev/null)" ]; then
    if [ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ]; then
        sudo -u "$SUDO_USER" cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
        sudo -u "$SUDO_USER" cmake --build "$BUILD" --target bootaz-cli -j"$(nproc)"
    else
        cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$BUILD" --target bootaz-cli -j"$(nproc)"
    fi
fi
exec "$BUILD/bootaz-cli" "$@"
