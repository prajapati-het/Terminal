/* exec.c -- fork/exec, pipes, redirection and jobs */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* executor                                                           */
/* ------------------------------------------------------------------ */


__thread Shared *g_sh_self = NULL;      /* this worker's shared control block */

static void free_stage_args(Stage *st, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < st[i].argc; j++)
            free(st[i].args[j]);
}

void expand_globs(Stage *st)
{
    char *na[MAX_ARGS];
    int n = 0, i;

    for (i = 0; i < st->argc; i++) {
        if (st->has_glob[i]) {
            glob_t g;
            memset(&g, 0, sizeof g);
            if (glob_in_cwd(st->args[i], &g) == 0) {
                for (size_t j = 0; j < g.gl_pathc && n < MAX_ARGS - 1; j++)
                    na[n++] = xstrdup(g.gl_pathv[j]);
            } else {
                if (n < MAX_ARGS - 1)
                    na[n++] = xstrdup(st->args[i]);
            }
            globfree(&g);
        } else {
            if (n < MAX_ARGS - 1)
                na[n++] = xstrdup(st->args[i]);
        }
    }
    for (i = 0; i < n; i++)
        st->args[i] = na[i];
    st->argc = n;
    st->args[n] = NULL;
}

int open_redirections(Stage *st)
{
    int fd;

    if (st->input) {
        fd = open(st->input, O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "%s: %s: %s\n", APP_NAME, st->input,
                    strerror(errno));
            return 0;
        }
        if (dup2(fd, STDIN_FILENO) < 0) { close(fd); return 0; }
        close(fd);
    }
    if (st->output) {
        int fl = O_WRONLY | O_CREAT | (st->append_out ? O_APPEND : O_TRUNC);
        fd = open(st->output, fl, 0666);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "%s: %s: %s\n", APP_NAME, st->output,
                    strerror(errno));
            return 0;
        }
        if (dup2(fd, STDOUT_FILENO) < 0) { close(fd); return 0; }
        close(fd);
    }
    if (st->error) {
        int fl = O_WRONLY | O_CREAT | (st->append_err ? O_APPEND : O_TRUNC);
        fd = open(st->error, fl, 0666);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "%s: %s: %s\n", APP_NAME, st->error,
                    strerror(errno));
            return 0;
        }
        if (dup2(fd, STDERR_FILENO) < 0) { close(fd); return 0; }
        close(fd);
    }
    return 1;
}

