#!/usr/bin/env bash
#
# Recursively copy all MinGW runtime DLL dependencies next to the target
# binary. windeployqt only handles Qt-owned DLLs, not the rest of the
# MinGW C/C++/library runtime (libb2, libicu, libdouble-conversion, ...).
#
# Usage:  deploy-mingw-deps.sh <target.exe-or-dll> [<mingw-bin-dir>]
#
set -euo pipefail

target="${1:?usage: $0 <target.exe|dll> [mingw-bin-dir]}"
mingw_bin="${2:-/mingw64/bin}"
target_dir="$(dirname "$target")"

declare -A copied=()

walk() {
    local file="$1"
    local deps
    deps=$(objdump -p "$file" 2>/dev/null \
        | awk '/DLL Name/ {print $3}')

    for dep in $deps; do
        # already handled — avoid infinite recursion
        [[ -n "${copied[$dep]:-}" ]] && continue

        # 1. dep already lives in target_dir (e.g. libstrongswan-0.dll
        #    that we just built). Don't copy, but do recurse.
        if [[ -f "$target_dir/$dep" ]]; then
            copied[$dep]=1
            walk "$target_dir/$dep"
            continue
        fi

        # 2. dep lives in mingw_bin: copy and recurse.
        local src="$mingw_bin/$dep"
        if [[ -f "$src" ]]; then
            copied[$dep]=1
            cp -u "$src" "$target_dir/"
            walk "$src"
            continue
        fi

        # 3. Otherwise it's a system DLL — the loader will find it.
    done
}

walk "$target"
# Also: every DLL already sitting in target_dir (built by us) might bring
# in further transitive deps — walk them too.
for dll in "$target_dir"/*.dll; do
    [[ -f "$dll" ]] || continue
    name="$(basename "$dll")"
    [[ -n "${copied[$name]:-}" ]] && continue
    copied[$name]=1
    walk "$dll"
done

echo "deploy-mingw-deps: copied ${#copied[@]} DLL(s) into $target_dir"
