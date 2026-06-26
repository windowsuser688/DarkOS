SHELL := /bin/sh

ARCH ?= x86_64
ALPINE_VERSION ?= edge
MIRROR ?= https://dl-cdn.alpinelinux.org/alpine
QEMU ?= qemu-system-x86_64
QEMU_IMG ?= qemu-img
QEMU_DISK ?= out/darkos-disk.qcow2
QEMU_DISK_SIZE ?= 16G

.PHONY: all rootfs iso disk run clean darkelf darkinit darkstage1 darkdiskinit darkrcinit

all: iso

darkelf:
	mkdir -p out
	$(CC) -std=c99 -Wall -Wextra -O2 -o out/darkelf src/darkelf.c

darkinit:
	mkdir -p out
	$(CC) -std=c99 -Wall -Wextra -Os -static -o out/darkinit src/darkinit.c

darkstage1:
	mkdir -p out
	$(CC) -std=c99 -Wall -Wextra -Os -static -o out/darkstage1 src/darkstage1.c

darkdiskinit:
	mkdir -p out
	$(CC) -std=c99 -Wall -Wextra -Os -static -o out/darkdiskinit src/darkdiskinit.c

darkshutdown:
	mkdir -p out
	$(CC) -std=c99 -Wall -Wextra -Os -static -o out/darkshutdown src/darkshutdown.c

darkrcinit:
	mkdir -p out
	$(CC) -std=c99 -Wall -Wextra -Os -static -o out/darkrcinit src/darkrcinit.c

rootfs: darkelf darkinit darkdiskinit darkrcinit darkshutdown
	ARCH="$(ARCH)" ALPINE_VERSION="$(ALPINE_VERSION)" MIRROR="$(MIRROR)" scripts/mkrootfs.sh

iso: rootfs darkstage1
	ARCH="$(ARCH)" ALPINE_VERSION="$(ALPINE_VERSION)" MIRROR="$(MIRROR)" scripts/mkiso.sh

disk:
	@mkdir -p out
	@if [ ! -f "$(QEMU_DISK)" ]; then \
		command -v "$(QEMU_IMG)" >/dev/null 2>&1 || { echo "$(QEMU_IMG) is missing; install qemu-img"; exit 1; }; \
		echo "creating DarkOS install disk: $(QEMU_DISK) ($(QEMU_DISK_SIZE))"; \
		"$(QEMU_IMG)" create -f qcow2 "$(QEMU_DISK)" "$(QEMU_DISK_SIZE)"; \
	else \
		echo "using existing DarkOS install disk: $(QEMU_DISK)"; \
	fi

run: disk
	@test -f out/darkos.iso || { echo "out/darkos.iso is missing; run 'make iso' first"; exit 1; }
	$(QEMU) -m 4096 -machine accel=kvm:tcg -device bochs-display -device qemu-xhci -device usb-tablet -device usb-kbd -audiodev none,id=audio0 -device ich9-intel-hda -device hda-micro,audiodev=audio0 -netdev user,id=net0 -device e1000,netdev=net0 -boot order=d,menu=on -cdrom out/darkos.iso -drive file="$(QEMU_DISK)",format=qcow2,if=ide,media=disk

clean:
	rm -rf build out
