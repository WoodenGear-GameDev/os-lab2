#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "proclaunch.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

static int arg_needs_quotes(const char *arg) {
    if (arg[0] == '\0') {
        return 1;
    }
    for (const char *p = arg; *p != '\0'; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '"') {
            return 1;
        }
    }
    return 0;
}

static size_t append_quoted_arg(char *dst, const char *arg) {
    size_t n = 0;

    if (!arg_needs_quotes(arg)) {
        size_t len = strlen(arg);
        if (dst && len > 0) {
            memcpy(dst, arg, len);
        }
        return len;
    }

    if (dst) {
        dst[n] = '"';
    }
    ++n;

    const char *p = arg;
    while (*p != '\0') {
        size_t backslashes = 0;
        while (*p == '\\') {
            ++backslashes;
            ++p;
        }

        if (*p == '\0') {
            size_t count = backslashes * 2;
            if (dst && count > 0) {
                memset(dst + n, '\\', count);
            }
            n += count;
            break;
        } else if (*p == '"') {
            size_t count = backslashes * 2 + 1;
            if (dst) {
                memset(dst + n, '\\', count);
                dst[n + count] = '"';
            }
            n += count + 1;
            ++p;
        } else {
            if (dst && backslashes > 0) {
                memset(dst + n, '\\', backslashes);
            }
            n += backslashes;
            if (dst) {
                dst[n] = *p;
            }
            ++n;
            ++p;
        }
    }

    if (dst) {
        dst[n] = '"';
    }
    ++n;

    return n;
}

static char *build_command_line(const char *const argv[]) {
    size_t total = 0;
    for (size_t i = 0; argv[i] != NULL; ++i) {
        if (i > 0) {
            ++total;
        }
        total += append_quoted_arg(NULL, argv[i]);
    }

    char *cmd = (char *)malloc(total + 1);
    if (!cmd) {
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; argv[i] != NULL; ++i) {
        if (i > 0) {
            cmd[pos++] = ' ';
        }
        pos += append_quoted_arg(cmd + pos, argv[i]);
    }
    cmd[pos] = '\0';
    return cmd;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || argv[0][0] == '\0' || !proc) {
        return -1;
    }

    proc->handle = 0;
    proc->pid = -1;

    char *cmd = build_command_line(argv);
    if (!cmd) {
        return -1;
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessA(
        NULL,
        cmd,
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi
    );

    free(cmd);

    if (!ok) {
        return -1;
    }

    CloseHandle(pi.hThread);
    proc->handle = (intptr_t)pi.hProcess;
    proc->pid = (long)pi.dwProcessId;
    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || proc->handle == 0 || (HANDLE)proc->handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    HANDLE h = (HANDLE)proc->handle;
    DWORD wait_res = WaitForSingleObject(h, INFINITE);
    if (wait_res != WAIT_OBJECT_0) {
        return -1;
    }

    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    proc->handle = 0;
    proc->pid = -1;

    if (!ok) {
        return -1;
    }
    if (exit_code) {
        *exit_code = (int)code;
    }
    return 0;
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || proc->handle == 0 || (HANDLE)proc->handle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    HANDLE h = (HANDLE)proc->handle;
    DWORD wait_res = WaitForSingleObject(h, 0);
    if (wait_res == WAIT_TIMEOUT) {
        return 0;
    }
    if (wait_res != WAIT_OBJECT_0) {
        return -1;
    }

    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    proc->handle = 0;
    proc->pid = -1;

    if (!ok) {
        return -1;
    }
    if (exit_code) {
        *exit_code = (int)code;
    }
    return 1;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}

static int decode_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

int pl_start(const char *const argv[], pl_process *proc) {
    if (!argv || !argv[0] || argv[0][0] == '\0' || !proc) {
        errno = EINVAL;
        return -1;
    }

    proc->handle = 0;
    proc->pid = -1;

    int errpipe[2];
    if (pipe(errpipe) != 0) {
        return -1;
    }

    if (set_cloexec(errpipe[0]) != 0 || set_cloexec(errpipe[1]) != 0) {
        int saved_errno = errno;
        close(errpipe[0]);
        close(errpipe[1]);
        errno = saved_errno;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int saved_errno = errno;
        close(errpipe[0]);
        close(errpipe[1]);
        errno = saved_errno;
        return -1;
    }

    if (pid == 0) {
        close(errpipe[0]);
        execvp(argv[0], (char *const *)argv);

        int exec_err = errno;
        const char *p = (const char *)&exec_err;
        size_t rem = sizeof(exec_err);
        while (rem > 0) {
            ssize_t w = write(errpipe[1], p, rem);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            p += (size_t)w;
            rem -= (size_t)w;
        }
        close(errpipe[1]);
        _exit(127);
    }

    close(errpipe[1]);

    int child_err = 0;
    char *p = (char *)&child_err;
    size_t total = 0;
    while (total < sizeof(child_err)) {
        ssize_t r = read(errpipe[0], p + total, sizeof(child_err) - total);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            child_err = errno;
            total = sizeof(child_err);
            break;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    close(errpipe[0]);

    if (total > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        errno = (total == sizeof(child_err)) ? child_err : EIO;
        return -1;
    }

    proc->handle = 0;
    proc->pid = (long)pid;
    return 0;
}

int pl_wait(pl_process *proc, int *exit_code) {
    if (!proc || proc->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t)proc->pid, &status, 0);
    } while (r < 0 && errno == EINTR);

    if (r < 0) {
        return -1;
    }

    proc->pid = -1;
    if (exit_code) {
        *exit_code = decode_status(status);
    }
    return 0;
}

int pl_poll(pl_process *proc, int *exit_code) {
    if (!proc || proc->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t)proc->pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);

    if (r < 0) {
        return -1;
    }
    if (r == 0) {
        return 0;
    }

    proc->pid = -1;
    if (exit_code) {
        *exit_code = decode_status(status);
    }
    return 1;
}

#endif
