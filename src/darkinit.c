#define _GNU_SOURCE

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
		msg("darkinit: mkdir %s failed: %s", path, strerror(errno));
	chmod(path, mode);
}

static void mount_one(const char *src, const char *target, const char *type, unsigned long flags)
{
	if (mount(src, target, type, flags, "") < 0 && errno != EBUSY)
		msg("darkinit: mount %s on %s failed: %s", type, target, strerror(errno));
}

static void bind_file(const char *src, const char *target)
{
	if (mount(src, target, NULL, MS_BIND, "") < 0 && errno != EBUSY)
		msg("darkinit: bind %s on %s failed: %s", src, target, strerror(errno));
}

static void make_node(const char *path, mode_t mode, dev_t dev)
{
	if (mknod(path, mode, dev) < 0 && errno != EEXIST)
		msg("darkinit: mknod %s failed: %s", path, strerror(errno));
	chmod(path, mode & 0777);
}

static void write_file_if_missing(const char *path, const char *contents)
{
	int fd;

	if (access(path, F_OK) == 0)
		return;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		msg("darkinit: create %s failed: %s", path, strerror(errno));
		return;
	}
	if (write(fd, contents, strlen(contents)) < 0)
		msg("darkinit: write %s failed: %s", path, strerror(errno));
	close(fd);
}

static void write_file(const char *path, const char *contents)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0) {
		msg("darkinit: create %s failed: %s", path, strerror(errno));
		return;
	}
	if (write(fd, contents, strlen(contents)) < 0)
		msg("darkinit: write %s failed: %s", path, strerror(errno));
	close(fd);
}

static void append_file(const char *path, const char *contents)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);

	if (fd < 0) {
		msg("darkinit: open %s failed: %s", path, strerror(errno));
		return;
	}
	if (write(fd, contents, strlen(contents)) < 0)
		msg("darkinit: append %s failed: %s", path, strerror(errno));
	close(fd);
}

static void check_path(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		msg("darkinit: missing %s: %s", path, strerror(errno));
		return;
	}
	msg("darkinit: found %s mode=%o size=%lld", path, st.st_mode & 07777,
	    (long long)st.st_size);
}

static char xorg_input_conf[2048];

static const char *musl_loader(void)
{
	if (access("/lib/ld-musl-x86_64.so.1", X_OK) == 0)
		return "/lib/ld-musl-x86_64.so.1";
	if (access("/usr/lib/ld-musl-x86_64.so.1", X_OK) == 0)
		return "/usr/lib/ld-musl-x86_64.so.1";
	if (access("/usr/lib/libc.so.1", X_OK) == 0)
		return "/usr/lib/libc.so.1";
	return "/lib/ld-musl-x86_64.so.1";
}

static void ensure_musl_compat(void)
{
	mkdir_p("/lib", 0755);
	if (access("/lib/ld-musl-x86_64.so.1", X_OK) != 0) {
		if (access("/usr/lib/ld-musl-x86_64.so.1", X_OK) == 0)
			symlink("/usr/lib/ld-musl-x86_64.so.1", "/lib/ld-musl-x86_64.so.1");
		else if (access("/usr/lib/libc.so.1", X_OK) == 0)
			symlink("/usr/lib/libc.so.1", "/lib/ld-musl-x86_64.so.1");
	}
	if (access("/lib/libc.musl-x86_64.so.1", X_OK) != 0) {
		if (access("/usr/lib/libc.so.1", X_OK) == 0)
			symlink("/usr/lib/libc.so.1", "/lib/libc.musl-x86_64.so.1");
	}
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

static int run_wait(const char *label, const char *path, char *const argv[])
{
	pid_t pid = fork();
	int status = 0;

	if (pid < 0) {
		msg("darkinit: fork %s failed: %s", label, strerror(errno));
		return -1;
	}
	if (pid == 0) {
		setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
		execv(path, argv);
		msg("darkinit: exec %s failed: %s", label, strerror(errno));
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0) {
		msg("darkinit: wait %s failed: %s", label, strerror(errno));
		return -1;
	}
	msg("darkinit: %s exited status=%d", label, status);
	return status;
}

static void modprobe_one(const char *module)
{
	char *const argv[] = { "modprobe", "-q", (char *)module, NULL };
	run_wait(module, "/sbin/modprobe", argv);
}

static void start_udev(void)
{
	char *const udevd_argv[] = { "udevd", "--daemon", NULL };

	mkdir_p("/run/udev", 0755);
	mkdir_p("/run/udev/data", 0755);
	run_wait("udevd", "/sbin/udevd", udevd_argv);
}

static void trigger_udev(void)
{
	char *const trigger_argv[] = { "udevadm", "trigger", "--action=add", NULL };
	char *const settle_argv[] = { "udevadm", "settle", "--timeout=5", NULL };

	run_wait("udevadm trigger", "/bin/udevadm", trigger_argv);
	run_wait("udevadm settle", "/bin/udevadm", settle_argv);
}

static void log_file(const char *path)
{
	char buf[256];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0) {
		msg("darkinit: cannot read %s: %s", path, strerror(errno));
		return;
	}
	msg("darkinit: contents of %s:", path);
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = 0;
		msg("%s", buf);
	}
	close(fd);
}

