#!/bin/sh
# Registers the installed Ungine for the current user: application menu entry with icon, and
# .ungineproj files open in UngineEditor on double-click. Run it from the install's bin/ folder.
set -e
BIN="$(cd "$(dirname "$0")" && pwd)"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$DATA/applications" "$DATA/mime/packages" "$DATA/icons/hicolor/256x256/apps" \
         "$DATA/icons/hicolor/256x256/mimetypes"
cp "$BIN/resources/ungine.png" "$DATA/icons/hicolor/256x256/apps/ungine.png"
cp "$BIN/resources/ungine.png" "$DATA/icons/hicolor/256x256/mimetypes/application-x-ungine-project.png"
sed "s|@BIN@|$BIN|g" "$BIN/resources/ungine-editor.desktop.in" > "$DATA/applications/ungine-editor.desktop"
cp "$BIN/resources/ungine-mime.xml" "$DATA/mime/packages/ungine.xml"
command -v update-mime-database >/dev/null && update-mime-database "$DATA/mime" || true
command -v update-desktop-database >/dev/null && update-desktop-database "$DATA/applications" || true
command -v xdg-mime >/dev/null && xdg-mime default ungine-editor.desktop application/x-ungine-project || true
echo "Ungine registered: 'Ungine Editor' in the menu, .ungineproj files open in the editor."
