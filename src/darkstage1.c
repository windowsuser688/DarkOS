#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/loop.h>

static void msg(const char *fmt, ...)
{
	va_list ap;
	FILE *fp = fopen("/dev/console", "a");
	if (!fp)
		fp = stderr;
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fputc('\n', fp);
	if (fp != stderr)
		fclose(fp);
}

static void mkdir_p(const char *path, mode_t mode)
{
	if (mkdir(path, mode) < 0 && errno != EEXIST)
		msg("darkstage1: mkdir %s failed: %s", path, strerror(errno));
	chmod(path, mode);
}

static void make_node(const char *path, mode_t mode, dev_t dev)
{
	if (mknod(path, mode, dev) < 0 && errno != EEXIST)
		msg("darkstage1: mknod %s failed: %s", path, strerror(errno));
	chmod(path, mode & 0777);
}

static void mount_one(const char *src, const char *target, const char *type, unsigned long flags)
{
	if (mount(src, target, type, flags, "") < 0 && errno != EBUSY)
		msg("darkstage1: mount %s on %s failed: %s", type, target, strerror(errno));
}

static void redirect_stdio(void)
{
	int fd = open("/dev/console", O_RDWR);
	if (fd < 0)
		return;
	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO)
		close(fd);
}

static int run_modprobe(const char *module)
{
	pid_t pid = fork();
	int status = 0;

	if (pid < 0) {
		msg("darkstage1: fork modprobe %s failed: %s", module, strerror(errno));
		return -1;
	}
	if (pid == 0) {
		setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
		execl("/sbin/modprobe", "modprobe", "-q", module, (char *)0);
		msg("darkstage1: exec modprobe %s failed: %s", module, strerror(errno));
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0) {
		msg("darkstage1: wait modprobe %s failed: %s", module, strerror(errno));
		return -1;
	}
	msg("darkstage1: modprobe %s status=%d", module, status);
	return status;
}

static void load_boot_modules(void)
{
	const char *mods[] = {
		"scsi_mod", "sd_mod", "cdrom", "sr_mod",
		"libata", "ahci", "ata_piix", "ata_generic", "pata_acpi",
		"virtio", "virtio_pci", "virtio_blk", "virtio_scsi",
		"nvme", "nvme_core", "vmd",
		"usbcore", "xhci_hcd", "xhci_pci", "ehci_hcd", "ehci_pci",
		"ohci_hcd", "ohci_pci", "uhci_hcd", "usb_storage", "uas",
		"isofs", "squashfs", "overlay", "loop", NULL
	};
	size_t i;

	for (i = 0; mods[i]; i++)
		run_modprobe(mods[i]);
}

static int try_mount_iso(const char *dev)
{
	if (mount(dev, "/media/cdrom", "iso9660", MS_RDONLY, "") == 0) {
		msg("darkstage1: mounted ISO from %s", dev);
		return 0;
	}
	msg("darkstage1: mount ISO %s failed: %s", dev, strerror(errno));
	return -1;
}

static int mount_iso(void)
{
	const char *devs[] = {
		"/dev/sr0", "/dev/sr1", "/dev/cdrom", "/dev/hdc",
		"/dev/sda", "/dev/sdb", "/dev/vda", "/dev/vdb",
		"/dev/xvda", "/dev/nvme0n1", "/dev/mmcblk0", NULL
	};
	int round;
	size_t i;

	for (round = 0; round < 80; round++) {
		for (i = 0; devs[i]; i++) {
			if (access(devs[i], F_OK) == 0 && try_mount_iso(devs[i]) == 0)
				return 0;
		}
		usleep(100000);
	}
	return -1;
}

