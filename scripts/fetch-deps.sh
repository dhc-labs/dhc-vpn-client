#!/usr/bin/env bash
#
# Fetch external dependencies into third_party/:
#   - strongSwan source (cloned from GitHub, pinned tag)
#   - Wintun SDK (DLL + header, binary package from wintun.net)
#   - WinDivert SDK (DLL + .sys + header + .lib, GitHub release)
#
# These are intentionally not committed to the repo — fetch-deps.sh
# reproducibly recreates them.
#
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"

STRONGSWAN_TAG="${STRONGSWAN_TAG:-6.0.6}"
WINTUN_VERSION="${WINTUN_VERSION:-0.14.1}"
WINDIVERT_VERSION="${WINDIVERT_VERSION:-2.2.2}"

# --- strongSwan ---
ss_dir="$repo_root/third_party/strongswan"
if [[ -d "$ss_dir/.git" ]]; then
    echo "==> strongSwan already present at $ss_dir"
else
    echo "==> clone strongSwan @ $STRONGSWAN_TAG"
    git clone --depth=1 --branch "$STRONGSWAN_TAG" \
        https://github.com/strongswan/strongswan.git "$ss_dir"
fi

# --- Wintun ---
wt_dir="$repo_root/third_party/wintun"
if [[ -f "$wt_dir/include/wintun.h" ]]; then
    echo "==> Wintun already present at $wt_dir"
else
    echo "==> download Wintun $WINTUN_VERSION"
    tmp="$repo_root/third_party/wintun.zip"
    curl -fsSL -o "$tmp" "https://www.wintun.net/builds/wintun-${WINTUN_VERSION}.zip"
    # The ZIP unpacks directly into third_party/ and creates wintun/
    ( cd "$repo_root/third_party" && unzip -q -o wintun.zip )
    rm -f "$tmp"
fi

# --- WinDivert ---
wd_dir="$repo_root/third_party/windivert"
if [[ -f "$wd_dir/include/windivert.h" ]]; then
    echo "==> WinDivert already present at $wd_dir"
else
    echo "==> download WinDivert $WINDIVERT_VERSION"
    tmp="$repo_root/third_party/windivert.zip"
    curl -fsSL -o "$tmp" \
        "https://github.com/basil00/WinDivert/releases/download/v${WINDIVERT_VERSION}/WinDivert-${WINDIVERT_VERSION}-A.zip"
    # The ZIP unpacks into WinDivert-<ver>-A/. Rename to a stable lowercase path.
    ( cd "$repo_root/third_party" && unzip -q -o windivert.zip \
        && rm -rf windivert \
        && mv "WinDivert-${WINDIVERT_VERSION}-A" windivert )
    rm -f "$tmp"
fi

echo
echo "DONE."
echo "  strongSwan: $ss_dir"
echo "  Wintun:     $wt_dir"
echo "  WinDivert:  $wd_dir"
