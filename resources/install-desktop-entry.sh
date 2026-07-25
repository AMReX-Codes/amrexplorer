#!/usr/bin/env bash
# Install the AMReXplorer icon + .desktop entry into the per-user XDG locations so
# that Linux docks/taskbars show the logo. Re-run after rebuilding if the
# executable moves. Removes nothing else; fully user-local and reversible
# (delete ~/.local/share/applications/amrexplorer.desktop and the amrexplorer.png files
# under ~/.local/share/icons/hicolor to undo).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="amrexplorer"
BIN="${AMREXPLORER_BIN:-$ROOT/build/src/qt/amrexplorer}"
ICON_BASE="$HOME/.local/share/icons/hicolor"
APPS_DIR="$HOME/.local/share/applications"

if [[ ! -x "$BIN" ]]; then
    echo "amrexplorer not found at $BIN -- build it first, or set AMREXPLORER_BIN." >&2
    exit 1
fi

# Icons named after the app so Icon=amrexplorer resolves them from the theme.
for s in 16 32 64 128 256; do
    install -d "$ICON_BASE/${s}x${s}/apps"
    install -m 0644 "$ROOT/resources/$APP-$s.png" "$ICON_BASE/${s}x${s}/apps/$APP.png"
done

# .desktop entry; StartupWMClass matches the app's WM_CLASS / desktop file name
# so the dock associates the running window with this entry and its icon.
install -d "$APPS_DIR"
cat > "$APPS_DIR/$APP.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=AMReXplorer
GenericName=AMR Visualization
Comment=Demand-driven AMR visualization
Exec=$BIN %F
Icon=$APP
StartupWMClass=$APP
Terminal=false
Categories=Science;DataVisualization;
EOF

# gtk-update-icon-cache warns ("No theme index file") without an index.theme;
# copy the system hicolor one into the user tree if it is missing.
if [[ ! -f "$ICON_BASE/index.theme" && -f /usr/share/icons/hicolor/index.theme ]]; then
    cp /usr/share/icons/hicolor/index.theme "$ICON_BASE/index.theme"
fi
gtk-update-icon-cache -f -t "$ICON_BASE" >/dev/null 2>&1 || true
update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true

echo "Installed AMReXplorer icon + desktop entry (user-local):"
echo "  icons: $ICON_BASE/{16,32,64,128,256}x.../apps/$APP.png"
echo "  entry: $APPS_DIR/$APP.desktop"
echo "Re-launch the app (or use the desktop entry); the dock should now show the logo."
echo "Some desktops need a logout/login to refresh their icon cache."
