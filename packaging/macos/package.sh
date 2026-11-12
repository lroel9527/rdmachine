#!/bin/bash
# Packages the macOS build into signed app bundles:
#
#   dist/RDMachine.app           the client (rdmachine-client)
#   dist/RDMachine Server.app    the server (rdmachine-server), a menu bar app
#   dist/rdmachine-server.sh     starts the server app from Terminal
#   dist/rdmachine-connect       command-line test client
#   dist/rdmachine-service       runs the server as a service: login window,
#                                start at login, restart by launchd (sudo)
#
#   packaging/macos/package.sh [--build-dir DIR] [--out DIR] [--identity NAME]
#                              [--keychain PATH] [--build] [--zip] [--dmg] [--pkg]
#
#   --build-dir  CMake build directory with bin/ (default: build)
#   --out        output directory, replaced on each run (default: dist)
#   --identity   code signing identity (default: "rdmachine Local Signing",
#                made by create-signing-identity.sh; also takes a SHA-1). "-"
#                signs ad hoc: fine for a quick test, but macOS then asks for
#                the permissions again after every rebuild.
#   --keychain   keychain holding the identity (default: the search list)
#   --build      configure and build a universal Release build first
#   --zip        also write dist/rdmachine-VERSION-macos.zip
#   --dmg        also write dist/RDMachine-VERSION.dmg: both apps next to an
#                Applications shortcut (drag to install), the command-line
#                tools in a folder of their own; signed like the apps
#   --pkg        also write dist/RDMachine-VERSION.pkg, the installer: the
#                server (app, daemon and agents, started at once: what
#                rdmachine-service install does) and the client, each a
#                choice; unsigned (that needs a Developer ID Installer
#                certificate), the apps inside keep their signatures
#
# Bundle identifiers are fixed (org.rdmachine.client, org.rdmachine.server).
# Together with the fixed certificate they are what macOS remembers the
# permissions by, so do not change them casually.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
pkg="$root/packaging/macos"
build_dir=build
out=dist
identity="rdmachine Local Signing"
keychain=""
do_build=0
do_zip=0
do_dmg=0
do_pkg=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) build_dir=${2:?}; shift 2 ;;
    --build-dir=*) build_dir=${1#*=}; shift ;;
    --out) out=${2:?}; shift 2 ;;
    --out=*) out=${1#*=}; shift ;;
    --identity) identity=${2:?}; shift 2 ;;
    --identity=*) identity=${1#*=}; shift ;;
    --keychain) keychain=${2:?}; shift 2 ;;
    --keychain=*) keychain=${1#*=}; shift ;;
    --build) do_build=1; shift ;;
    --zip) do_zip=1; shift ;;
    --dmg) do_dmg=1; shift ;;
    --pkg) do_pkg=1; shift ;;
    -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

[ "$(uname -s)" = Darwin ] || { echo "this script runs on macOS" >&2; exit 1; }
cd "$root"

