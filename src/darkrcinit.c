#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t shutdown_action;
static pid_t rescue_getty_pid = -1;

static void reap_children(int sig)
{
	(void)sig;
}

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
		msg("darkrcinit: mkdir %s failed: %s", path, strerror(errno));
	chmod(path, mode);
}

static void make_node(const char *path, mode_t mode, dev_t dev)
{
	if (mknod(path, mode, dev) < 0 && errno != EEXIST)
		msg("darkrcinit: mknod %s failed: %s", path, strerror(errno));
	chmod(path, mode & 0777);
}

static void mount_one(const char *src, const char *target, const char *type,
		      unsigned long flags)
{
	if (mount(src, target, type, flags, "") < 0 && errno != EBUSY)
		msg("darkrcinit: mount %s on %s failed: %s", type, target,
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

static int run_openrc(const char *runlevel)
{
	char *const argv[] = { "openrc", (char *)runlevel, NULL };
	pid_t pid = fork();
	int status = 0;

	if (pid < 0) {
		msg("darkrcinit: fork openrc %s failed: %s", runlevel, strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execv("/sbin/openrc", argv);
		msg("darkrcinit: exec openrc %s failed: %s", runlevel, strerror(errno));
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0) {
		if (errno == EINTR)
			continue;
		msg("darkrcinit: wait openrc %s failed: %s", runlevel, strerror(errno));
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		msg("darkrcinit: openrc %s exited status=%d", runlevel, status);
	return status;
}

static void request_reboot(int sig)
{
	if (sig == SIGUSR1 || sig == SIGINT)
		shutdown_action = RB_AUTOBOOT;
	else
		shutdown_action = RB_POWER_OFF;
}

static void finish_shutdown(void)
{
	int action = shutdown_action;

	shutdown_action = 0;
	msg("darkrcinit: starting OpenRC shutdown");
	run_openrc("shutdown");
	sync();
	msg("darkrcinit: handing off to kernel shutdown");
	reboot(action);
	msg("darkrcinit: reboot syscall failed: %s", strerror(errno));
	for (;;)
		pause();
}

static void start_rescue_getty(void)
{
	pid_t pid;

	if (access("/etc/darkos/live-image", F_OK) != 0 || rescue_getty_pid > 0)
		return;
	pid = fork();
	if (pid < 0) {
		msg("darkrcinit: fork rescue getty failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		execl("/sbin/agetty", "agetty", "--noclear", "--autologin",
		      "dark", "38400", "tty2", "linux", (char *)0);
		msg("darkrcinit: exec rescue getty failed: %s", strerror(errno));
		_exit(127);
	}
	rescue_getty_pid = pid;
}

int main(void)
{
	setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
	setenv("LD_LIBRARY_PATH", "/lib:/usr/lib", 1);

	mkdir_p("/proc", 0555);
	mkdir_p("/sys", 0555);
	mkdir_p("/dev", 0755);
	mkdir_p("/run", 0755);
	mount_one("proc", "/proc", "proc", 0);
	mount_one("sysfs", "/sys", "sysfs", 0);
	mount_one("devtmpfs", "/dev", "devtmpfs", 0);
	mount_one("tmpfs", "/run", "tmpfs", 0);
	mkdir_p("/dev/pts", 0755);
	mount_one("devpts", "/dev/pts", "devpts", 0);
	make_node("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	make_node("/dev/null", S_IFCHR | 0666, makedev(1, 3));
	redirect_stdio();

	signal(SIGUSR1, request_reboot);
	signal(SIGUSR2, request_reboot);
	signal(SIGTERM, request_reboot);
	signal(SIGINT, request_reboot);
	signal(SIGCHLD, reap_children);

	msg("darkrcinit: starting DarkOS OpenRC services");
	run_openrc("sysinit");
	run_openrc("boot");
	run_openrc("default");
	msg("darkrcinit: default runlevel complete");

	for (;;) {
		int status;
		pid_t child;

		if (shutdown_action)
			finish_shutdown();
		start_rescue_getty();
		pause();
		while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
			if (child == rescue_getty_pid)
				rescue_getty_pid = -1;
		}
	}
}
