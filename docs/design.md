# DarkOS Design Notes

DarkOS is intentionally Linux-kernel based. POSIX compliance is provided by the userland contract: a POSIX shell, musl libc, musl development headers, Linux headers, normal filesystem layout, process tools, permissions, terminals, signals, and standalone command implementations.

This makes DarkOS POSIX-oriented. Formal POSIX certification would still require running and passing a conformance suite.

## No BusyBox

The build avoids `alpine-base` and explicitly fails if `busybox` appears in the target root filesystem.

Utilities come from separate packages:

- GNU/Core POSIX utilities from `coreutils`
- `findutils`
- `diffutils`
- `grep`
- `sed`
- `gawk`
- `util-linux`
- `procps-ng`
- `musl`, `musl-dev`, and `linux-headers`
- filesystem tools
- network tools
- editors and diagnostics

This gives a real multi-binary userland rather than BusyBox applets.

## Package Manager

`dpk` means `DarkOS Package Manager`. It reads `/etc/dpk/repositories` and delegates package transactions to `apk`, using Alpine package directories as the binary package source.

This keeps DarkOS package commands branded while preserving Alpine dependency solving and signatures.

## Graphics

The graphical stack includes:

- `/dev/fb0` setup through `darkos-fb`
- Xorg with the `fbdev` driver
- XFCE as the default desktop environment
- `twm` and `xterm` rescue sessions through `darkos.session=`
- `xfce4-terminal`, `thunar`, `xeyes`, `xfe`, `firefox`, `mpv`
- `xloadimage` compatibility via the Alpine-packaged ImageMagick `display` command
- Wayland libraries and Weston
- Xwayland for X11 apps under Wayland
- GTK 2, GTK 3, and GTK 4 runtime libraries

`darkos-x` starts the X session automatically during the default OpenRC runlevel. `start-darkos-x` remains available for manual startup.

## Boot And Locale

The ISO is built by GRUB rescue media tooling and the build verifies that
the finished artifact has a x86_64 UEFI El Torito boot image in addition
to legacy BIOS boot support.

Stage 1 mounts the immutable squashfs as the lower layer and a tmpfs-backed
overlay upper layer as `/`, so the live system is read/write while running.
Those writes are currently ephemeral.

The default userspace locale is `en_US.UTF-8`, backed by `musl-locales`
and Noto fonts.