static int wait_for_path(const char *path, int tenths)
{
	int i;

	for (i = 0; i < tenths; i++) {
		if (access(path, F_OK) == 0)
			return 0;
		usleep(100000);
	}
	return -1;
}

static int has_real_fb(void)
{
	char buf[64];
	int fd = open("/proc/fb", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	return buf[0] >= '0' && buf[0] <= '9';
}

static void ensure_fb0_from_proc(void)
{
	if (!has_real_fb())
		return;
	if (access("/dev/fb0", F_OK) != 0)
		make_node("/dev/fb0", S_IFCHR | 0660, makedev(29, 0));
}

static void probe_video(void)
{
	modprobe_one("drm");
	modprobe_one("drm_kms_helper");
	modprobe_one("simpledrm");
	modprobe_one("i915");
	modprobe_one("amdgpu");
	modprobe_one("radeon");
	modprobe_one("nouveau");
	modprobe_one("ast");
	modprobe_one("mgag200");
	modprobe_one("bochs");
	modprobe_one("qxl");
	modprobe_one("virtio_gpu");
	modprobe_one("cirrus_qemu");
	modprobe_one("vmwgfx");
	modprobe_one("vboxvideo");
	usleep(500000);
	ensure_fb0_from_proc();
	log_file("/proc/fb");
	check_path("/dev/fb0");
	check_path("/dev/dri/card0");
}

static void probe_input(void)
{
	const char *mods[] = {
		"serio_raw",
		"i8042",
		"libps2",
		"atkbd",
		"applespi",
		"cros_ec_keyb",
		"cypress_sf",
		"gpio_keys",
		"gpio_keys_polled",
		"hyperv_keyboard",
		"sparse_keymap",
		"matrix_keymap",
		"psmouse",
		"sermouse",
		"synaptics_usb",
		"synaptics_i2c",
		"evdev",
		"mousedev",
		"joydev",
		"uinput",
		"xhci_pci",
		"ehci_pci",
		"ohci_pci",
		"uhci_hcd",
		"hid_generic",
		"usbhid",
		"uhid",
		"hid_cherry",
		"hid_asus",
		"hid_lenovo",
		"hid_keytouch",
		"hid_prodikeys",
		"hid_multitouch",
		"hid_logitech",
		"hid_logitech_dj",
		"hid_logitech_hidpp",
		"hid_microsoft",
		"hid_apple",
		"hid_magicmouse",
		"wacom",
		"i2c_hid",
		"i2c_hid_acpi",
		"virtio_input",
		NULL
	};
	size_t i;

	for (i = 0; mods[i]; i++)
		modprobe_one(mods[i]);
	usleep(500000);
	mkdir_p("/dev/input", 0755);
	wait_for_path("/dev/input/event0", 20);
	wait_for_path("/dev/input/mice", 20);
	log_file("/proc/bus/input/devices");
	check_path("/dev/input/mice");
	check_path("/dev/input/mouse0");
	check_path("/dev/input/event0");
}

static void find_event_token(const char *line, char *out, size_t outsz)
{
	const char *event = strstr(line, "event");
	size_t i = 0;

	if (!event || out[0])
		return;
	while (event[i] && event[i] != ' ' && event[i] != '\t' && event[i] != '\n' &&
	       i + 1 < outsz) {
		out[i] = event[i];
		i++;
	}
	out[i] = 0;
}

static void consider_input_block(int has_kbd, int has_mouse, int has_abs,
				 int has_usb, const char *event, char *key,
				 size_t keysz, char *usb_key, size_t usb_keysz,
				 char *ptr, size_t ptrsz, char *abs_ptr,
				 size_t abs_ptrsz)
{
	if (event[0] == 0)
		return;
	if (has_kbd && key[0] == 0) {
		strncpy(key, event, keysz - 1);
		key[keysz - 1] = 0;
	}
	if (has_kbd && has_usb && usb_key[0] == 0) {
		strncpy(usb_key, event, usb_keysz - 1);
		usb_key[usb_keysz - 1] = 0;
	}
	if (has_mouse && has_abs && abs_ptr[0] == 0) {
		strncpy(abs_ptr, event, abs_ptrsz - 1);
		abs_ptr[abs_ptrsz - 1] = 0;
	}
	if (has_mouse && ptr[0] == 0) {
		strncpy(ptr, event, ptrsz - 1);
		ptr[ptrsz - 1] = 0;
	}
}

static void choose_input_events(char *key, size_t keysz, char *ptr, size_t ptrsz)
{
	FILE *fp = fopen("/proc/bus/input/devices", "r");
	char line[512];
	char event[32] = "";
	char usb_key[32] = "";
	char first_ptr[32] = "";
	char abs_ptr[32] = "";
	int has_kbd = 0;
	int has_mouse = 0;
	int has_abs = 0;
	int has_usb = 0;

	key[0] = 0;
	ptr[0] = 0;
	if (!fp)
		goto fallback;

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '\n') {
			consider_input_block(has_kbd, has_mouse, has_abs, has_usb,
					     event, key, keysz, usb_key,
					     sizeof(usb_key), first_ptr,
					     sizeof(first_ptr), abs_ptr,
					     sizeof(abs_ptr));
			event[0] = 0;
			has_kbd = has_mouse = has_abs = has_usb = 0;
			continue;
		}
		if (strncmp(line, "I:", 2) == 0 && strstr(line, "Bus=0003"))
			has_usb = 1;
		if (strncmp(line, "H:", 2) == 0) {
			if (strstr(line, "kbd"))
				has_kbd = 1;
			if (strstr(line, "mouse"))
				has_mouse = 1;
			find_event_token(line, event, sizeof(event));
		}
		if (strncmp(line, "B: ABS=", 7) == 0)
			has_abs = 1;
	}
	consider_input_block(has_kbd, has_mouse, has_abs, has_usb, event, key,
			     keysz, usb_key, sizeof(usb_key), first_ptr,
			     sizeof(first_ptr), abs_ptr, sizeof(abs_ptr));
	fclose(fp);

	if (usb_key[0]) {
		strncpy(key, usb_key, keysz - 1);
		key[keysz - 1] = 0;
	}
	if (abs_ptr[0]) {
		strncpy(ptr, abs_ptr, ptrsz - 1);
		ptr[ptrsz - 1] = 0;
	} else if (first_ptr[0]) {
		strncpy(ptr, first_ptr, ptrsz - 1);
		ptr[ptrsz - 1] = 0;
	}

