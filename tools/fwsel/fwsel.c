// fwsel: select the patched Wi-Fi firmware from a KernelSU preinit hook, with a
// one-strike rollback.
//
// Usage: fwsel <state_dir> <firmware_dir>
//
//   <state_dir>/failed   present: never select again (cleared by reinstalling)
//   <state_dir>/pending  present: the last selected boot was never confirmed by
//                        service.sh, so mark it failed and boot on stock firmware
//   otherwise            write pending, then point firmware_class.path at
//                        <firmware_dir>
//
// Runs before SELinux policy load with no shell, so it is a static nolibc
// binary. Every outcome exits 0 so the runner never aborts the module.

#include "nolibc.h"

#define FW_PATH_PARAM "/sys/module/firmware_class/parameters/path"
#define PATH_MAX_LEN 256

static void say(const char *msg)
{
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
}

static int join(char *out, const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    if (a + 1 + b + 1 > PATH_MAX_LEN)
        return -1;
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return 0;
}

static int write_file(const char *path, const char *data, int flags)
{
    int fd = open(path, flags | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    size_t len = strlen(data);
    int ok = write(fd, data, len) == (ssize_t)len;
    if (ok && (flags & O_CREAT))
        ok = fsync(fd) == 0;
    close(fd);
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    char pending[PATH_MAX_LEN], failed[PATH_MAX_LEN];

    if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/' ||
        join(pending, argv[1], "pending") || join(failed, argv[1], "failed")) {
        say("usage: fwsel <state_dir> <firmware_dir>");
        return 0;
    }
    mkdir(argv[1], 0700);

    if (access(failed, F_OK) == 0) {
        say("rolled back earlier; staying on stock firmware");
        return 0;
    }
    if (access(pending, F_OK) == 0) {
        write_file(failed, "unconfirmed boot\n", O_CREAT | O_TRUNC);
        unlink(pending);
        say("last patched boot was not confirmed; rolled back to stock firmware");
        return 0;
    }
    if (write_file(pending, "1\n", O_CREAT | O_TRUNC)) {
        say("cannot write pending marker; staying on stock firmware");
        return 0;
    }
    if (write_file(FW_PATH_PARAM, argv[2], 0)) {
        unlink(pending);
        say("cannot set firmware_class.path; staying on stock firmware");
        return 0;
    }
    say("patched firmware selected");
    return 0;
}
