#!/bin/sh
set -eu

ARCH=${ARCH:-x86_64}
ALPINE_VERSION=${ALPINE_VERSION:-edge}
MIRROR=${MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
ROOT=${ROOT:-build/rootfs}
CACHE=${CACHE:-cache}
APK_STATIC="$CACHE/apk.static"
APK_PAYLOADS="$CACHE/payloads-$ALPINE_VERSION-$ARCH"

case "$ROOT" in
	""|"/")
		echo "refusing to build rootfs into ROOT=$ROOT" >&2
		exit 1
		;;
esac

mkdir -p "$CACHE" build out
rm -rf "$ROOT"
mkdir -p "$ROOT"

case "$ARCH" in
	x86_64) apk_arch=x86_64 ;;
	aarch64) apk_arch=aarch64 ;;
	*) echo "unsupported ARCH: $ARCH" >&2; exit 1 ;;
esac

fetch_apk_static() {
	# Re-fetch if missing, not executable, or suspiciously small (corrupt)
	if [ -x "$APK_STATIC" ] && [ "$(wc -c < "$APK_STATIC")" -gt 100000 ]; then
		return
	fi
	rm -f "$APK_STATIC"

	index="$CACHE/APKINDEX-$ALPINE_VERSION-main-$apk_arch.tar.gz"
	index_dir="$CACHE/index-$ALPINE_VERSION-main-$apk_arch"
	mkdir -p "$index_dir"

	curl -L -o "$index" "$MIRROR/$ALPINE_VERSION/main/$apk_arch/APKINDEX.tar.gz"
	tar -xzf "$index" -C "$index_dir" APKINDEX
	apk_tools_version=$(
		awk 'BEGIN{RS=""} /\nP:apk-tools-static\n/ { split($0, a, "\n"); for (i in a) if (a[i] ~ /^V:/) { sub(/^V:/, "", a[i]); print a[i]; exit } }' "$index_dir/APKINDEX"
	)
	if [ -z "$apk_tools_version" ]; then
		echo "could not discover apk-tools-static version" >&2
		exit 1
	fi

	url="$MIRROR/$ALPINE_VERSION/main/$apk_arch/apk-tools-static-$apk_tools_version.apk"
	tmp="$CACHE/apk-tools-static.apk"
	echo "fetching $url"
	curl -L -o "$tmp" "$url"
	tar -xzf "$tmp" -C "$CACHE" sbin/apk.static
	mv "$CACHE/sbin/apk.static" "$APK_STATIC"
	rmdir "$CACHE/sbin"
	chmod +x "$APK_STATIC"
}

