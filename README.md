# DarkOS

Not to be confused with [dArkOS.](https://github.com/christianhaitian/dArkOS)

DarkOS is a small POSIX-oriented Operating System build scaffold with:

- no BusyBox in the target root filesystem
- musl libc runtime, musl development headers, Linux headers, and a POSIX shell as `/bin/sh`
- `/dev/fb0` framebuffer setup and DRM device preparation
- automatic KDE Plasma 6.7 Wayland session with KWin
- UTF-8 locale defaults through musl locales
- legacy BIOS and x86_64 UEFI ISO boot through GRUB
- writable live root filesystem through an overlayfs tmpfs upper layer
- X11 and Wayland runtime support
- GTK application support
- a native package command, `dpk` (`DarkOS Package Manager`), backed by Alpine repositories
- an ELF parser utility, `darkelf`
- a package set intended to expose roughly 300+ standalone commands through discrete packages
- Alpine repository apps: `xeyes`, `firefox`, `xterm`, `konsole`, `dolphin`, and `mpv`
- `xloadimage` command compatibility through Alpine's `imagemagick` package, because Alpine does not currently publish an `xloadimage` package in stable or edge

This repository does not vendor a kernel or Alpine packages. The build scripts fetch Alpine's static package manager and assemble a root filesystem from Alpine package directories.

## Requirements

On the host:

- POSIX shell
- `curl`
- `tar`
- `gzip`
- `mksquashfs`
- `xorriso`
- GRUB BIOS and x86_64 EFI modules for `grub-mkrescue`
- `cc`
- Linux kernel and initramfs packages available from the configured Alpine repository

## Build

```sh
make iso
```

The output is written to:

```text
out/darkos.iso
```

The first build downloads `apk.static` into `cache/` and then uses Alpine edge package repositories to assemble `build/rootfs`. This tracks KDE Plasma 6.7 and its Wayland-first session.

## Try in QEMU

```sh
make run
```

`make run` boots the existing `out/darkos.iso`. It does not rebuild the ISO; run `make iso` first when you want a fresh image.
On first run it also creates a persistent installer target disk:

```text
out/darkos-disk.qcow2
```

Inside DarkOS, `darkos-install` auto-detects that single target disk. In QEMU it
usually appears as `/dev/sda`; on laptops it may be `/dev/nvme0n1` or
`/dev/mmcblk0`. To change the QEMU disk path or size:

```sh
make run QEMU_DISK=out/test.qcow2 QEMU_DISK_SIZE=32G
```

The default boot command line uses the kernel's native video mode:

```text
quiet console=tty0 i8042.nomux=1 i8042.reset=1 psmouse.proto=imps
```

DarkOS boots the read-only squashfs through a writable overlayfs live root.
Filesystem changes are writable during the session and reset on reboot unless
persistent storage is added later.

## Package Management

Inside DarkOS:

```sh
dpk update
dpk search xterm
dpk add xterm
dpk del xterm
dpk info mpv
```

`dpk` stores Alpine repository configuration in `/etc/dpk/repositories` and delegates package solving/installing to `apk` while keeping the DarkOS-facing command name stable.

## Graphics

DarkOS boots with `eudev`, loads common framebuffer modules when available, and creates `/dev/fb0` as a fallback character device if the kernel exposes framebuffer major `29`.

SDDM starts KDE Plasma 6.7 automatically after boot through a native Wayland session using KWin. X11 applications remain available through Xwayland.

Use the desktop session selected in SDDM. Xwayland starts automatically for X11 applications.

## Locales

DarkOS defaults to:

```text
LANG=en_US.UTF-8
LC_CTYPE=en_US.UTF-8
```

The root filesystem includes `musl-locales` and `musl-locales-lang`, plus Noto fonts for broader glyph coverage.

Check the POSIX-oriented runtime pieces:

```sh
posix-check
```

Start Wayland:

```sh
weston-launch
```

or, as a regular user with the right seat permissions:

```sh
weston
```

## Utilities

The target intentionally avoids BusyBox and uses real packages such as:

- `coreutils`
- `findutils`
- `diffutils`
- `grep`
- `sed`
- `gawk`
- `util-linux`
- `procps-ng`
- `iproute2`
- `iputils`
- `shadow`
- `musl`, `musl-dev`, `linux-headers`
- `tar`, `gzip`, `bzip2`, `xz`, `zstd`
- `e2fsprogs`, `dosfstools`, `xfsprogs`
- `pciutils`, `usbutils`
- editors, network tools, and process tools

Use this to count target commands after a rootfs build:

```sh
scripts/count-commands.sh build/rootfs
```

# WARNING FOR FLATPAK USERS!

If you try to install an Flatpak app, you must run `flatpak install flathub (FLATHUB APP)` as `sudo` for apps from flatpak in order to work, and replace `(FLATHUB APP)` to the app you are trying to install with Flathub.
