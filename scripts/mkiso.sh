#!/bin/sh
set -eu

ROOT=${ROOT:-build/rootfs}
ISO_ROOT=${ISO_ROOT:-build/iso}
STAGE=${STAGE:-build/initramfs-stage}
OUT=${OUT:-out/darkos.iso}

case "$ISO_ROOT" in
	""|"/")
		echo "refusing to stage ISO into ISO_ROOT=$ISO_ROOT" >&2
		exit 1
		;;
esac

rm -rf "$ISO_ROOT"
mkdir -p "$ISO_ROOT/boot/grub/fonts" out

kernel=$(find "$ROOT/boot" -name 'vmlinuz*' -type f | sort | tail -n 1)

if [ -z "$kernel" ]; then
	echo "kernel missing in $ROOT/boot" >&2
	exit 1
fi

# Build a pseudo-file list so mksquashfs can set correct ownership and
# setgid on security-sensitive files while defaulting everything else to
# root:root (equivalent to -all-root for the remaining tree).
squashfs_pseudo=$(mktemp /tmp/darkos-pseudo.XXXXXX)
cat > "$squashfs_pseudo" <<'PSEUDO'
# shadow must be root:shadow (0:42) mode 0640 so unix_chkpwd can read it
etc/shadow m 0640 0 42
# unix_chkpwd must be setgid shadow (mode 02755, gid 42) so non-root PAM
# callers (elogind, polkit, sddm) can verify passwords against shadow
usr/sbin/unix_chkpwd m 02755 0 42
PSEUDO
mksquashfs "$ROOT" "$ISO_ROOT/boot/darkos.squashfs" \
	-noappend -all-root -comp xz \
	-pseudo-override \
	-pf "$squashfs_pseudo"
rm -f "$squashfs_pseudo"
cp "$kernel" "$ISO_ROOT/boot/vmlinuz"
if [ -f "$ROOT/usr/share/grub/unicode.pf2" ]; then
	install -m 0644 "$ROOT/usr/share/grub/unicode.pf2" "$ISO_ROOT/boot/grub/fonts/unicode.pf2"
elif [ -f /usr/share/grub/unicode.pf2 ]; then
	install -m 0644 /usr/share/grub/unicode.pf2 "$ISO_ROOT/boot/grub/fonts/unicode.pf2"
else
	echo "GRUB unicode.pf2 font missing; install grub fonts on the host or in the rootfs" >&2
	exit 1
fi

echo "generating DarkOS stage-1 initramfs"
rm -rf "$STAGE"
mkdir -p \
	"$STAGE/bin" \
	"$STAGE/sbin" \
	"$STAGE/lib" \
	"$STAGE/usr/lib" \
	"$STAGE/proc" \
	"$STAGE/sys" \
	"$STAGE/dev" \
	"$STAGE/run" \
	"$STAGE/tmp" \
	"$STAGE/media/cdrom" \
	"$STAGE/lowerroot" \
	"$STAGE/overlay" \
	"$STAGE/newroot"

install -m 0755 out/darkstage1 "$STAGE/init"
cp -L "$ROOT/bin/kmod" "$STAGE/bin/kmod"
chmod 0755 "$STAGE/bin/kmod"
ln -sf ../bin/kmod "$STAGE/sbin/modprobe"
cp -a "$ROOT/lib/modules" "$STAGE/lib/modules"

copy_stage_file() {
	src=$1
	dst=$2
	if [ -e "$src" ]; then
		mkdir -p "$(dirname "$dst")"
		cp -L "$src" "$dst"
	fi
}