init_apk_database() {
	index="$CACHE/installed-$ALPINE_VERSION-$apk_arch.tar.gz"

	mkdir -p "$ROOT/lib/apk/db" "$ROOT/var/cache/apk" "$ROOT/etc/apk"
	"$APK_STATIC" index --allow-untrusted --no-warnings -o "$index" "$APK_PAYLOADS"/*.apk
	tar -xOzf "$index" APKINDEX | awk '
		BEGIN { RS = ""; ORS = "" }

		function forbidden_busybox_dep(dep) {
			return dep ~ /^busybox([<>=~].*)?$/ || dep ~ /^busybox-binsh([<>=~].*)?$/ || dep ~ /^ssl_client([<>=~].*)?$/
		}

		function emit_record(line_count, lines,    i, j, pkg, out, deps, dep, dep_count, first) {
			pkg = ""
			for (i = 1; i <= line_count; i++) {
				if (lines[i] ~ /^P:/) {
					pkg = substr(lines[i], 3)
					break
				}
			}
			if (pkg == "" || pkg == "busybox" || pkg == "busybox-binsh" || pkg == "ssl_client")
				return

			out = ""
			for (i = 1; i <= line_count; i++) {
				if (lines[i] ~ /^i:/)
					continue
				if (lines[i] ~ /^D:/) {
					deps = ""
					dep_count = split(substr(lines[i], 3), dep, " ")
					first = 1
					for (j = 1; j <= dep_count; j++) {
						if (dep[j] == "" || forbidden_busybox_dep(dep[j]))
							continue
						deps = deps (first ? "" : " ") dep[j]
						first = 0
					}
					if (deps != "")
						out = out "D:" deps "\n"
					continue
				}
				out = out lines[i] "\n"
			}
			printf "%s\n", out
		}

		{
			line_count = split($0, lines, "\n")
			emit_record(line_count, lines)
		}
	' > "$ROOT/lib/apk/db/installed"
	awk '/^P:/ { print substr($0, 3) }' "$ROOT/lib/apk/db/installed" | sort -u > "$ROOT/etc/apk/world"
	tar -cf "$ROOT/lib/apk/db/scripts.tar" --files-from /dev/null
	: > "$ROOT/lib/apk/db/triggers"
	: > "$ROOT/lib/apk/db/lock"
	chmod 0755 "$ROOT/lib/apk" "$ROOT/lib/apk/db" "$ROOT/var/cache/apk" "$ROOT/etc/apk"
	chmod 0644 \
		"$ROOT/lib/apk/db/installed" \
		"$ROOT/lib/apk/db/scripts.tar" \
		"$ROOT/lib/apk/db/triggers" \
		"$ROOT/lib/apk/db/lock" \
		"$ROOT/etc/apk/world"
}

repo_main="$MIRROR/$ALPINE_VERSION/main"
repo_community="$MIRROR/$ALPINE_VERSION/community"
repo_testing="$MIRROR/$ALPINE_VERSION/testing"

fetch_apk_static

mkdir -p "$ROOT/etc/apk" "$ROOT/etc/dpk" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/tmp"
printf '%s\n%s\n%s\n' "$repo_main" "$repo_community" "$repo_testing" > "$ROOT/etc/apk/repositories"
printf '%s\n%s\n%s\n' "$repo_main" "$repo_community" "$repo_testing" > "$ROOT/etc/dpk/repositories"

packages=$(sed -e 's/#.*//' -e '/^[	 ]*$/d' packages/base.list packages/graphics.list | tr '\n' ' ')

mkdir -p "$APK_PAYLOADS"
rm -f "$APK_PAYLOADS"/*.apk
"$APK_STATIC" \
	--arch "$apk_arch" \
	--no-cache \
	--allow-untrusted \
	-X "$repo_main" \
	-X "$repo_community" \
	-X "$repo_testing" \
	fetch --recursive --output "$APK_PAYLOADS" $packages

for apk in "$APK_PAYLOADS"/*.apk; do
	case "$(basename "$apk")" in
		busybox-[0-9]*.apk|busybox-binsh-[0-9]*.apk|ssl_client-[0-9]*.apk)
			echo "skipping forbidden BusyBox payload: $(basename "$apk")"
			continue
			;;
	esac
	# Preserve package file permissions exactly, including setuid bits.
	tar -xzf "$apk" -p -C "$ROOT" \
		--exclude=.PKGINFO \
		--exclude=.SIGN.* \
		--exclude=.INSTALL \
		--exclude=.pre-install \
		--exclude=.post-install \
		--exclude=.pre-upgrade \
		--exclude=.post-upgrade \
		--exclude=.trigger \
		2>/dev/null || true
done

if [ -e "$ROOT/bin/busybox" ] || [ -e "$ROOT/usr/bin/busybox" ]; then
	echo "busybox appeared in the target rootfs; refusing to continue" >&2
	exit 1
fi

cp -a rootfs-overlay/. "$ROOT/"
install -m 0755 out/darkelf "$ROOT/usr/bin/darkelf"
mkdir -p "$ROOT/usr/lib/darkos"
install -m 0755 out/darkdiskinit "$ROOT/usr/lib/darkos/darkdiskinit"
install -m 4755 out/darkshutdown "$ROOT/usr/bin/darkshutdown"
mkdir -p "$ROOT/sbin"
install -m 0755 out/darkrcinit "$ROOT/sbin/darkrcinit"
rm -f "$ROOT/init"
ln -s sbin/init "$ROOT/init"
if [ -f wallpaper.jpg ]; then
	mkdir -p "$ROOT/usr/share/backgrounds/darkos"
	install -m 0644 wallpaper.jpg "$ROOT/usr/share/backgrounds/darkos/wallpaper.jpg"
fi

adwaita_png_fallback="$ROOT/usr/share/icons/Adwaita/16x16/mimetypes/image-x-generic.png"
if [ -f "$adwaita_png_fallback" ]; then
	for icon in image-missing image-loading; do
		rm -f "$ROOT/usr/share/icons/Adwaita/scalable/status/$icon.svg"
		mkdir -p "$ROOT/usr/share/icons/Adwaita/16x16/status" \
			"$ROOT/usr/share/icons/Adwaita/scalable/status"
		install -m 0644 "$adwaita_png_fallback" \
			"$ROOT/usr/share/icons/Adwaita/16x16/status/$icon.png"
		install -m 0644 "$adwaita_png_fallback" \
			"$ROOT/usr/share/icons/Adwaita/scalable/status/$icon.png"
	done
	for icon in image-missing-symbolic image-loading-symbolic; do
		rm -f "$ROOT/usr/share/icons/Adwaita/symbolic/status/$icon.svg"
		mkdir -p "$ROOT/usr/share/icons/Adwaita/symbolic/status"
		install -m 0644 "$adwaita_png_fallback" \
			"$ROOT/usr/share/icons/Adwaita/symbolic/status/$icon.png"
	done
	for cache in \
		"$ROOT/usr/share/icons/Adwaita/icon-theme.cache" \
		"$ROOT/usr/share/icons/hicolor/icon-theme.cache"
	do
		rm -f "$cache"
	done
fi

chmod 0755 \
	"$ROOT/etc/init.d/sddm" \
	"$ROOT/etc/init.d/wpa_supplicant" \
	"$ROOT/etc/init.d/networkmanager" \
	"$ROOT/etc/init.d/virtualbox-guest-additions" \
	"$ROOT/usr/bin/darkos-x-config" \
	"$ROOT/usr/bin/darkos-x-session" \
	"$ROOT/usr/bin/start-darkos-x" \
	"$ROOT/usr/bin/darkos-session-setup" \
	"$ROOT/usr/bin/darkos-plasma-session" \
	"$ROOT/usr/bin/dpk" \
	"$ROOT/usr/bin/darkos-install" \
	"$ROOT/usr/bin/darkos-install-launcher" \
	"$ROOT/usr/bin/darkos-desktop-setup" \
	"$ROOT/usr/bin/darkos-gtk-setup" \
	"$ROOT/usr/bin/help" \
	"$ROOT/usr/bin/lock" \
	"$ROOT/usr/bin/logout" \
	"$ROOT/usr/bin/reboot" \
	"$ROOT/usr/bin/poweroff" \
	"$ROOT/usr/bin/network-status" \
	"$ROOT/usr/bin/network-up" \
	"$ROOT/usr/bin/posix-check" \
	"$ROOT/usr/bin/xloadimage"
for desktop_file in \
	"$ROOT/usr/share/applications/darkos-install.desktop" \
	"$ROOT/root/Desktop/install-darkos.desktop" \
	"$ROOT/etc/skel/Desktop/install-darkos.desktop"
do
	[ -e "$desktop_file" ] && chmod 0755 "$desktop_file"
done
rm -f "$ROOT/etc/X11/xorg.conf"
rm -f "$ROOT/etc/X11/xorg.conf.d/10-fbdev.conf"
rm -f "$ROOT/etc/X11/xorg.conf.d/10-darkos-video.conf"
rm -f "$ROOT/usr/share/X11/xorg.conf.d/00-darkos-fbdev.conf"

mkdir -p "$ROOT/root" "$ROOT/home/dark" "$ROOT/var/log" "$ROOT/var/lib/dhcpcd" "$ROOT/var/db" "$ROOT/media/cdrom"
chmod 1777 "$ROOT/tmp"

init_apk_database

mkdir -p "$ROOT/lib" "$ROOT/usr/lib"
if [ -e "$ROOT/lib/ld-musl-x86_64.so.1" ]; then
	install -m 0755 "$ROOT/lib/ld-musl-x86_64.so.1" "$ROOT/usr/lib/ld-musl-x86_64.so.1"
	install -m 0755 "$ROOT/lib/ld-musl-x86_64.so.1" "$ROOT/usr/lib/libc.so.1"
	install -m 0755 "$ROOT/lib/ld-musl-x86_64.so.1" "$ROOT/usr/lib/libc.musl-x86_64.so.1"
elif [ -e "$ROOT/usr/lib/libc.so.1" ]; then
	install -m 0755 "$ROOT/usr/lib/libc.so.1" "$ROOT/lib/ld-musl-x86_64.so.1"
	install -m 0755 "$ROOT/usr/lib/libc.so.1" "$ROOT/lib/libc.musl-x86_64.so.1"
	install -m 0755 "$ROOT/usr/lib/libc.so.1" "$ROOT/usr/lib/ld-musl-x86_64.so.1"
fi

rm -f "$ROOT/dev/fb0"

if [ ! -x "$ROOT/bin/mksh" ]; then
	echo "mksh is required for the DarkOS POSIX shell" >&2
	exit 1
fi
rm -f "$ROOT/bin/sh"
ln -s mksh "$ROOT/bin/sh"

rm -f "$ROOT/sbin/init"
ln -s darkrcinit "$ROOT/sbin/init"

mkdir -p "$ROOT/sbin"
ln -sf /usr/bin/reboot "$ROOT/sbin/reboot"
ln -sf /usr/bin/poweroff "$ROOT/sbin/poweroff"

ensure_group() {
	name=$1
	gid=$2
	if ! grep -q "^$name:" "$ROOT/etc/group"; then
		printf '%s:x:%s:\n' "$name" "$gid" >> "$ROOT/etc/group"
	fi
}

ensure_user() {
	name=$1
	uid=$2
	gid=$3
	gecos=$4
	home=$5
	shell=$6
	if ! grep -q "^$name:" "$ROOT/etc/passwd"; then
		printf '%s:x:%s:%s:%s:%s:%s\n' "$name" "$uid" "$gid" "$gecos" "$home" "$shell" >> "$ROOT/etc/passwd"
	fi
	# Ensure a shadow entry exists so unix_chkpwd can resolve the user.
	# '*' means the account is locked (no password); autologin does not need one.
	if [ -f "$ROOT/etc/shadow" ] && ! grep -q "^$name:" "$ROOT/etc/shadow"; then
		printf '%s:*::0:::::\n' "$name" >> "$ROOT/etc/shadow"
	fi
}

ensure_group_member() {
	group=$1
	member=$2
	tmp="$ROOT/etc/group.tmp"

	if awk -F: -v group="$group" -v member="$member" '
		$1 == group {
			found = 1
			n = split($4, members, ",")
			for (i = 1; i <= n; i++)
				if (members[i] == member)
					member_found = 1
		}
		END { exit !(found && member_found) }
	' "$ROOT/etc/group"; then
		return
	fi

	awk -F: -v OFS=: -v group="$group" -v member="$member" '
		$1 == group { $4 = ($4 == "" ? member : $4 "," member) }
		{ print }
	' "$ROOT/etc/group" > "$tmp"
	mv "$tmp" "$ROOT/etc/group"
}

ensure_group messagebus 81
ensure_user messagebus 81 81 messagebus /dev/null /sbin/nologin
ensure_group dhcpcd 101
ensure_user dhcpcd 101 101 dhcpcd /var/lib/dhcpcd /sbin/nologin
ensure_group polkitd 102
ensure_user polkitd 102 102 polkitd /var/empty /sbin/nologin
mkdir -p "$ROOT/var/empty"
ensure_group dark 1000
ensure_user dark 1000 1000 "DarkOS Live User" /home/dark /bin/sh
ensure_group audio 18
ensure_group input 24
ensure_group_member audio dark
ensure_group_member input dark
ensure_group_member video dark
mkdir -p "$ROOT/home/dark"

if [ -e "$ROOT/usr/bin/sddm" ]; then
	ensure_group sddm 240
	ensure_user sddm 240 240 sddm /var/lib/sddm /sbin/nologin
	ensure_group_member video sddm
	mkdir -p "$ROOT/var/lib/sddm" "$ROOT/var/cache/sddm" "$ROOT/var/log/sddm"
	if [ "$(id -u)" -eq 0 ]; then
		chown -R 240:240 "$ROOT/var/lib/sddm" "$ROOT/var/cache/sddm" "$ROOT/var/log/sddm"
	fi
fi

mkdir -p "$ROOT/etc/runlevels/sysinit" "$ROOT/etc/runlevels/boot" "$ROOT/etc/runlevels/default"
rm -f \
	"$ROOT/etc/runlevels/default/networkmanager" \
	"$ROOT/etc/runlevels/default/wpa_supplicant"
for svc in devfs dmesg hwdrivers mdev modules sysctl cgroups; do
	[ -e "$ROOT/etc/init.d/$svc" ] && ln -sf "../../init.d/$svc" "$ROOT/etc/runlevels/sysinit/$svc"
done
for svc in bootmisc hostname hwclock keymaps localmount loopback modules seedrng syslog udev udev-trigger; do
	[ -e "$ROOT/etc/init.d/$svc" ] && ln -sf "../../init.d/$svc" "$ROOT/etc/runlevels/boot/$svc"
done
for svc in dbus darkos-fb darkos-local elogind wpa_supplicant networkmanager polkit openrc-settingsd sddm udisks2 virtualbox-guest-additions; do
	[ -e "$ROOT/etc/init.d/$svc" ] && ln -sf "../../init.d/$svc" "$ROOT/etc/runlevels/default/$svc"
done

# Harden shadow and unix_chkpwd permissions.
# /etc/shadow: root:shadow 0640 — readable only by root and the shadow group.
# unix_chkpwd: setgid shadow 02755 — allows non-root PAM callers (elogind,
# polkit, sddm) to verify passwords without exposing the shadow file directly.
# Note: mksquashfs -all-root would override ownership, so mkiso.sh also
# applies these via a -pf pseudo-file. Setting them here keeps the rootfs
# tree itself correct for any tooling that inspects it directly.
ensure_group shadow 42
if [ -f "$ROOT/etc/shadow" ]; then
	chmod 0640 "$ROOT/etc/shadow"
	if [ "$(id -u)" -eq 0 ]; then
		chown 0:42 "$ROOT/etc/shadow"
	fi
fi
if [ -f "$ROOT/usr/sbin/unix_chkpwd" ]; then
	chmod 02755 "$ROOT/usr/sbin/unix_chkpwd"
	if [ "$(id -u)" -eq 0 ]; then
		chown 0:42 "$ROOT/usr/sbin/unix_chkpwd"
	fi
fi

echo "DarkOS rootfs ready at $ROOT"