version=$(sed -n 's/^project(rdmachine VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
min_macos=$(sed -n 's/.*set(CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9.]*\)".*/\1/p' CMakeLists.txt)
[ -n "$version" ] && [ -n "$min_macos" ] || { echo "could not read the version from CMakeLists.txt" >&2; exit 1; }
# CFBundleVersion must grow for macOS to treat a build as newer; the commit
# count does that without bookkeeping.
build_no=$(git rev-list --count HEAD 2>/dev/null || echo 1)

if [ "$do_build" = 1 ]; then
  cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DRDM_BUILD_TESTS=OFF
  cmake --build "$build_dir" -j "$(sysctl -n hw.ncpu)"
fi
bin="$build_dir/bin"
for b in rdmachine-server rdmachine-client rdmachine-connect; do
  [ -x "$bin/$b" ] || { echo "$bin/$b not found: build first, or pass --build" >&2; exit 1; }
done

sign_args=(--force --sign "$identity" --timestamp=none)
[ -n "$keychain" ] && sign_args+=(--keychain "$keychain")
if [ "$identity" = "-" ]; then
  echo "warning: signing ad hoc; macOS will ask for Screen Recording and Accessibility again after each rebuild" >&2
elif ! security find-identity -v -p codesigning ${keychain:+"$keychain"} | grep -qF -- "$identity"; then
  echo "code signing identity \"$identity\" not found." >&2
  echo "Create it once with packaging/macos/create-signing-identity.sh, or pass --identity -" >&2
  exit 1
fi

# make_app NAME.app EXECUTABLE BUNDLE_ID PLIST_TEMPLATE [RESOURCE...]
make_app() {
  local app="$out/$1" exe=$2 id=$3 plist=$4
  shift 4
  mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
  ditto "$bin/$exe" "$app/Contents/MacOS/$exe"
  local r
  for r in "$@"; do
    install -m 755 "$r" "$app/Contents/Resources/$(basename "$r")"
  done
  sed -e "s/@BUNDLE_ID@/$id/" -e "s/@VERSION@/$version/" -e "s/@BUILD@/$build_no/" \
      -e "s/@MIN_MACOS@/$min_macos/" "$pkg/$plist" >"$app/Contents/Info.plist"
  printf 'APPL????' >"$app/Contents/PkgInfo"
  plutil -lint -s "$app/Contents/Info.plist"
  # The bundle identifier comes from Info.plist; the designated requirement
  # codesign derives from it and the certificate is what TCC checks.
  codesign "${sign_args[@]}" "$app"
  codesign --verify --strict --verbose=1 "$app"
}

rm -rf "$out"
mkdir -p "$out"
make_app "RDMachine.app" rdmachine-client org.rdmachine.client client-Info.plist.in
# rdmachine-service travels inside the server app (the installer runs it, and
# it is there for status, stop and uninstall).
make_app "RDMachine Server.app" rdmachine-server org.rdmachine.server server-Info.plist.in "$pkg/rdmachine-service"

ditto "$bin/rdmachine-connect" "$out/rdmachine-connect"
codesign "${sign_args[@]}" --identifier org.rdmachine.connect "$out/rdmachine-connect"
install -m 755 "$pkg/rdmachine-server.sh" "$out/rdmachine-server.sh"
install -m 755 "$pkg/rdmachine-service" "$out/rdmachine-service"

echo
for app in "$out/RDMachine.app" "$out/RDMachine Server.app"; do
  echo "$app"
  lipo -info "$app/Contents/MacOS/"* | sed 's/^/  /'
  codesign -d -r- "$app" 2>&1 | sed -n 's/^designated => /  requirement: /p'
done

if [ "$do_zip" = 1 ]; then
  # One top-level folder in the zip, so unpacking it does not scatter files.
  zip="$out/rdmachine-$version-macos.zip"
  stage=$(mktemp -d)
  mkdir "$stage/rdmachine-$version"
  for f in "RDMachine.app" "RDMachine Server.app" rdmachine-server.sh rdmachine-connect rdmachine-service; do
    ditto "$out/$f" "$stage/rdmachine-$version/$f"
  done
  ditto -c -k --sequesterRsrc --keepParent "$stage/rdmachine-$version" "$zip"
  rm -rf "$stage"
  echo "$zip"
fi

if [ "$do_dmg" = 1 ]; then
  dmg="$out/RDMachine-$version.dmg"
  stage=$(mktemp -d)
  ditto "$out/RDMachine.app" "$stage/RDMachine.app"
  ditto "$out/RDMachine Server.app" "$stage/RDMachine Server.app"
  ln -s /Applications "$stage/Applications"
  # rdmachine-server.sh finds the server app in /Applications once it is
  # installed, so the tools need not sit next to it.
  mkdir "$stage/Command Line Tools"
  ditto "$out/rdmachine-server.sh" "$stage/Command Line Tools/rdmachine-server.sh"
  ditto "$out/rdmachine-connect" "$stage/Command Line Tools/rdmachine-connect"
  ditto "$out/rdmachine-service" "$stage/Command Line Tools/rdmachine-service"
  hdiutil create -quiet -volname "RDMachine $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg"
  rm -rf "$stage"
  codesign "${sign_args[@]}" "$dmg"
  hdiutil verify -quiet "$dmg"
  echo "$dmg"
fi

if [ "$do_pkg" = 1 ]; then
  installer_pkg="$out/RDMachine-$version.pkg"
  stage=$(mktemp -d)
  # component PKG_ID APP [SCRIPTS_DIR]: one app installed to /Applications,
  # always there (not relocated to some other copy of the app on the disk).
  component() {
    local id=$1 app=$2 scripts=${3:-}
    mkdir -p "$stage/root-$id"
    # Only the signed bundle, no quarantine or resource forks. (macOS tags
    # every new file with com.apple.provenance, which cannot be removed;
    # pkgbuild lists it as ._ entries that become attributes again on
    # install, not files, and are not part of the signature.)
    ditto --norsrc --noextattr --noqtn "$out/$app" "$stage/root-$id/$app"
    codesign --verify --strict "$stage/root-$id/$app"
    pkgbuild --analyze --root "$stage/root-$id" "$stage/$id.plist" >/dev/null
    plutil -replace 0.BundleIsRelocatable -bool NO "$stage/$id.plist"
    plutil -replace 0.BundleOverwriteAction -string upgrade "$stage/$id.plist"
    pkgbuild --quiet --root "$stage/root-$id" --component-plist "$stage/$id.plist" \
      --identifier "$id" --version "$version" --install-location /Applications \
      ${scripts:+--scripts "$scripts"} "$stage/$id.pkg"
  }
  component org.rdmachine.server.pkg "RDMachine Server.app" "$pkg/pkg/server-scripts"
  component org.rdmachine.client.pkg "RDMachine.app"
  sed -e "s/@VERSION@/$version/g" -e "s/@MIN_MACOS@/$min_macos/g" "$pkg/pkg/distribution.xml.in" >"$stage/distribution.xml"
  productbuild --quiet --distribution "$stage/distribution.xml" --resources "$pkg/pkg/resources" \
    --package-path "$stage" "$installer_pkg"
  rm -rf "$stage"
  echo "$installer_pkg"
fi

