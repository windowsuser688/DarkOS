#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
		msg("darkdiskinit: mkdir %s failed: %s", path, strerror(errno));
	chmod(path, mode);
}

static void make_node(const char *path, mode_t mode, dev_t dev)
{
	if (mknod(path, mode, dev) < 0 && errno != EEXIST)
		msg("darkdiskinit: mknod %s failed: %s", path, strerror(errno));
	chmod(path, mode & 0777);
}

static void mount_one(const char *src, const char *target, const char *type,
		      unsigned long flags)
{
	if (mount(src, target, type, flags, "") < 0 && errno != EBUSY)
		msg("darkdiskinit: mount %s on %s failed: %s", type, target,
		    strerror(errno));
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
		msg("darkdiskinit: fork modprobe %s failed: %s", module,
		    strerror(errno));
		return -1;
	}
	if (pid == 0) {
		setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
		execl("/sbin/modprobe", "modprobe", "-q", module, (char *)0);
		msg("darkdiskinit: exec modprobe %s failed: %s", module,
		    strerror(errno));
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0) {
		msg("darkdiskinit: wait modprobe %s failed: %s", module,
		    strerror(errno));
		return -1;
	}
	return status;
}

static void load_boot_modules(void)
{
	const char *mods[] = {
		"scsi_mod", "sd_mod", "sr_mod", "cdrom",
		"libata", "ahci", "ata_piix", "ata_generic", "pata_acpi",
		"virtio", "virtio_pci", "virtio_blk", "virtio_scsi",
		"nvme", "nvme_core", "vmd",
		"usbcore", "xhci_hcd", "xhci_pci", "ehci_hcd", "ehci_pci",
		"ohci_hcd", "ohci_pci", "uhci_hcd", "usb_storage", "uas",
		"mmc_core", "sdhci", "sdhci_pci", "sdhci_acpi",
		"hid", "hid_generic", "usbhid",
		"ext4", "jbd2", "mbcache", NULL
	};
	size_t i;

	for (i = 0; mods[i]; i++)
		run_modprobe(mods[i]);
}

static void read_cmdline(char *root, size_t rootsz, char *fstype,
			 size_t fstypesz, char *init, size_t initsz)
{
	char buf[4096];
	int fd;
	ssize_t n;
	char *tok;

	root[0] = 0;
	snprintf(fstype, fstypesz, "%s", "ext4");
	snprintf(init, initsz, "%s", "/sbin/init");

	fd = open("/proc/cmdline", O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;

	for (tok = strtok(buf, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
		if (strncmp(tok, "root=", 5) == 0)
			snprintf(root, rootsz, "%s", tok + 5);
		else if (strncmp(tok, "rootfstype=", 11) == 0)
			snprintf(fstype, fstypesz, "%s", tok + 11);
		else if (strncmp(tok, "init=", 5) == 0)
			snprintf(init, initsz, "%s", tok + 5);
	}
}

static char *trim_line(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = 0;
	return s;
}

static int read_uevent_value(const char *block, const char *key, char *out,
			     size_t outsz)
{
	char path[512];
	char line[256];
	FILE *fp;
	size_t keylen = strlen(key);

	snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", block);
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char *value;

		if (strncmp(line, key, keylen) != 0 || line[keylen] != '=')
			continue;
		value = trim_line(line + keylen + 1);
		snprintf(out, outsz, "%s", value);
		fclose(fp);
		return 0;
	}
	fclose(fp);
	return -1;
}

static void create_block_node_from_sysfs(const char *block, const char *devname)
{
	char major_s[32] = "";
	char minor_s[32] = "";
	char path[256];
	long major;
	long minor;

	if (read_uevent_value(block, "MAJOR", major_s, sizeof(major_s)) < 0 ||
	    read_uevent_value(block, "MINOR", minor_s, sizeof(minor_s)) < 0)
		return;
	major = strtol(major_s, NULL, 10);
	minor = strtol(minor_s, NULL, 10);
	if (major <= 0 || minor < 0)
		return;
	snprintf(path, sizeof(path), "/dev/%s", devname);
	if (access(path, F_OK) != 0)
		make_node(path, S_IFBLK | 0600, makedev((unsigned)major,
							(unsigned)minor));
}

static int find_partuuid(const char *uuid, char *out, size_t outsz)
{
	DIR *dir = opendir("/sys/class/block");
	struct dirent *de;

	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		char found_uuid[128] = "";
		char devname[240] = "";

		if (de->d_name[0] == '.')
			continue;
		if (read_uevent_value(de->d_name, "PARTUUID", found_uuid,
				      sizeof(found_uuid)) < 0)
			continue;
		if (strcasecmp(found_uuid, uuid) != 0)
			continue;
		if (read_uevent_value(de->d_name, "DEVNAME", devname,
				      sizeof(devname)) < 0) {
			strncpy(devname, de->d_name, sizeof(devname) - 1);
			devname[sizeof(devname) - 1] = 0;
		}
		create_block_node_from_sysfs(de->d_name, devname);
		snprintf(out, outsz, "/dev/%s", devname);
		closedir(dir);
		return 0;
	}

	closedir(dir);
	return -1;
}