fallback:
	if (key[0] == 0 && access("/dev/input/event0", F_OK) == 0)
		strncpy(key, "event0", keysz - 1);
	if (ptr[0] == 0 && access("/dev/input/event2", F_OK) == 0)
		strncpy(ptr, "event2", ptrsz - 1);
	if (ptr[0] == 0 && access("/dev/input/event1", F_OK) == 0)
		strncpy(ptr, "event1", ptrsz - 1);
	key[keysz - 1] = 0;
	ptr[ptrsz - 1] = 0;
}

static void write_input_config(void)
{
	char key[32];
	char ptr[32];

	xorg_input_conf[0] = 0;
	choose_input_events(key, sizeof(key), ptr, sizeof(ptr));
	if (key[0] == 0 || ptr[0] == 0) {
		msg("darkinit: Xorg static input fallback unavailable key=%s ptr=%s",
		    key, ptr);
		return;
	}

	snprintf(xorg_input_conf, sizeof(xorg_input_conf),
		 "Section \"ServerFlags\"\n"
		 "\tOption \"AutoAddDevices\" \"false\"\n"
		 "EndSection\n\n"
		 "Section \"InputDevice\"\n"
		 "\tIdentifier \"DarkOS Keyboard\"\n"
		 "\tDriver \"evdev\"\n"
		 "\tOption \"Device\" \"/dev/input/%s\"\n"
		 "\tOption \"CoreKeyboard\" \"true\"\n"
		 "\tOption \"XkbRules\" \"evdev\"\n"
		 "\tOption \"XkbModel\" \"pc105\"\n"
		 "\tOption \"XkbLayout\" \"us\"\n"
		 "EndSection\n\n"
		 "Section \"InputDevice\"\n"
		 "\tIdentifier \"DarkOS Pointer\"\n"
		 "\tDriver \"evdev\"\n"
		 "\tOption \"Device\" \"/dev/input/%s\"\n"
		 "\tOption \"CorePointer\" \"true\"\n"
		 "EndSection\n",
		 key, ptr);
	write_file("/tmp/xorg.conf.d/20-darkos-input.conf", xorg_input_conf);
	msg("darkinit: using Xorg input devices keyboard=/dev/input/%s pointer=/dev/input/%s",
	    key, ptr);
}

