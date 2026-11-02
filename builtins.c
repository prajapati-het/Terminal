/* builtins.c -- shell builtins */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* builtins                                                           */
/* ------------------------------------------------------------------ */

/*
 * Change this tab's working directory.  In thread mode every tab shares one
 * process, so we keep a per-tab logical cwd instead of relying on chdir();
 * child processes chdir() into it just before exec.
 */
int change_dir(const char *arg)
{
    char target[PATH_MAX];
    const char *p = arg;

    if (!p || !*p)
        p = getenv("HOME");
    if (!p) {
        emit("%s: cd: HOME not set\n", APP_NAME);
        return -1;
    }

    {
        int ok;

        if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
            const char *h = getenv("HOME");
            if (!h) { emit("%s: cd: HOME not set\n", APP_NAME); return -1; }
            ok = str_cat2(target, sizeof target, h, p + 1);
        } else if (p[0] == '/') {
            ok = str_cat2(target, sizeof target, p, NULL);
        } else {
            char base[PATH_MAX];
            ok = str_cat2(base, sizeof base, g_cwd, "/") &&
                 str_cat2(target, sizeof target, base, p);
        }
        if (!ok) {
            emit("%s: cd: %s: %s\n", APP_NAME, p, strerror(ENAMETOOLONG));
            return -1;
        }
    }

    pthread_mutex_lock(&g_cwd_mtx);
    if (chdir(target) != 0) {
        pthread_mutex_unlock(&g_cwd_mtx);
        emit("%s: cd: %s: %s\n", APP_NAME, p, strerror(errno));
        return -1;
    }
    {
        char real[PATH_MAX];
        if (getcwd(real, sizeof real))
            snprintf(g_cwd, sizeof g_cwd, "%s", real);
    }
    pthread_mutex_unlock(&g_cwd_mtx);
    return 0;
}



/* ---- [ ... ] and test ---- */

static int eval_test(char **a, int n)
{
    int neg = 0;
    struct stat sb;

    while (n > 0 && !strcmp(a[0], "!")) { neg = !neg; a++; n--; }

    if (n == 0) { g_last_status = neg ? 0 : 1; return 1; }

    if (n == 1) {
        int r = (a[0][0] != '\0');
        g_last_status = (r ^ neg) ? 0 : 1;
        return 1;
    }

    if (n == 2) {
        const char *op = a[0], *v = a[1];
        int r;
        if (!strcmp(op, "-f"))      r = (stat(v, &sb) == 0 && S_ISREG(sb.st_mode));
        else if (!strcmp(op, "-d")) r = (stat(v, &sb) == 0 && S_ISDIR(sb.st_mode));
        else if (!strcmp(op, "-e")) r = (stat(v, &sb) == 0);
        else if (!strcmp(op, "-r")) r = (access(v, R_OK) == 0);
        else if (!strcmp(op, "-w")) r = (access(v, W_OK) == 0);
        else if (!strcmp(op, "-x")) r = (access(v, X_OK) == 0);
        else if (!strcmp(op, "-s")) r = (stat(v, &sb) == 0 && sb.st_size > 0);
        else if (!strcmp(op, "-z")) r = (v[0] == '\0');
        else if (!strcmp(op, "-n")) r = (v[0] != '\0');
        else {
            emit("%s: test: unknown operator '%s'\n", APP_NAME, op);
            g_last_status = 2;
            return 1;
        }
        g_last_status = (r ^ neg) ? 0 : 1;
        return 1;
    }

    if (n == 3) {
        const char *l = a[0], *op = a[1], *r2 = a[2];
        int r;
        if (!strcmp(op, "=") || !strcmp(op, "==")) r = !strcmp(l, r2);
        else if (!strcmp(op, "!=")) r = strcmp(l, r2) != 0;
        else if (!strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-lt") ||
                 !strcmp(op, "-le") || !strcmp(op, "-gt") || !strcmp(op, "-ge")) {
            long x = strtol(l, NULL, 10), y = strtol(r2, NULL, 10);
            if (!strcmp(op, "-eq")) r = (x == y);
            else if (!strcmp(op, "-ne")) r = (x != y);
            else if (!strcmp(op, "-lt")) r = (x < y);
            else if (!strcmp(op, "-le")) r = (x <= y);
            else if (!strcmp(op, "-gt")) r = (x > y);
            else r = (x >= y);
        } else {
            emit("%s: test: unknown operator '%s'\n", APP_NAME, op);
            g_last_status = 2;
            return 1;
        }
        g_last_status = (r ^ neg) ? 0 : 1;
        return 1;
    }

    emit("%s: test: too many arguments\n", APP_NAME);
    g_last_status = 2;
    return 1;
}

