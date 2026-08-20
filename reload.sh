#!/usr/bin/env sh
# this script is for testing only
set -euo pipefail

PLUGIN_NAME="hyprstretch"
ABS_SO="$(pwd)/hyprstretch/$PLUGIN_NAME.so"
HYPRPM_SO="/var/cache/hyprpm/$USER/$PLUGIN_NAME/$PLUGIN_NAME.so"

cmd="${1:-reload}"

case "$cmd" in
  reload)
    make -C hyprstretch all
    hyprctl plugin unload "$ABS_SO" || true
    hyprctl plugin unload "$HYPRPM_SO" || true
    hyprctl plugin load "$ABS_SO"
    ;;
  restore)
    hyprctl plugin unload "$ABS_SO" || true
    hyprctl plugin unload "$HYPRPM_SO" || true
    hyprpm reload
    ;;
  *)
    echo "usage: $0 [reload|restore]" >&2
    exit 1
    ;;
esac
