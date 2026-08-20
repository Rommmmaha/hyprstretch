#!/usr/bin/env sh
# this script is for testing only
set -euo pipefail

PLUGIN_NAME="hyprstretch"
SO="hyprstretch/$PLUGIN_NAME.so"
ABS_SO="$(pwd)/$SO"
HYPRPM_SO="/var/cache/hyprpm/r/$PLUGIN_NAME/$PLUGIN_NAME.so"

cmd="${1:-reload}"

case "$cmd" in
  reload)
    make -C hyprstretch all
    # drop any loaded instance first so hyprpm's copy and the local copy don't clash
    hyprctl plugin unload "$HYPRPM_SO" || true
    hyprctl plugin unload "$ABS_SO" || true
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
