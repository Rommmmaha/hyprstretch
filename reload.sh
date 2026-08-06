#!/usr/bin/env sh
# this script is for testing only
set -euo pipefail

PLUGIN_NAME="hyprstretch"
SO="hyprstretch/$PLUGIN_NAME.so"
ABS_SO="$(pwd)/$SO"

make -C hyprstretch all

hyprctl plugin unload "$ABS_SO" || true
hyprctl plugin load "$ABS_SO"