static void probe_audio(void)
{
	const char *mods[] = {
		"snd",
		"snd_pcm",
		"snd_seq",
		"snd_rawmidi",
		"snd_hda_intel",
		"snd_intel8x0",
		"snd_usb_audio",
		NULL
	};
	size_t i;

	for (i = 0; mods[i]; i++)
		modprobe_one(mods[i]);
	usleep(500000);
	mkdir_p("/dev/snd", 0755);
	wait_for_path("/dev/snd/controlC0", 20);
	log_file("/proc/asound/cards");
	check_path("/dev/snd/controlC0");
	check_path("/dev/snd/pcmC0D0p");
	check_path("/dev/snd/pcmC0D0c");
}

static void prepare_network_runtime(void)
{
	mkdir_p("/run/dhcpcd", 0755);
	mkdir_p("/run/resolvconf", 0755);
	mkdir_p("/run/resolvconf/interfaces", 0755);
	mkdir_p("/var/lib", 0755);
	mkdir_p("/var/lib/dhcpcd", 0755);
	mount_one("tmpfs", "/var/lib/dhcpcd", "tmpfs", 0);
	chmod("/var/lib/dhcpcd", 0755);
	write_file("/run/resolv.conf",
		   "# Generated by resolvconf\n"
		   "nameserver 10.0.2.3\n"
		   "nameserver 1.1.1.1\n"
		   "nameserver 8.8.8.8\n");
	if (access("/etc/resolv.conf", F_OK) == 0)
		bind_file("/run/resolv.conf", "/etc/resolv.conf");
}

static void prepare_home(void)
{
	mkdir_p("/root", 0700);
	mount_one("tmpfs", "/root", "tmpfs", 0);
	chmod("/root", 0700);
	mkdir_p("/root/.cache", 0700);
	mkdir_p("/root/.config", 0700);
	mkdir_p("/root/.local", 0700);
	mkdir_p("/root/.local/share", 0700);
	mkdir_p("/root/.mozilla", 0700);
}