static int resolve_root(const char *root, char *dev, size_t devsz)
{
	const char *fallback[] = {
		"/dev/sda3", "/dev/vda3", "/dev/xvda3", "/dev/nvme0n1p3",
		"/dev/mmcblk0p3", NULL
	};
	int round;
	size_t i;

	for (round = 0; round < 120; round++) {
		if (strncmp(root, "PARTUUID=", 9) == 0) {
			if (find_partuuid(root + 9, dev, devsz) == 0 &&
			    access(dev, F_OK) == 0)
				return 0;
		} else if (strncmp(root, "/dev/", 5) == 0) {
			if (access(root, F_OK) == 0) {
				snprintf(dev, devsz, "%s", root);
				return 0;
			}
		} else if (root[0] == 0) {
			for (i = 0; fallback[i]; i++) {
				if (access(fallback[i], F_OK) == 0) {
					snprintf(dev, devsz, "%s", fallback[i]);
					return 0;
				}
			}
		}
		usleep(100000);
	}
	return -1;
}

static void move_mount_dir(const char *old, const char *new)
{
	mkdir_p(new, 0755);
	if (mount(old, new, NULL, MS_MOVE, "") < 0)
		msg("darkdiskinit: move %s to %s failed: %s", old, new,
		    strerror(errno));
}

int main(void)
{
	char root[256];
	char fstype[64];
	char init[128];
	char rootdev[256];

	setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
	setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
	signal(SIGCHLD, SIG_DFL);

	mkdir_p("/proc", 0555);
	mkdir_p("/sys", 0555);
	mkdir_p("/dev", 0755);
	mkdir_p("/run", 0755);
	mkdir_p("/newroot", 0755);
	mount_one("proc", "/proc", "proc", 0);
	mount_one("sysfs", "/sys", "sysfs", 0);
	mount_one("devtmpfs", "/dev", "devtmpfs", 0);
	mount_one("tmpfs", "/run", "tmpfs", 0);
	make_node("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	make_node("/dev/null", S_IFCHR | 0666, makedev(1, 3));
	redirect_stdio();

	msg("darkdiskinit: starting installed DarkOS handoff");
	read_cmdline(root, sizeof(root), fstype, sizeof(fstype), init,
		     sizeof(init));
	load_boot_modules();

	if (resolve_root(root, rootdev, sizeof(rootdev)) < 0) {
		msg("darkdiskinit: could not resolve root=%s", root);
		for (;;)
			pause();
	}
	msg("darkdiskinit: mounting %s as %s", rootdev, fstype);
	if (mount(rootdev, "/newroot", fstype, MS_RELATIME, "") < 0) {
		msg("darkdiskinit: mount root failed: %s", strerror(errno));
		for (;;)
			pause();
	}

	move_mount_dir("/proc", "/newroot/proc");
	move_mount_dir("/sys", "/newroot/sys");
	move_mount_dir("/dev", "/newroot/dev");
	move_mount_dir("/run", "/newroot/run");

	if (chdir("/newroot") < 0 || mount(".", "/", NULL, MS_MOVE, "") < 0 ||
	    chroot(".") < 0 || chdir("/") < 0) {
		msg("darkdiskinit: switch to rootfs failed: %s", strerror(errno));
		for (;;)
			pause();
	}

	msg("darkdiskinit: exec %s", init);
	execl(init, "init", (char *)0);
	msg("darkdiskinit: exec %s failed: %s", init, strerror(errno));
	for (;;)
		pause();
}
