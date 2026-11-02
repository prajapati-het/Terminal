/* history.c -- shared command history and its file */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* shared history                                                     */
/* ------------------------------------------------------------------ */

void history_init(int mode)
{
    if (mode == MODE_PROCESS) {
        void *m = mmap(NULL, sizeof(HistStore), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            fprintf(stderr, "%s: shared history mmap failed: %s\n",
                    APP_NAME, strerror(errno));
            exit(1);
        }
        g_hist = m;
        sem_init(&g_hist->sem, 1, 1);   /* process-shared */
    } else {
        g_hist = &g_hist_local;
        sem_init(&g_hist->sem, 0, 1);   /* thread-shared  */
    }
    g_hist->count = 0;
    g_hist->head  = 0;
}

/* path of the shared history file, e.g. ~/.terminator_history */
static const char *history_path(void)
{
    static char p[PATH_MAX];
    const char *h = getenv("HOME");
    snprintf(p, sizeof p, "%s/.terminator_history", h ? h : ".");
    return p;
}

/* append one command to the history file (called with the semaphore held) */
static void hist_file_append(const char *cmd)
{
    int fd = open(history_path(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    write_all(fd, cmd, strlen(cmd));
    write_all(fd, "\n", 1);
    close(fd);
}

/* add to the in-memory ring; caller holds the semaphore */
static void hist_push(const char *cmd)
{
    int idx;

    if (g_hist->count < HIST_MAX) {
        idx = (g_hist->head + g_hist->count) % HIST_MAX;
        g_hist->count++;
    } else {
        idx = g_hist->head;
        g_hist->head = (g_hist->head + 1) % HIST_MAX;
    }
    snprintf(g_hist->entries[idx], HIST_ENTRY, "%s", cmd);
}

void history_add(const char *cmd)
{
    if (!g_hist || !cmd || !*cmd)
        return;

    sem_wait(&g_hist->sem);
    hist_push(cmd);
    hist_file_append(cmd);
    sem_post(&g_hist->sem);
}

/* load the history file at start-up, keeping only the last HIST_MAX lines */
void history_load_file(void)
{
    FILE *f;
    char line[HIST_ENTRY];
    long nread = 0;
    int rewrite = 0;

    if (!g_hist)
        return;

    f = fopen(history_path(), "r");
    if (!f)
        return;

    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = '\0';
        if (!L)
            continue;
        sem_wait(&g_hist->sem);
        hist_push(line);
        sem_post(&g_hist->sem);
        nread++;
    }
    fclose(f);

    if (nread > HIST_MAX)
        rewrite = 1;                 /* the file grew past the cap: trim it */

    if (rewrite) {
        int i, total;
        int fd = open(history_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            return;
        sem_wait(&g_hist->sem);
        total = g_hist->count;
        for (i = 0; i < total; i++) {
            write_all(fd, g_hist->entries[(g_hist->head + i) % HIST_MAX],
                      strlen(g_hist->entries[(g_hist->head + i) % HIST_MAX]));
            write_all(fd, "\n", 1);
        }
        sem_post(&g_hist->sem);
        close(fd);
    }
}

/* clear both the ring and the file */
void history_clear_all(void)
{
    if (!g_hist)
        return;
    sem_wait(&g_hist->sem);
    g_hist->count = 0;
    g_hist->head = 0;
    sem_post(&g_hist->sem);
    if (truncate(history_path(), 0) < 0) {
        int fd = open(history_path(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) close(fd);
    }
}

int history_count(void)
{
    int c;
    if (!g_hist)
        return 0;
    sem_wait(&g_hist->sem);
    c = g_hist->count;
    sem_post(&g_hist->sem);
    return c;
}

int history_get(int i, char *buf, size_t cap)
{
    int ok = 0;
    if (!g_hist || cap == 0)
        return 0;
    sem_wait(&g_hist->sem);
    if (i >= 0 && i < g_hist->count) {
        snprintf(buf, cap, "%s", g_hist->entries[(g_hist->head + i) % HIST_MAX]);
        ok = 1;
    }
    sem_post(&g_hist->sem);
    return ok;
}