int run_test_builtin(char **args, int argc)
{
    int n = argc - 1;
    char **a = args + 1;

    if (!strcmp(args[0], "[")) {
        if (n < 1 || strcmp(a[n - 1], "]")) {
            emit("%s: [: missing ']'\n", APP_NAME);
            g_last_status = 2;
            return 1;
        }
        n--;                             /* drop the closing ']' */
    }
    return eval_test(a, n);
}

int is_builtin(const char *name)
{
    static const char *b[] = {
        "cd", "exit", "history", "jobs", "fg", "bg", "source", ".",
        "export", "pwd", "clear", "terminator", "true", "false",
        "[", "test", NULL
    };
    for (int i = 0; b[i]; i++)
        if (!strcmp(name, b[i]))
            return 1;
    return 0;
}

int run_builtin_parent(char **args, int argc)
{
    const char *c = args[0];

    if (!strcmp(c, "cd")) {
        g_last_status = change_dir(argc > 1 ? args[1] : NULL) == 0 ? 0 : 1;
        return 1;
    }
    if (!strcmp(c, "exit")) {
        g_last_status = argc > 1 ? atoi(args[1]) : g_last_status;
        if (g_script_depth > 0)
            g_script_exit = 1;          /* stop the script, keep the shell */
        else
            g_exit_requested = 1;
        return 1;
    }
    if (!strcmp(c, "history")) {
        char b[HIST_ENTRY];
        int total = history_count();
        int start = total > HIST_SHOW ? total - HIST_SHOW : 0;
        for (int i = start; i < total; i++)
            if (history_get(i, b, sizeof b))
                emit("%5d  %s\n", i + 1, b);
        g_last_status = 0;
        return 1;
    }
    if (!strcmp(c, "pwd")) {
        emit("%s\n", g_cwd);
        g_last_status = 0;
        return 1;
    }
    if (!strcmp(c, "export")) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(args[i], '=');
            if (eq) { *eq = '\0'; setenv(args[i], eq + 1, 1); *eq = '='; }
        }
        g_last_status = 0;
        return 1;
    }
    if (!strcmp(c, "jobs")) {
        int best = -1, prev = -1, i;
        for (i = 0; i < g_job_count; i++) {
            if (g_jobs[i].state == JOB_DONE) continue;
            if (best < 0 || g_jobs[i].id > g_jobs[best].id) { prev = best; best = i; }
            else if (prev < 0 || g_jobs[i].id > g_jobs[prev].id) { prev = i; }
        }
        for (i = 0; i < g_job_count; i++) {
            Job *jb = &g_jobs[i];
            char m;
            if (jb->state == JOB_DONE) continue;
            m = (i == best) ? '+' : (i == prev) ? '-' : ' ';
            emit("[%d]%c  %-24s%s%s\n", jb->id, m, job_state_str(jb->state),
                 jb->cmd, (jb->bg && jb->state == JOB_RUNNING) ? " &" : "");
        }
        g_last_status = 0;
        return 1;
    }
    if (!strcmp(c, "bg")) {
        int ix;
        if (job_pick(argc > 1 ? args[1] : NULL, &ix) == 0) {
            emit("%s: bg: no such job\n", APP_NAME);
            g_last_status = 1;
            return 1;
        }
        kill(-g_jobs[ix].pgid, SIGCONT);
        g_jobs[ix].state = JOB_RUNNING;
        g_jobs[ix].bg = 1;
        emit("[%d]+ %s &\n", g_jobs[ix].id, g_jobs[ix].cmd);
        g_last_status = 0;
        return 1;
    }
    if (!strcmp(c, "fg")) {
        int ix;
        if (job_pick(argc > 1 ? args[1] : NULL, &ix) == 0) {
            emit("%s: fg: no such job\n", APP_NAME);
            g_last_status = 1;
            return 1;
        }
        emit("%s\n", g_jobs[ix].cmd);
        g_jobs[ix].bg = 0;
        if (g_sh_self) g_sh_self->fg_pgid = g_jobs[ix].pgid;
        kill(-g_jobs[ix].pgid, SIGCONT);
        if (wait_foreground(g_jobs[ix].pids, g_jobs[ix].npids)) {
            g_jobs[ix].state = JOB_STOPPED;
            emit("[%d]+  Stopped                 %s\n", g_jobs[ix].id, g_jobs[ix].cmd);
        } else {
            g_jobs[ix].state = JOB_DONE;
        }
        if (g_sh_self) g_sh_self->fg_pgid = 0;
        return 1;
    }
    if (!strcmp(c, "clear")) {
        emit("\f");
        g_last_status = 0;
        return 1;
    }
    if (!strcmp(c, "[") || !strcmp(c, "test"))
        return run_test_builtin(args, argc);
    if (!strcmp(c, "true")) { g_last_status = 0; return 1; }
    if (!strcmp(c, "false")) { g_last_status = 1; return 1; }
    if (!strcmp(c, "terminator")) {
        const char *sub = argc > 1 ? args[1] : "";
        if (!strcmp(sub, "keybind")) {
            char err[160];
            if (argc < 4) {
                emit("usage: terminator keybind <keyspec> <action>\n");
                g_last_status = 2;
                return 1;
            }
            if (!bindings_add_user(args[2], args[3], err, sizeof err)) {
                emit("%s: keybind: %s (keeping default)\n", APP_NAME, err);
                g_last_status = 1;
                return 1;
            }
            bindings_save(config_path());
            if (g_mode == MODE_PROCESS && g_ctl_fd >= 0) {
                unsigned char t = CTL_BIND;
                unsigned short l1 = (unsigned short)strlen(args[2]);
                unsigned short l2 = (unsigned short)strlen(args[3]);
                write_all(g_ctl_fd, &t, 1);
                write_all(g_ctl_fd, &l1, sizeof l1);
                write_all(g_ctl_fd, args[2], l1);
                write_all(g_ctl_fd, &l2, sizeof l2);
                write_all(g_ctl_fd, args[3], l2);
            }
            emit("%s: bound %s to %s (saved to %s)\n", APP_NAME, args[2], args[3], config_path());
            g_last_status = 0;
            return 1;
        }
        if (!strcmp(sub, "keybinds")) {
            emit("user bindings:\n");
            for (int i = 0; i < g_n_user_binds; i++) {
                const char *n = XKeysymToString(g_user_binds[i].ks);
                emit("  %s%s%s%-12s -> %s\n",
                     (g_user_binds[i].mods & ControlMask) ? "Ctrl+" : "",
                     (g_user_binds[i].mods & ShiftMask) ? "Shift+" : "",
                     (g_user_binds[i].mods & Mod1Mask) ? "Alt+" : "",
                     n ? n : "?", action_name(g_user_binds[i].act));
            }
            emit("defaults:\n");
            for (int i = 0; i < g_n_def_binds; i++) {
                const char *n = XKeysymToString(g_def_binds[i].ks);
                emit("  %s%s%s%-12s -> %s\n",
                     (g_def_binds[i].mods & ControlMask) ? "Ctrl+" : "",
                     (g_def_binds[i].mods & ShiftMask) ? "Shift+" : "",
                     (g_def_binds[i].mods & Mod1Mask) ? "Alt+" : "",
                     n ? n : "?", action_name(g_def_binds[i].act));
            }
            g_last_status = 0;
            return 1;
        }
        if (!strcmp(sub, "reload")) {
            bindings_load(config_path(), 0);
            if (g_ctl_fd >= 0) {
                unsigned char t = CTL_RELOAD;
                write_all(g_ctl_fd, &t, 1);
            }
            emit("%s: configuration reloaded\n", APP_NAME);
            g_last_status = 0;
            return 1;
        }
        if (!strcmp(sub, "history") && argc > 2 && !strcmp(args[2], "clear")) {
            history_clear_all();
            emit("%s: history cleared\n", APP_NAME);
            g_last_status = 0;
            return 1;
        }
        emit("%s: terminator: unknown subcommand '%s'\n", APP_NAME, sub);
        g_last_status = 2;
        return 1;
    }
    if (!strcmp(c, "source") || !strcmp(c, ".")) {
        char full[PATH_MAX];
        if (argc < 2) {
            emit("%s: source: filename required\n", APP_NAME);
            g_last_status = 2;
            return 1;
        }
        resolve_path(args[1], full, sizeof full);
        run_script_file(full, argc - 2, args + 2, 1 /* sourced */);
        return 1;
    }
    return 0;
}