static int attach_loop(const char *image, const char *loopdev)
{
	struct loop_info64 info;
	int img = -1;
	int loop = -1;
	int ret = -1;

	make_node("/dev/loop-control", S_IFCHR | 0600, makedev(10, 237));
	make_node(loopdev, S_IFBLK | 0600, makedev(7, 0));

	img = open(image, O_RDONLY | O_CLOEXEC);
	if (img < 0) {
		msg("darkstage1: open %s failed: %s", image, strerror(errno));
		goto out;
	}

	loop = open(loopdev, O_RDWR | O_CLOEXEC);
	if (loop < 0) {
		msg("darkstage1: open %s failed: %s", loopdev, strerror(errno));
		goto out;
	}

	if (ioctl(loop, LOOP_SET_FD, img) < 0) {
		msg("darkstage1: LOOP_SET_FD failed: %s", strerror(errno));
		goto out;
	}

	memset(&info, 0, sizeof(info));
	info.lo_flags = LO_FLAGS_READ_ONLY;
	strncpy((char *)info.lo_file_name, image, LO_NAME_SIZE - 1);
	if (ioctl(loop, LOOP_SET_STATUS64, &info) < 0)
		msg("darkstage1: LOOP_SET_STATUS64 failed: %s", strerror(errno));

	ret = 0;
out:
	if (loop >= 0)
		close(loop);
	if (img >= 0)
		close(img);
	return ret;
}

int main(void)
{
	setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
	setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
	signal(SIGCHLD, SIG_DFL);

	mkdir_p("/proc", 0555);
	mkdir_p("/sys", 0555);
	mkdir_p("/dev", 0755);
	mkdir_p("/run", 0755);
	mkdir_p("/tmp", 01777);
	mkdir_p("/media", 0755);
	mkdir_p("/media/cdrom", 0755);
	mkdir_p("/lowerroot", 0755);
	mkdir_p("/overlay", 0755);
	mkdir_p("/newroot", 0755);

	mount_one("proc", "/proc", "proc", 0);
	mount_one("sysfs", "/sys", "sysfs", 0);
	mount_one("devtmpfs", "/dev", "devtmpfs", 0);
	make_node("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	make_node("/dev/null", S_IFCHR | 0666, makedev(1, 3));
	redirect_stdio();

	msg("darkstage1: starting");
	load_boot_modules();

	if (mount_iso() < 0) {
		msg("darkstage1: could not mount ISO");
		for (;;)
			pause();
	}

	if (attach_loop("/media/cdrom/boot/darkos.squashfs", "/dev/loop0") < 0) {
		msg("darkstage1: could not attach squashfs loop");
		for (;;)
			pause();
	}

	if (mount("/dev/loop0", "/lowerroot", "squashfs", MS_RDONLY, "") < 0) {
		msg("darkstage1: mount squashfs failed: %s", strerror(errno));
		for (;;)
			pause();
	}

	msg("darkstage1: mounted DarkOS squashfs");
	if (mount("tmpfs", "/overlay", "tmpfs", 0, "mode=0755") < 0) {
		msg("darkstage1: mount overlay tmpfs failed: %s", strerror(errno));
		for (;;)
			pause();
	}
	mkdir_p("/overlay/upper", 0755);
	mkdir_p("/overlay/work", 0755);
	if (mount("overlay", "/newroot", "overlay", 0,
		  "lowerdir=/lowerroot,upperdir=/overlay/upper,workdir=/overlay/work") < 0) {
		msg("darkstage1: mount writable overlay failed: %s", strerror(errno));
		for (;;)
			pause();
	}

	{
		int testfd = open("/newroot/.darkos-rw-test", O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (testfd < 0) {
			msg("darkstage1: writable overlay test failed: %s", strerror(errno));
			for (;;)
				pause();
		}
		close(testfd);
		unlink("/newroot/.darkos-rw-test");
	}

	msg("darkstage1: mounted writable overlay root");
	if (chroot("/newroot") < 0 || chdir("/") < 0) {
		msg("darkstage1: chroot failed: %s", strerror(errno));
		for (;;)
			pause();
	}

	execl("/sbin/init", "init", (char *)0);
	msg("darkstage1: exec /sbin/init failed: %s", strerror(errno));
	for (;;)
		pause();
}