static void start_rc_service(const char *service)
{
	pid_t pid = fork();

	if (pid < 0) {
		msg("darkinit: fork rc-service %s failed: %s", service, strerror(errno));
		return;
	}
	if (pid == 0) {
		setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
		setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
		execl("/sbin/rc-service", "rc-service", service, "start", (char *)0);
		msg("darkinit: exec rc-service %s failed: %s", service, strerror(errno));
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

static void start_network(void)
{
	msg("darkinit: starting dbus service");
	start_rc_service("dbus");
	msg("darkinit: starting networkmanager service");
	start_rc_service("networkmanager");
}

static void start_xorg(void)
{
	static const char *fbdev_conf =
		"Section \"ServerLayout\"\n"
		"\tIdentifier \"DarkOS Layout\"\n"
		"\tScreen \"Screen0\"\n"
		"\tInputDevice \"DarkOS Keyboard\" \"CoreKeyboard\"\n"
		"\tInputDevice \"DarkOS Pointer\" \"CorePointer\"\n"
		"EndSection\n\n"
		"Section \"Device\"\n"
		"\tIdentifier \"DarkOS Framebuffer\"\n"
		"\tDriver \"fbdev\"\n"
		"\tOption \"fbdev\" \"/dev/fb0\"\n"
		"EndSection\n\n"
		"Section \"Monitor\"\n"
		"\tIdentifier \"Monitor0\"\n"
		"EndSection\n\n"
		"Section \"Screen\"\n"
		"\tIdentifier \"Screen0\"\n"
		"\tDevice \"DarkOS Framebuffer\"\n"
		"\tMonitor \"Monitor0\"\n"
		"EndSection\n";
	static const char *modesetting_conf =
		"Section \"ServerLayout\"\n"
		"\tIdentifier \"DarkOS Layout\"\n"
		"\tScreen \"Screen0\"\n"
		"\tInputDevice \"DarkOS Keyboard\" \"CoreKeyboard\"\n"
		"\tInputDevice \"DarkOS Pointer\" \"CorePointer\"\n"
		"EndSection\n\n"
		"Section \"Device\"\n"
		"\tIdentifier \"DarkOS DRM\"\n"
		"\tDriver \"modesetting\"\n"
		"EndSection\n\n"
		"Section \"Monitor\"\n"
		"\tIdentifier \"Monitor0\"\n"
		"EndSection\n\n"
		"Section \"Screen\"\n"
		"\tIdentifier \"Screen0\"\n"
		"\tDevice \"DarkOS DRM\"\n"
		"\tMonitor \"Monitor0\"\n"
		"EndSection\n";
	static const char *vesa_conf =
		"Section \"ServerLayout\"\n"
		"\tIdentifier \"DarkOS Layout\"\n"
		"\tScreen \"Screen0\"\n"
		"\tInputDevice \"DarkOS Keyboard\" \"CoreKeyboard\"\n"
		"\tInputDevice \"DarkOS Pointer\" \"CorePointer\"\n"
		"EndSection\n\n"
		"Section \"Device\"\n"
		"\tIdentifier \"DarkOS VESA\"\n"
		"\tDriver \"vesa\"\n"
		"EndSection\n\n"
		"Section \"Monitor\"\n"
		"\tIdentifier \"Monitor0\"\n"
		"EndSection\n\n"
		"Section \"Screen\"\n"
		"\tIdentifier \"Screen0\"\n"
		"\tDevice \"DarkOS VESA\"\n"
		"\tMonitor \"Monitor0\"\n"
		"EndSection\n";
	pid_t pid = fork();

	if (pid < 0) {
		msg("darkinit: fork xinit failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		setsid();
		setenv("HOME", "/root", 1);
		setenv("USER", "root", 1);
		setenv("LOGNAME", "root", 1);
		setenv("SHELL", "/bin/sh", 1);
		setenv("LANG", "en_US.UTF-8", 1);
		setenv("LC_CTYPE", "en_US.UTF-8", 1);
		setenv("BROWSER", "chromium", 1);
		setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
		setenv("XDG_CACHE_HOME", "/root/.cache", 1);
		setenv("XDG_CONFIG_HOME", "/root/.config", 1);
		setenv("XDG_DATA_HOME", "/root/.local/share", 1);
		setenv("GDK_BACKEND", "x11,wayland", 1);
		setenv("OZONE_PLATFORM", "wayland", 1);
		
		setenv("GDK_GL", "disable", 1);
		setenv("LIBGL_ALWAYS_SOFTWARE", "0", 1);
		setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
		setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);
		setenv("WEBKIT_WEBGL_DISABLE_GBM", "1", 1);
		setenv("MESA_SHADER_CACHE_DIR", "/root/.cache/mesa", 1);
		setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);

		mount_one("tmpfs", "/tmp", "tmpfs", 0);
		chmod("/tmp", 01777);
		prepare_home();
		mkdir_p("/run/user", 0755);
		mkdir_p("/run/user/0", 0700);
		mkdir_p("/root/.cache", 0700);
		mkdir_p("/root/.cache/mesa", 0700);
		mkdir_p("/root/.config", 0700);
		mkdir_p("/root/.local", 0700);
		mkdir_p("/root/.local/share", 0700);
		mkdir_p("/root/.mozilla", 0700);
		mkdir_p("/tmp/.X11-unix", 01777);
		mkdir_p("/tmp/xorg.conf.d", 0755);
		mkdir_p("/etc/X11", 0755);
		mkdir_p("/etc/X11/xorg.conf.d", 0755);
		chown("/tmp", 0, 0);
		chown("/tmp/.X11-unix", 0, 0);
		unlink("/tmp/.X0-lock");
		unlink("/tmp/.tX0-lock");
		unlink("/etc/X11/xorg.conf.d/10-fbdev.conf");
		unlink("/usr/share/X11/xorg.conf.d/00-darkos-fbdev.conf");
		start_udev();
		probe_input();
		probe_audio();
		probe_video();
		trigger_udev();
		write_input_config();
		if (wait_for_path("/dev/dri/card0", 20) == 0) {
			msg("darkinit: using Xorg modesetting config");
			write_file("/tmp/xorg.conf", modesetting_conf);
		} else if (has_real_fb()) {
			ensure_fb0_from_proc();
			msg("darkinit: using Xorg fbdev config");
			write_file("/tmp/xorg.conf", fbdev_conf);
		} else {
			msg("darkinit: using Xorg vesa fallback config");
			write_file("/tmp/xorg.conf", vesa_conf);
		}
		if (xorg_input_conf[0]) {
			append_file("/tmp/xorg.conf", "\n");
			append_file("/tmp/xorg.conf", xorg_input_conf);
		}

		execl(musl_loader(), "ld-musl-x86_64.so.1",
		      "/usr/bin/xinit", "/etc/X11/xinit/xinitrc", "--",
		      "/usr/libexec/Xorg", ":0", "vt7", "-nolisten", "tcp",
		      "-config", "/tmp/xorg.conf", "-configdir", "/tmp/xorg.conf.d",
		      "-logfile", "/tmp/Xorg.0.log", (char *)0);
		msg("darkinit: exec xinit failed: %s", strerror(errno));
		_exit(127);
	}
}

int main(void)
{
	setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
	setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);
	setenv("LANG", "en_US.UTF-8", 1);
	setenv("LC_CTYPE", "en_US.UTF-8", 1);
	signal(SIGCHLD, SIG_DFL);

	mkdir_p("/proc", 0555);
	mkdir_p("/sys", 0555);
	mkdir_p("/dev", 0755);
	mkdir_p("/run", 0755);
	mkdir_p("/tmp", 01777);
	mkdir_p("/etc", 0755);
	mkdir_p("/var", 0755);
	mkdir_p("/var/log", 0755);

	mount_one("proc", "/proc", "proc", 0);
	mount_one("sysfs", "/sys", "sysfs", 0);
	mount_one("devtmpfs", "/dev", "devtmpfs", 0);
	mount_one("tmpfs", "/run", "tmpfs", 0);
	mount_one("tmpfs", "/tmp", "tmpfs", 0);
	mount_one("tmpfs", "/var/log", "tmpfs", 0);
	chmod("/run", 0755);
	chmod("/tmp", 01777);
	chmod("/var/log", 0755);
	mkdir_p("/dev/pts", 0755);
	mkdir_p("/dev/shm", 01777);
	mount_one("devpts", "/dev/pts", "devpts", 0);

	make_node("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	make_node("/dev/null", S_IFCHR | 0666, makedev(1, 3));
	redirect_stdio();
	ensure_musl_compat();
	write_file_if_missing("/etc/fstab",
			      "proc /proc proc defaults 0 0\n"
			      "sysfs /sys sysfs defaults 0 0\n"
			      "devpts /dev/pts devpts mode=0620 0 0\n"
			      "tmpfs /run tmpfs mode=0755 0 0\n"
			      "tmpfs /tmp tmpfs mode=1777 0 0\n"
			      "tmpfs /root tmpfs mode=0700 0 0\n"
			      "tmpfs /var/log tmpfs mode=0755 0 0\n");

	msg("darkinit: starting DarkOS");
	check_path("/lib/ld-musl-x86_64.so.1");
	check_path("/lib/libc.musl-x86_64.so.1");
	check_path("/usr/lib/ld-musl-x86_64.so.1");
	check_path("/usr/lib/libc.so.1");
	check_path("/sbin/openrc");
	check_path("/usr/lib/librc.so.1");
	check_path("/usr/lib/libeinfo.so.1");
	check_path("/usr/bin/network-up");
	prepare_home();
	prepare_network_runtime();
	start_network();
	msg("darkinit: starting Xorg directly");
	start_xorg();

	for (;;) {
		pause();
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
	}
}
