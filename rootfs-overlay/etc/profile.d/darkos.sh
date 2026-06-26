export EDITOR=vi
export PAGER=less
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export BROWSER="${BROWSER:-chromium}"
export OZONE_PLATFORM=wayland
export GDK_BACKEND=wayland,x11
export GDK_GL="${GDK_GL:-disable}"
export GIO_EXTRA_MODULES=/usr/lib/gio/modules
export GTK_THEME="${GTK_THEME:-adw-gtk3}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-0}"
export WEBKIT_DISABLE_COMPOSITING_MODE="${WEBKIT_DISABLE_COMPOSITING_MODE:-1}"
export WEBKIT_DISABLE_DMABUF_RENDERER="${WEBKIT_DISABLE_DMABUF_RENDERER:-1}"
export WEBKIT_WEBGL_DISABLE_GBM="${WEBKIT_WEBGL_DISABLE_GBM:-1}"
export NO_AT_BRIDGE=1
export QT_QPA_PLATFORM='wayland;xcb'
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
export MESA_SHADER_CACHE_DIR="${MESA_SHADER_CACHE_DIR:-$XDG_CACHE_HOME/mesa}"

mkdir -p "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" \
	"$MESA_SHADER_CACHE_DIR" "$HOME/.mozilla" 2>/dev/null || true

if [ -r /run/darkos/gtk/env.sh ]; then
	. /run/darkos/gtk/env.sh
fi

if [ -t 0 ]; then
	stty erase '^?' 2>/dev/null || true
fi
