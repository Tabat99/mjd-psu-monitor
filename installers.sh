#!/usr/bin/env bash
# Build packages as your regular user; never installs the generated packages.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
project_dir="$PWD"
mode="${1:-all}"
case "$mode" in all|deb|appimage) ;; *) echo 'Usage: bash installers.sh [all|deb|appimage]' >&2; exit 2;; esac
for tool in cmake pkg-config desktop-file-validate; do
  command -v "$tool" >/dev/null || { echo "Missing $tool. Run: bash prep.sh --packaging" >&2; exit 1; }
done
version="$(tr -d '\r\n' < version.ini)"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid version.ini' >&2; exit 1; }
if [[ "$mode" != appimage ]]; then
  for tool in dpkg-deb dpkg-shlibdeps dpkg-architecture; do
    command -v "$tool" >/dev/null || { echo "Missing $tool. Run: bash prep.sh --packaging" >&2; exit 1; }
  done
fi
if [[ "$mode" != deb ]]; then
  for tool in curl wget file patchelf; do
    command -v "$tool" >/dev/null || { echo "Missing $tool. Run: bash prep.sh --packaging" >&2; exit 1; }
  done
  case "$(uname -m)" in x86_64) image_arch=x86_64;; aarch64) image_arch=aarch64;; *) echo 'AppImage supports x86_64 or aarch64 in this script.' >&2; exit 1;; esac
fi
desktop-file-validate packaging/mjd-psu-monitor.desktop
bash ./build.sh
mkdir -p dist
stage="$(mktemp -d "$project_dir/.package-stage.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT

if [[ "$mode" != appimage ]]; then
  deb_root="$stage/deb"
  mkdir -p "$deb_root/DEBIAN" "$stage/debian"
  DESTDIR="$deb_root" cmake --install build --prefix /usr
  # dpkg-shlibdeps uses a minimal source control file while scanning the ELF.
  cat > "$stage/debian/control" <<CONTROL
Source: mjd-psu-monitor
Section: utils
Priority: optional
Maintainer: mjd

Package: mjd-psu-monitor
Architecture: any
Description: Independent monitor for compatible Corsair power supplies
CONTROL
  dependency_line="$(cd "$stage" && dpkg-shlibdeps -O -e"$deb_root/usr/bin/mjd-psu-monitor")"
  dependencies="$(printf '%s\n' "$dependency_line" | sed -n 's/^shlibs:Depends=//p')"
  [[ -n "$dependencies" ]] || { echo 'Failed to calculate runtime dependencies.' >&2; exit 1; }
  deb_arch="$(dpkg-architecture -qDEB_HOST_ARCH)"
  cat > "$deb_root/DEBIAN/control" <<CONTROL
Package: mjd-psu-monitor
Version: $version
Section: utils
Priority: optional
Architecture: $deb_arch
Maintainer: mjd
Depends: $dependencies
Recommends: lm-sensors
Description: Independent monitor for compatible Corsair power supplies
 Displays power, voltage, current, temperatures and fan readings.
 Includes tray monitoring, configurable alarms and USB telemetry detection.
 Not developed, sponsored, endorsed by, or affiliated with CORSAIR.
CONTROL
  dpkg-deb --root-owner-group --build "$deb_root" "dist/mjd-psu-monitor_${version}_${deb_arch}.deb"
  dpkg-deb --info "dist/mjd-psu-monitor_${version}_${deb_arch}.deb"
fi

if [[ "$mode" != deb ]]; then
  # Tools are cached outside build/, which build.sh deliberately removes.
  tools_dir="$project_dir/.packaging-tools"
  mkdir -p "$tools_dir"
  fetch() {
    local url="$1" target="$2"
    if [[ ! -s "$target" ]]; then
      curl --fail --location --retry 3 --proto '=https' "$url" -o "$target.tmp"
      chmod +x "$target.tmp"
      mv -- "$target.tmp" "$target"
    fi
  }
  fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${image_arch}.AppImage" "$tools_dir/linuxdeploy.AppImage"
  fetch 'https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh' "$tools_dir/linuxdeploy-plugin-gtk.sh"
  # Some upstream GTK plugin versions force X11 in their generated runtime hook.
  # Remove those explicit assignments so GTK can select Wayland or X11 normally.
  sed -i \
    -e 's/export GDK_BACKEND=x11/# Display backend selected by GTK/g' \
    -e 's/export GDK_BACKEND="x11"/# Display backend selected by GTK/g' \
    "$tools_dir/linuxdeploy-plugin-gtk.sh"
  appdir="$stage/MJD-PSU-Monitor.AppDir"
  mkdir -p "$appdir"
  DESTDIR="$appdir" cmake --install build --prefix /usr
  # The GTK plugin supplies GTK resources/modules as well as linked libraries.
  # Extraction mode avoids needing FUSE during packaging.
  (
    cd "$stage"
    export PATH="$tools_dir:$PATH"
    export APPIMAGE_EXTRACT_AND_RUN=1
    export DEPLOY_GTK_VERSION=3
    export ARCH="$image_arch"
    export OUTPUT="$project_dir/dist/MJD-PSU-Monitor-${version}-${image_arch}.AppImage"
    "$tools_dir/linuxdeploy.AppImage" --appimage-extract-and-run \
      --appdir "$appdir" \
      --desktop-file "$project_dir/packaging/mjd-psu-monitor.desktop" \
      --icon-file "$project_dir/packaging/mjd-psu-monitor.svg" \
      --plugin gtk --output appimage
  )
fi
printf '\nPackages created in %s/dist\n' "$project_dir"
