/* worker.c -- the per-tab worker (the actual shell) */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* worker                                                             */
/* ------------------------------------------------------------------ */


int in_getc(void)
{
    unsigned char c;
    ssize_t n;

    for (;;) {
        n = read(g_in_fd, &c, 1);
        if (n == 1)
            return c;
        if (n == 0)
            return -1;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static void worker_emit_prompt(void)
{
    unsigned char m = PMARK_START;
    const char *pr = PROMPT_MAIN;

    emit_raw((char *)&m, 1);
    emit_raw(g_cwd, strlen(g_cwd));
    m = PMARK_SEP;
    emit_raw((char *)&m, 1);
    emit_raw(pr, strlen(pr));
    m = PMARK_END;
    emit_raw((char *)&m, 1);
}

char *worker_read_command(void)
{
    static char buf[MAX_CMD];
    int len = 0;

    for (;;) {
        int c = in_getc();
        if (c < 0)
            return NULL;
        if (c == '\n')
            break;
        if (len < MAX_CMD - 1)
            buf[len++] = (char)c;
    }
    buf[len] = '\0';
    /* undo the GUI's escaping: NL_MARK stands for a newline inside one command */
    for (int k = 0; k < len; k++)
        if ((unsigned char)buf[k] == NL_MARK)
            buf[k] = '\n';
    return xstrdup(buf);
}

void worker_loop(WorkerCtx *ctx)
{
    g_out_fd = ctx->out_fd;
    g_in_fd  = ctx->in_fd;
    g_ctl_fd = ctx->ctl_fd;
    g_hist   = ctx->hist;
    g_sh_self = (Shared *)ctx->sh;
    g_mode   = ctx->mode;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    /* Terminator itself must never stop */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    if (!g_cwd[0] && getcwd(g_cwd, sizeof g_cwd) == NULL)
        snprintf(g_cwd, sizeof g_cwd, "/");

    for (;;) {
        char *line;

        worker_emit_prompt();
        line = worker_read_command();
        if (!line)
            break;

        {
            char *p = line;
            while (*p && isspace((unsigned char)*p))
                p++;
            if (*p)
                execute_line(p);
        }
        free(line);

        reap_background_jobs();

        if (g_exit_requested)
            break;
    }

    for (int i = 0; i < g_job_count; i++)
        if (g_jobs[i].state == 0)
            kill(-g_jobs[i].pgid, SIGHUP);

    if (g_ctl_fd >= 0) {
        unsigned char b = CTL_EXIT;
        write_all(g_ctl_fd, &b, 1);
    }
}

void *worker_thread(void *p)
{
    worker_loop((WorkerCtx *)p);
    return NULL;
}