void child_exec(Stage *st, int in_fd, int out_fd, int term_out, pid_t pgid,
                       const char *e_notfound, const char *e_noperm, const char *e_cannot)
{
    /* own process group so the GUI can signal the whole job */
    setpgid(0, pgid);

    /* every child runs in this tab's logical working directory */
    if (g_cwd[0])
        chdir(g_cwd);

    if (in_fd >= 0 && dup2(in_fd, STDIN_FILENO) < 0)
        _exit(126);
    if (out_fd >= 0) {
        if (dup2(out_fd, STDOUT_FILENO) < 0)
            _exit(126);
    } else if (term_out) {
        if (dup2(g_out_fd, STDOUT_FILENO) < 0)
            _exit(126);
    }
    if (term_out && dup2(g_out_fd, STDERR_FILENO) < 0)
        _exit(126);

    /* restore default signal dispositions for the child */
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);

    if (!open_redirections(st))
        _exit(1);

    /* builtins that must still work in a child (pipeline / redirection) */
    if (!strcmp(st->args[0], "cd")) {
        const char *p = st->argc > 1 ? st->args[1] : getenv("HOME");
        if (!p || chdir(p) < 0) {
            dprintf(STDERR_FILENO, "%s: cd: %s\n", APP_NAME,
                    p ? strerror(errno) : "HOME not set");
            _exit(1);
        }
        _exit(0);
    }
    if (!strcmp(st->args[0], "pwd")) {
        char b[PATH_MAX];
        if (getcwd(b, sizeof b))
            dprintf(STDOUT_FILENO, "%s\n", b);
        _exit(0);
    }
    if (!strcmp(st->args[0], "[") || !strcmp(st->args[0], "test")) {
        run_test_builtin(st->args, st->argc);
        _exit(g_last_status);
    }
    if (!strcmp(st->args[0], "export")) {
        for (int i = 1; i < st->argc; i++) {
            char *eq = strchr(st->args[i], '=');
            if (eq) { *eq = '\0'; setenv(st->args[i], eq + 1, 1); *eq = '='; }
        }
        _exit(0);
    }
    if (!strcmp(st->args[0], "history")) {
        char b[HIST_ENTRY];
        int total = history_count();
        int start = total > HIST_SHOW ? total - HIST_SHOW : 0;
        for (int i = start; i < total; i++)
            if (history_get(i, b, sizeof b))
                dprintf(STDOUT_FILENO, "%5d  %s\n", i + 1, b);
        _exit(0);
    }
    if (!strcmp(st->args[0], "exit"))
        _exit(st->argc > 1 ? atoi(st->args[1]) : g_last_status);

    execvp(st->args[0], st->args);

    {
        int e = errno;
        if (e == ENOENT)
            dprintf(STDERR_FILENO, "%s: %s: command not found\n", e_notfound,
                    st->args[0]);
        else if (e == EACCES)
            dprintf(STDERR_FILENO, "%s: %s: %s\n", e_noperm, st->args[0],
                    strerror(e));
        else
            dprintf(STDERR_FILENO, "%s: %s: %s\n", e_cannot, st->args[0],
                    strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
}

/*
 * Wait for a foreground job.  WUNTRACED makes a stopped child (Ctrl+Z) report
 * back immediately instead of blocking, so the shell can return to the prompt.
 * Returns 1 if the job was stopped, 0 if it finished.
 */
int wait_foreground(pid_t *pids, int n)
{
    int i, st, stopped = 0;

    for (i = 0; i < n; i++) {
        st = -1;
        while (waitpid(pids[i], &st, WUNTRACED) < 0) {
            if (errno == EINTR)
                continue;
            break;                 /* ECHILD: already reaped */
        }
        if (st != -1 && WIFSTOPPED(st))
            stopped = 1;
        if (i == n - 1) {
            if (st != -1 && WIFEXITED(st))
                g_last_status = WEXITSTATUS(st);
            else if (st != -1 && WIFSIGNALED(st))
                g_last_status = 128 + WTERMSIG(st);
            else if (st != -1 && WIFSTOPPED(st))
                g_last_status = 128 + WSTOPSIG(st);
            else
                g_last_status = 1;
        }
    }
    return stopped;
}

const char *job_state_str(int st)
{
    return st == JOB_STOPPED ? "Stopped" : st == JOB_DONE ? "Done" : "Running";
}

int job_add(pid_t *pids, int n, pid_t pgid, const char *cmd, int state, int bg)
{
    Job *jb;

    if (g_job_count >= MAX_JOBS)
        return 0;
    jb = &g_jobs[g_job_count++];
    memset(jb, 0, sizeof *jb);
    jb->id = g_next_job_id++;
    jb->pgid = pgid;
    jb->npids = n > MAX_STAGES ? MAX_STAGES : n;
    for (int i = 0; i < jb->npids; i++)
        jb->pids[i] = pids[i];
    jb->state = state;
    jb->bg = bg;
    snprintf(jb->cmd, sizeof jb->cmd, "%s", cmd ? cmd : "");
    /* store the command without a trailing '&' so listings can add their own */
    {
        int L = (int)strlen(jb->cmd);
        while (L > 0 && isspace((unsigned char)jb->cmd[L - 1]))
            L--;
        if (L > 0 && jb->cmd[L - 1] == '&') {
            L--;
            while (L > 0 && isspace((unsigned char)jb->cmd[L - 1]))
                L--;
        }
        jb->cmd[L] = '\0';
    }
    return jb->id;
}

/*
 * Resolve a job spec: NULL/""/"%"/"%+" = current, "%-" = previous,
 * "%N" or "N" = job N.  Returns the job id (or 0) and its table index.
 */
int job_pick(const char *spec, int *idx_out)
{
    int want, best = -1, prev = -1, i;

    *idx_out = -1;

    if (!spec || !*spec || !strcmp(spec, "%") || !strcmp(spec, "%+"))
        want = -2;
    else if (!strcmp(spec, "%-"))
        want = -3;
    else if (spec[0] == '%')
        want = atoi(spec + 1);
    else
        want = atoi(spec);

    for (i = 0; i < g_job_count; i++) {
        if (g_jobs[i].state == JOB_DONE)
            continue;
        if (best < 0 || g_jobs[i].id > g_jobs[best].id) { prev = best; best = i; }
        else if (prev < 0 || g_jobs[i].id > g_jobs[prev].id) { prev = i; }
    }

    if (want == -2) {
        if (best < 0) return 0;
        *idx_out = best;
        return g_jobs[best].id;
    }
    if (want == -3) {
        if (prev < 0) return 0;
        *idx_out = prev;
        return g_jobs[prev].id;
    }
    for (i = 0; i < g_job_count; i++) {
        if (g_jobs[i].id == want && g_jobs[i].state != JOB_DONE) {
            *idx_out = i;
            return g_jobs[i].id;
        }
    }
    return 0;
}

void reap_background_jobs(void)
{
    int j, k;

    for (j = 0; j < g_job_count; j++) {
        Job *jb = &g_jobs[j];
        if (jb->state != 0)
            continue;

        int all_done = 1;
        for (k = 0; k < jb->npids; k++) {
            int st;
            pid_t r = waitpid(jb->pids[k], &st, WNOHANG);
            if (r == 0)
                all_done = 0;
        }
        if (all_done) {
            jb->state = 1;
            emit("[%d] done  %s\n", jb->id, jb->cmd);
        }
    }
}

/*
 * Run a pipeline.  Every child inherits the tab's output pipe directly (its
 * stdout and stderr are pointed at g_out_fd), so the worker never has to sit
 * in read() while a child runs -- which is what lets a stopped child (Ctrl+Z)
 * be noticed immediately instead of deadlocking.
 */
int run_pipeline(Stage *st, int n)
{
    int pipes[MAX_STAGES][2];
    pid_t pids[MAX_STAGES];
    int npipes = n - 1;
    int background = st[n - 1].background;
    int i;

    for (i = 0; i < n; i++)
        expand_globs(&st[i]);

    /* Task 11: run shell scripts with our own interpreter, never /bin/bash */
    if (!background && n == 1 && !st[0].input && !st[0].output &&
        !st[0].error && st[0].argc >= 1) {
        char *script = NULL;
        int argstart = 0;

        if (is_interp_name(st[0].args[0]) && st[0].argc >= 2) {
            script = st[0].args[1];          /* bash script.sh / sh script.sh */
            argstart = 2;
        } else if (strchr(st[0].args[0], '/')) {
            script = st[0].args[0];          /* ./script.sh, /path/script.sh */
            argstart = 1;
        }
        if (script) {
            char full[PATH_MAX];
            resolve_path(script, full, sizeof full);
            if (looks_like_script(full)) {
                run_script_file(full, st[0].argc - argstart,
                                st[0].args + argstart, 0);
                free_stage_args(st, n);
                return 1;
            }
        }
    }

    /* single, unadorned builtin runs in the worker itself so that it can
     * change the shell's own state (cwd, history, jobs, ...) */
    if (!background && n == 1 && !st[0].input && !st[0].output &&
        !st[0].error && is_builtin(st[0].args[0])) {
        int r = run_builtin_parent(st[0].args, st[0].argc);
        free_stage_args(st, n);
        return r;
    }

    for (i = 0; i < npipes; i++) {
        if (pipe(pipes[i]) < 0) {
            emit("%s: pipe: %s\n", APP_NAME, strerror(errno));
            g_last_status = 1;
            for (int j = 0; j < i; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            free_stage_args(st, n);
            return 0;
        }
    }

    for (i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            emit("%s: fork: %s\n", APP_NAME, strerror(errno));
            for (int j = 0; j < npipes; j++) { close(pipes[j][0]); close(pipes[j][1]); }
            for (int j = 0; j < i; j++) {
                kill(pids[j], SIGKILL);
                waitpid(pids[j], NULL, 0);
            }
            g_last_status = 1;
            free_stage_args(st, n);
            return 0;
        }

        if (pid == 0) {
            int in = i > 0 ? pipes[i - 1][0] : -1;
            int out = i < n - 1 ? pipes[i][1] : -1;
            pid_t pg = i == 0 ? 0 : pids[0];

            for (int j = 0; j < npipes; j++) {
                if (j != i - 1) close(pipes[j][0]);
                if (j != i) close(pipes[j][1]);
            }

            child_exec(&st[i], in, out, 1, pg,
                       APP_NAME, APP_NAME, APP_NAME);
            _exit(127);
        }

        pids[i] = pid;
        setpgid(pid, i == 0 ? pid : pids[0]);
    }

    for (i = 0; i < npipes; i++) { close(pipes[i][0]); close(pipes[i][1]); }

    if (background) {
        int id = job_add(pids, n, pids[0], g_cur_cmd, JOB_RUNNING, 1);
        if (id)
            emit("[%d] %d\n", id, (int)pids[0]);
        else
            emit("%s: too many background jobs\n", APP_NAME);
        g_last_status = 0;
        free_stage_args(st, n);
        return 1;
    }

    if (g_sh_self)
        g_sh_self->fg_pgid = pids[0];

    if (wait_foreground(pids, n)) {
        /* the job was stopped (Ctrl+Z): keep it around for fg/bg */
        int id = job_add(pids, n, pids[0], g_cur_cmd, JOB_STOPPED, 1);
        if (id)
            emit("[%d]+  Stopped                 %s\n", id, g_cur_cmd);
    }

    if (g_sh_self)
        g_sh_self->fg_pgid = 0;

    free_stage_args(st, n);
    return 1;
}

/* command substitution backend: runs through /bin/sh so pipes/quoting work */
int run_capture(const char *cmd, char *out, size_t cap)
{
    int fds[2];
    pid_t pid;
    size_t len = 0;
    char buf[1024];
    ssize_t nr;
    int st;

    if (cap == 0)
        return 0;
    out[0] = '\0';

    if (pipe(fds) < 0)
        return 0;

    pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return 0;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (g_cwd[0])
            chdir(g_cwd);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    while ((nr = read(fds[0], buf, sizeof buf)) != 0) {
        if (nr < 0) { if (errno == EINTR) continue; break; }
        if (len + (size_t)nr < cap - 1) {
            memcpy(out + len, buf, (size_t)nr);
            len += (size_t)nr;
        }
    }
    out[len] = '\0';
    close(fds[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st))
        g_last_status = WEXITSTATUS(st);

    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return 1;
}


/* ------------------------------------------------------------------ */
/* working-directory aware globbing (thread-safe)                     */
/* ------------------------------------------------------------------ */

int glob_in_cwd(const char *pat, glob_t *g)
{
    char save[PATH_MAX];
    int r;

    if (pat[0] == '/')
        return glob(pat, 0, NULL, g);

    pthread_mutex_lock(&g_cwd_mtx);
    if (getcwd(save, sizeof save) == NULL)
        save[0] = '\0';
    if (g_cwd[0])
        chdir(g_cwd);
    r = glob(pat, 0, NULL, g);
    if (save[0])
        chdir(save);
    pthread_mutex_unlock(&g_cwd_mtx);
    return r;
}
