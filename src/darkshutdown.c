#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

extern void sync(void);

static int write_string(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t len = strlen(value);
    ssize_t rc = write(fd, value, len);
    int saved = errno;
    close(fd);
    if (rc != len) {
        errno = saved;
        return -1;
    }
    return 0;
}

static int path_endswith(const char *path, const char *suffix) {
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen)
        return 0;
    return strcmp(path + plen - slen, suffix) == 0;
}

static int init_is_darkrc(void) {
    char target[512];
    ssize_t len = readlink("/proc/1/exe", target, sizeof(target) - 1);
    if (len < 0)
        return 0;
    target[len] = '\0';
    return path_endswith(target, "/darkrcinit") || path_endswith(target, "/sbin/init");
}

static int exec_backend(const char *path, char *const argv[]) {
    if (access(path, X_OK) != 0)
        return -1;
    execv(path, argv);
    return -1;
}

static int action_is_reboot(const char *action) {
    return action && (
        strcmp(action, "reboot") == 0 ||
        strcmp(action, "--reboot") == 0 ||
        strcmp(action, "-r") == 0 ||
        strcmp(action, "boot") == 0
    );
}

static int action_is_poweroff(const char *action) {
    return action && (
        strcmp(action, "poweroff") == 0 ||
        strcmp(action, "--poweroff") == 0 ||
        strcmp(action, "-p") == 0 ||
        strcmp(action, "-h") == 0 ||
        strcmp(action, "halt") == 0
    );
}

static const char *normalize_action(const char *arg) {
    if (!arg)
        return NULL;
    const char *slash = strrchr(arg, '/');
    return slash ? slash + 1 : arg;
}

int main(int argc, char *argv[]) {
    const char *name = argv[0];
    const char *base = strrchr(name, '/');
    if (base)
        base++;
    else
        base = name;

    const char *action = NULL;
    if (argc > 1)
        action = normalize_action(argv[1]);

    int do_reboot = -1;
    if (strcmp(base, "reboot") == 0 || action_is_reboot(base)) {
        do_reboot = 1;
    } else if (strcmp(base, "poweroff") == 0 || action_is_poweroff(base)) {
        do_reboot = 0;
    } else if (strcmp(base, "shutdown") == 0) {
        if (action_is_reboot(action))
            do_reboot = 1;
        else
            do_reboot = 0;
    } else if (action_is_reboot(action)) {
        do_reboot = 1;
    } else if (action_is_poweroff(action)) {
        do_reboot = 0;
    }

    if (do_reboot < 0) {
        fprintf(stderr, "%s: unknown action\n", base);
        return 2;
    }

    sync();

    if (init_is_darkrc()) {
        kill(1, do_reboot ? SIGUSR1 : SIGUSR2);
        sleep(5);
    }

    if (exec_backend("/sbin/openrc-shutdown", (char *const[]){"openrc-shutdown", do_reboot ? "-r" : "-p", "now", NULL}) == 0) {
        return 0;
    }

    if (exec_backend("/sbin/shutdown", (char *const[]){"shutdown", do_reboot ? "-r" : "-h", "now", NULL}) == 0) {
        return 0;
    }

    write_string("/proc/sys/kernel/sysrq", "1");
    if (write_string("/proc/sysrq-trigger", do_reboot ? "b" : "o") == 0)
        return 0;

    fprintf(stderr, "%s: no shutdown backend available\n", base);
    return 1;
}
