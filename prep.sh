#!/usr/bin/env bash
set -euo pipefail
# Install only missing packages. Run this as your regular desktop user.
command -v apt-get >/dev/null || { echo 'This script supports Linux Mint / Ubuntu / Debian.' >&2; exit 1; }
packages=(build-essential cmake pkg-config libgtk-3-dev libsensors-dev lm-sensors libayatana-appindicator3-dev)
case "${1:-}" in
  "") ;;
  --packaging) packages+=(dpkg-dev desktop-file-utils curl wget file patchelf librsvg2-bin) ;;
  *) echo 'Usage: bash prep.sh [--packaging]' >&2; exit 2 ;;
esac
missing=()
for package in "${packages[@]}"; do
  if ! dpkg-query -W -f='${Status}\n' "$package" 2>/dev/null | grep -qx 'install ok installed'; then
    missing+=("$package")
  fi
done
if ((${#missing[@]})); then
  if ((EUID == 0)); then admin=(); else admin=(sudo); fi
  "${admin[@]}" apt-get update
  "${admin[@]}" apt-get install -- "${missing[@]}"
else
  echo 'All build dependencies are installed.'
fi
echo 'Ready. Run ./build.sh and then ./build/mjd-psu-monitor'
