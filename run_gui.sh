#!/usr/bin/env bash
# BootAz desktop app (Linux). Builds the project on first use, adds BootAz to the application
# menu, then starts it. Writing to drives asks for your password (pkexec) when a job starts.
set -e
cd "$(dirname "$(readlink -f "$0")")"
BUILD=build
if [ ! -x "$BUILD/BootAz" ] || [ ! -x "$BUILD/bootaz-cli" ] || [ -n "$(find core cli gui resources CMakeLists.txt -newer "$BUILD/BootAz" -print -quit 2>/dev/null)" ]; then
    echo "BootAz: building (first start or sources changed)…"
    cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD" -j"$(nproc)"
fi

# Application menu entry and icon (once, for the current user).
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
if [ ! -f "$apps/bootaz.desktop" ] || ! grep -q "$PWD" "$apps/bootaz.desktop" 2>/dev/null; then
    mkdir -p "$apps" "$icons"
    cp resources/bootaz-256.png "$icons/bootaz.png"
    sed "s|@DIR@|$PWD|g" resources/bootaz.desktop > "$apps/bootaz.desktop"
    command -v update-desktop-database >/dev/null && update-desktop-database "$apps" >/dev/null 2>&1 || true
fi

exec "$BUILD/BootAz" "$@"
