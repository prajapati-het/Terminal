/* multiwatch.c -- multiWatch and watch */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* multiWatch / watch                                                 */
/* ------------------------------------------------------------------ */

static void mw_dash(void)
{
    for (int i = 0; i < SEP_LEN; i++)
        emit("-");
    emit("\n");
}

/* split  ["a", "b"]  into the command strings; returns count or -1 */
static int split_items(const char *raw, char *items[], int max)
{
    const char *open = strchr(raw, '[');
    const char *close = strrchr(raw, ']');
    const char *p;
    size_t inlen, i;
    char buf[MAX_CMD];
    int bl = 0, n = 0;
    char q = 0;

    if (!open || !close || close < open)
        return -1;

    p = open + 1;
    inlen = (size_t)(close - p);

    for (i = 0; i <= inlen; i++) {
        char c = (i < inlen) ? p[i] : ',';

        if (q == 0 && c == ',') {
            char *a;
            int L;
            buf[bl] = '\0';
            a = buf;
            while (*a && isspace((unsigned char)*a)) a++;
            L = (int)strlen(a);
            while (L > 0 && isspace((unsigned char)a[L - 1])) a[--L] = '\0';
            if (L >= 2 && (a[0] == '"' || a[0] == '\'') && a[L - 1] == a[0]) {
                a[L - 1] = '\0';
                a++;
            }
            if (*a && n < max)
                items[n++] = xstrdup(a);
            bl = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (q == 0) q = c;
            else if (q == c) q = 0;
        }
        if (bl < (int)sizeof buf - 1)
            buf[bl++] = c;
    }
    return n;
}

void do_multiwatch(const char *arg)
{
    char *items[32];
    int ni = split_items(arg, items, 32);
    long stamp = (long)time(NULL);
    pid_t pgid;
    int i;

    if (ni <= 0) {
        emit("%s: multiWatch: expected [\"cmd\", ...]\n", APP_NAME);
        g_last_status = 2;
        return;
    }

    if (g_sh_self) { g_sh_self->mw_active = 1; g_sh_self->mw_cancel = 0; }

    for (;;) {
        struct pollfd pf[32];
        pid_t pids[32];
        int started[32], done[32];
        int remaining;

        if (g_sh_self && g_sh_self->mw_cancel)
            break;

        stamp = (long)time(NULL);
        pgid = 0;
        for (i = 0; i < ni; i++) {
            int p[2];
            pid_t pid;

            if (pipe(p) < 0) { emit("%s: pipe: %s\n", APP_NAME, strerror(errno)); break; }
            pid = fork();
            if (pid < 0) { close(p[0]); close(p[1]); emit("%s: fork: %s\n", APP_NAME, strerror(errno)); break; }
            if (pid == 0) {
                setpgid(0, pgid);
                dup2(p[1], STDOUT_FILENO);
                dup2(p[1], STDERR_FILENO);
                close(p[0]); close(p[1]);
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                signal(SIGPIPE, SIG_DFL);
                if (g_cwd[0]) chdir(g_cwd);
                execl("/bin/sh", "sh", "-c", items[i], (char *)NULL);
                _exit(127);
            }
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);
            close(p[1]);
            pf[i].fd = p[0]; pf[i].events = POLLIN; pf[i].revents = 0;
            started[i] = 0; done[i] = 0; pids[i] = pid;
        }

        if (g_sh_self) g_sh_self->mw_pgid = pgid;

        remaining = ni;
        {
            int last_nl = 1;
            while (remaining > 0) {
                int r;
                if (g_sh_self && g_sh_self->mw_cancel)
                    break;
                r = poll(pf, ni, 200);
                if (r < 0) { if (errno == EINTR) continue; break; }
                if (r == 0) continue;
                for (i = 0; i < ni; i++) {
                    char b[2048];
                    ssize_t nr;
                    if (done[i] || pf[i].fd < 0) continue;
                    if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                    nr = read(pf[i].fd, b, sizeof b);
                    if (nr > 0) {
                        if (!started[i]) {
                            emit("\"%s\", %ld:\n", items[i], stamp);
                            mw_dash();
                            started[i] = 1;
                        }
                        emit_raw(b, (size_t)nr);
                        last_nl = (b[nr - 1] == '\n');
                    } else {
                        if (!started[i]) {
                            emit("\"%s\", %ld:\n", items[i], stamp);
                            mw_dash();
                        }
                        mw_dash();
                        last_nl = 1;
                        close(pf[i].fd);
                        pf[i].fd = -1;
                        done[i] = 1;
                        remaining--;
                    }
                }
            }

            if (g_sh_self && g_sh_self->mw_cancel) {
                kill(-pgid, SIGINT);
                for (i = 0; i < ni; i++)
                    if (pf[i].fd >= 0) { close(pf[i].fd); pf[i].fd = -1; }
                for (i = 0; i < ni; i++)
                    while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR) {}
                if (!last_nl)
                    emit("\n");
                break;
            }
        }

        for (i = 0; i < ni; i++)
            while (waitpid(pids[i], NULL, 0) < 0 && errno == EINTR) {}

        /* wait out the remainder of the interval, but stay responsive */
        for (;;) {
            struct timespec ts = { 0, 100000000 };
            if (g_sh_self && g_sh_self->mw_cancel) break;
            if ((long)time(NULL) - stamp >= MW_INTERVAL) break;
            nanosleep(&ts, NULL);
        }
        if (g_sh_self && g_sh_self->mw_cancel)
            break;
    }

    if (g_sh_self) {
        g_sh_self->mw_active = 0;
        g_sh_self->mw_pgid = 0;
        g_sh_self->mw_cancel = 0;
    }
    g_last_status = 0;
    for (i = 0; i < ni; i++)
        free(items[i]);
}

void do_watch(const char *arg)
{
    char cmd[MAX_CMD];
    size_t L;
    int k;

    snprintf(cmd, sizeof cmd, "%s", arg);
    {
        char *a = cmd;
        while (*a && isspace((unsigned char)*a)) a++;
        L = strlen(a);
        while (L > 0 && isspace((unsigned char)a[L - 1])) a[--L] = '\0';
        if (L >= 2 && (a[0] == '"' || a[0] == '\'') && a[L - 1] == a[0]) {
            a[L - 1] = '\0';
            a++;
        }
        memmove(cmd, a, strlen(a) + 1);
    }
    if (!cmd[0]) {
        emit("%s: watch: command required\n", APP_NAME);
        g_last_status = 2;
        return;
    }

    if (g_sh_self) { g_sh_self->mw_active = 1; g_sh_self->mw_cancel = 0; }

    while (!(g_sh_self && g_sh_self->mw_cancel)) {
        pid_t pid;
        int st;

        emit("\f");
        emit("Every %ds: %s\n\n", MW_INTERVAL, cmd);

        pid = fork();
        if (pid == 0) {
            setpgid(0, 0);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);
            if (g_cwd[0]) chdir(g_cwd);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        if (pid < 0) { emit("%s: fork: %s\n", APP_NAME, strerror(errno)); break; }
        if (g_sh_self) g_sh_self->mw_pgid = pid;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (g_sh_self && g_sh_self->mw_cancel) break;

        for (k = 0; k < MW_INTERVAL * 10; k++) {
            struct timespec ts = { 0, 100000000 };
            if (g_sh_self && g_sh_self->mw_cancel) break;
            nanosleep(&ts, NULL);
        }
    }

    if (g_sh_self) {
        g_sh_self->mw_active = 0;
        g_sh_self->mw_pgid = 0;
        g_sh_self->mw_cancel = 0;
    }
    emit("\n");
    g_last_status = 0;
}

