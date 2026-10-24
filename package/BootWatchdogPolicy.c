// SPDX-License-Identifier: MIT
// The T733 policy updates launchd overrides after persistent Data mounts.
// The caller completes this operation before executing launchd.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

int sheng_boot_watchdog_policy(void *, uint64_t);

int
sheng_boot_watchdog_policy(void *process, uint64_t scratch)
{
    proc_t p = process;
    int disable_watchdogd = 0;
    int disable_locationd = 0;
    int disable_identityservicesd = 0;
    PE_parse_boot_argn("sheng-disable-watchdogd", &disable_watchdogd,
        sizeof(disable_watchdogd));
    PE_parse_boot_argn("t733-disable-locationd", &disable_locationd,
        sizeof(disable_locationd));
    PE_parse_boot_argn("t733-disable-identityservicesd", &disable_identityservicesd,
        sizeof(disable_identityservicesd));

    // The three overrides are regenerated before launchd reads persistent Data.
    char policy[512];
    int policy_length = snprintf(policy, sizeof(policy),
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "%s%s%s</dict></plist>\n",
        disable_watchdogd == 1 ?
            "<key>com.apple.watchdogd</key><true/>" : "",
        disable_locationd == 1 ?
            "<key>com.apple.locationd</key><true/>" : "",
        disable_identityservicesd == 1 ?
            "<key>com.apple.identityservicesd</key><true/>" : "");
    if (policy_length < 0 || (size_t)policy_length >= sizeof(policy)) {
        return EINVAL;
    }

    const char *directory = "/private/var/db/com.apple.xpc.launchd";
    const char *path = "/private/var/db/com.apple.xpc.launchd/disabled.plist";
    const char *marker = "/private/var/db/com.apple.xpc.launchd/disabled.migrated";
    int error = copyout(directory, scratch, strlen(directory) + 1);
    int32_t result = 0;
    int fd = -1;
    if (error) {
        goto done;
    }
    struct mkdir_args md = { .path = scratch, .mode = 0755 };
    error = mkdir(p, &md, &result);
    if (error && error != EEXIST) {
        goto done;
    }
    error = copyout(path, scratch, strlen(path) + 1);
    if (error) {
        goto done;
    }
    struct open_args op = {
        .path = scratch, .flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
        .mode = 0644
    };
    error = open(p, &op, &result);
    if (error) {
        goto done;
    }
    fd = result;

    // The SystemOS environment starts at scratch + 0x400.
    size_t bytes = strlen(policy);
    error = copyout(policy, scratch + 0x200, bytes);
    if (error) {
        goto done;
    }
    struct write_args wr = { .fd = fd, .cbuf = scratch + 0x200, .nbyte = bytes };
    user_ssize_t written = 0;
    error = write(p, &wr, &written);
    if (error || written != (user_ssize_t)bytes) {
        if (!error) {
            error = EIO;
        }
        goto done;
    }
    struct fchown_args owner = { .fd = fd, .uid = 0, .gid = 0 };
    error = fchown(p, &owner, &result);
    if (error) {
        goto done;
    }
    struct fchmod_args mode = { .fd = fd, .mode = 0644 };
    error = fchmod(p, &mode, &result);
    if (error) {
        goto done;
    }
    struct fsync_args flush = { .fd = fd };
    error = fsync(p, &flush, &result);
    if (error) {
        goto done;
    }
    struct close_args cl = { .fd = fd };
    error = sys_close(p, &cl, &result);
    fd = -1;
    if (error) {
        goto done;
    }

    // The empty marker selects launchd's modern override loader.
    error = copyout(marker, scratch, strlen(marker) + 1);
    if (error) {
        goto done;
    }
    struct open_args mark = {
        .path = scratch, .flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
        .mode = 0644
    };
    error = open(p, &mark, &result);
    if (!error) {
        fd = result;
    } else if (error == EEXIST) {
        error = 0;
    }

done:
    if (fd >= 0) {
        struct close_args close_request = { .fd = fd };
        int close_error = sys_close(p, &close_request, &result);
        if (!error) {
            error = close_error;
        }
    }
    printf("T733 launch policy: watchdogd=%d locationd=%d identityservicesd=%d "
        "disabled; status=%d\n", disable_watchdogd == 1, disable_locationd == 1,
        disable_identityservicesd == 1, error);
    return error;
}
