#!/bin/sh
# Export browser environment variables for interactive shells
export BROWSER="${BROWSER:-chromium}"
export OZONE_PLATFORM=wayland

# Prefer Wayland backend for GDK when available
export GDK_BACKEND=wayland,x11