copy_stage_lib() {
	rel=$1
	if [ -e "$ROOT/$rel" ]; then
		copy_stage_file "$ROOT/$rel" "$STAGE/$rel"
		case "$rel" in
			/usr/lib/*)
				copy_stage_file "$ROOT/$rel" "$STAGE/lib/$(basename "$rel")"
				;;
		esac
	fi
}

for lib in \
	/lib/ld-musl-x86_64.so.1 \
	/lib/libc.musl-x86_64.so.1 \
	/lib/libz.so.1 \
	/usr/lib/ld-musl-x86_64.so.1 \
	/usr/lib/libc.so.1 \
	/usr/lib/libc.musl-x86_64.so.1 \
	/usr/lib/libzstd.so.1 \
	/usr/lib/liblzma.so.5 \
	/usr/lib/libz.so.1 \
	/usr/lib/libcrypto.so.3
do
	copy_stage_lib "$lib"
done

(cd "$STAGE" && find . -print | cpio -o -H newc | gzip -9) > "$ISO_ROOT/boot/initramfs"

cat > "$ISO_ROOT/boot/grub/grub.cfg" <<'CFG'
set timeout=2
set default=0
set gfxmode=auto
set gfxpayload=keep

insmod all_video
insmod font
if loadfont /boot/grub/fonts/unicode.pf2; then
	insmod gfxterm
	terminal_output gfxterm
fi

menuentry "DarkOS" {
	linux /boot/vmlinuz quiet console=tty0 i8042.nomux=1 i8042.reset=1 psmouse.proto=imps
	initrd /boot/initramfs
}
CFG

cat > "$ISO_ROOT/DARKOS-DRIVERS.txt" <<'EOF'
DarkOS bundled input and mouse drivers
======================================

Keyboard drivers included:
- i8042 (built into kernel)
- libps2 (built into kernel)
- atkbd (built into kernel)
- applespi.ko.gz
- cros_ec_keyb.ko.gz
- cypress-sf.ko.gz
- gpio_keys.ko.gz
- gpio_keys_polled.ko.gz
- hyperv-keyboard.ko.gz
- sparse-keymap.ko.gz
- matrix-keymap.ko.gz
- usbhid.ko.gz
- hid-generic.ko.gz
- hid-cherry.ko.gz
- hid-asus.ko.gz
- hid-lenovo.ko.gz
- hid-keytouch.ko.gz
- hid-prodikeys.ko.gz
- hid-logitech*.ko.gz
- hid-microsoft.ko.gz
- hid-apple.ko.gz

Mouse and pointer drivers included:
- psmouse.ko.gz
- sermouse.ko.gz
- mousedev.ko.gz
- evdev.ko.gz
- joydev.ko.gz
- uinput.ko.gz
- usbhid.ko.gz
- hid-generic.ko.gz
- hid-multitouch.ko.gz
- hid-logitech*.ko.gz
- hid-microsoft.ko.gz
- hid-apple.ko.gz
- hid-magicmouse.ko.gz
- wacom.ko.gz
- synaptics_usb.ko.gz
- synaptics_i2c.ko.gz
- xhci/ehci/ohci/uhci USB host controller modules
- virtio_input.ko.gz

Xorg input drivers included:
- evdev_drv.so (keyboard and pointer)
- libinput_drv.so (keyboard and pointer)
- vmmouse_drv.so
- synaptics_drv.so
- mtrack_drv.so
- wacom_drv.so
- joystick_drv.so

Debug tools included:
- xinput
- evtest
- kbd tools
- xkbcomp
- setxkbmap
- xkeyboard-config

Network drivers included:
- e100/e1000/e1000e/igb/igc/ixgbe Intel wired NICs
- r8169/8139too/8139cp Realtek wired NICs
- tg3/b44/bnx2/bnx2x/bnxt_en Broadcom wired NICs
- virtio_net, vmxnet3, hv_netvsc, ena, gve virtual NICs
- pcnet32, ne2k-pci, forcedeth legacy/VM NICs
- usbnet, cdc_ether, cdc_ncm, cdc_mbim, rndis_host USB Ethernet
- common Wi-Fi modules: iwlwifi, ath9k/ath10k/ath11k/ath12k, rtw88/rtw89, brcmfmac, mt76

Network commands included:
- help network
- network-up
- network-status
- dhcpcd
- ip
- ping
EOF

cat > "$ISO_ROOT/DARKOS-BOOT.txt" <<'EOF'
DarkOS boot support
===================

This ISO is built with GRUB rescue media support for both:

- legacy BIOS El Torito boot
- x86_64 UEFI removable-media boot

The build fails if the finished ISO does not contain the UEFI El Torito
boot image.
EOF

if ! command -v grub-mkrescue >/dev/null 2>&1; then
	echo "grub-mkrescue not found; install grub on the host" >&2
	exit 1
fi
if [ ! -d /usr/lib/grub/x86_64-efi ]; then
	echo "GRUB x86_64-efi modules not found; install the host GRUB EFI files for UEFI ISO boot" >&2
	exit 1
fi

grub-mkrescue -o "$OUT" "$ISO_ROOT"

eltorito_report=${TMPDIR:-/tmp}/darkos-eltorito.$$
xorriso -indev "$OUT" -report_el_torito as_mkisofs > "$eltorito_report" 2>/dev/null
if ! grep -q -- "-e '/efi.img'" "$eltorito_report"; then
	echo "UEFI boot image missing from $OUT" >&2
	cat "$eltorito_report" >&2
	rm -f "$eltorito_report"
	exit 1
fi
rm -f "$eltorito_report"

echo "DarkOS ISO ready at $OUT"
