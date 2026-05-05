#!/usr/bin/env bash
cd "$(dirname "$0")/../build/src/gui" || exit 1
for dll in Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll Qt6Network.dll; do
    echo "=== $dll ==="
    objdump -p "$dll" 2>/dev/null \
        | grep "DLL Name" \
        | awk '{print $3}' \
        | grep -iE '^(lib|zlib)'
done
