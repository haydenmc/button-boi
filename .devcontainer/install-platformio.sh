#!/usr/bin/env bash
# Installs (or upgrades) PlatformIO Core into ~/.platformio/penv using the
# official installer -- the same location the VS Code PlatformIO IDE extension
# uses -- and links the CLI into ~/.local/bin. ~/.platformio is a named volume,
# so toolchains and frameworks survive container rebuilds.
set -euo pipefail

installer="$(mktemp -d)/get-platformio.py"
curl -fsSL -o "$installer" \
  https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
python3 "$installer"
rm -f "$installer"

mkdir -p "$HOME/.local/bin"
for bin in pio platformio piodebuggdb; do
  ln -sf "$HOME/.platformio/penv/bin/$bin" "$HOME/.local/bin/$bin"
done

# Pre-fetch the platforms this project uses. The compiler/framework packages
# are pulled on the first build of an env that needs them.
for platform in ${PIO_PREINSTALL_PLATFORMS:-}; do
  "$HOME/.local/bin/pio" pkg install --global --platform "$platform"
done

"$HOME/.local/bin/pio" --version
