/*
 * terminator.c -- "Terminator": an X11 terminal emulator + shell written in C.
 *
 * Build:  make            (or)  gcc -O2 -std=gnu11 -pthread -o terminator terminator.c -lX11
 * Run:    ./terminator                 # thread mode  (one process, one thread per tab)
 *         ./terminator useProcess=True # process mode (one process per tab)
 *
 * See DESIGN.md for the architecture discussion.
 *
 * A single translation unit keeps the cross-module interfaces honest.  The file
 * is organised in sections:  types -> history -> parser -> executor ->
 * builtins -> worker -> config -> GUI -> main.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <glob.h>
#include <dirent.h>
#include <time.h>
#include <limits.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>

/* ------------------------------------------------------------------ */
/* tunables                                                           */
/* ------------------------------------------------------------------ */
#define APP_NAME        "terminator"
#define MAX_TABS        16
#define MAX_CMD         4096
#define MAX_EXPANDED    16384
#define ARENA_SZ        131072
#define MAX_ARGS        256
#define MAX_STAGES      64
#define MAX_LINE_LEN    4096
#define MAX_SCROLLBACK  4000
#define HIST_MAX        10000       /* commands kept, in memory and on disk */
#define HIST_SHOW       1000        /* how many `history` prints */
#define HIST_SCAN       2000        /* how many entries Ctrl+R scans */
#define HIST_ENTRY      2048
#define LINE_HEIGHT     18
#define TAB_HEIGHT      30
#define TAB_WIDTH       110
#define WIN_W           860
#define WIN_H           540
#define SEP_LEN         52          /* length of the multiWatch dash rule */
#define MW_INTERVAL     2           /* multiWatch / watch re-run period (s) */
#define MAX_JOBS        32
#define JOB_CMD_LEN     512

/* job states */
#define JOB_RUNNING     0
#define JOB_DONE        1
#define JOB_STOPPED     2

#define MODE_THREAD     0
#define MODE_PROCESS    1

#define CTL_EXIT        1
#define CTL_RELOAD      2
#define CTL_BIND        3

/* Worker-private state is thread-local so that, in thread mode, each tab's
 * worker thread has its own copy.  In process mode there is a single worker
 * thread per process, so this is equally correct. */
#define TSTATIC static __thread

/* ------------------------------------------------------------------ */
/* types                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    sem_t sem;
    int   count;                 /* number of entries */
    int   head;                  /* index of oldest entry (ring) */
    char  entries[HIST_MAX][HIST_ENTRY];
} HistStore;

typedef struct {
    char *text;
    int   quoted;                /* had quoting/escaping -> no globbing */
    int   has_glob;              /* contains an unquoted * ? [ */
    int   is_op;                 /* operator token */
} Token;

typedef struct {
    char *args[MAX_ARGS];
    int   has_glob[MAX_ARGS];
    int   argc;
    char *input;                 /* '<'          */
    char *output;                /* '>' or '>>'  */
    char *error;                 /* '2>' or '2>>'*/
    int   append_out;
    int   append_err;
    int   background;
} Stage;

typedef struct {
    int   id;
    pid_t pgid;
    pid_t pids[MAX_STAGES];
    int   npids;
    int   state;                 /* 0 = running, 1 = done */
    int   bg;
    char  cmd[JOB_CMD_LEN];
} Job;

typedef struct {
    int        in_fd, out_fd, ctl_fd;
    HistStore *hist;
    void      *sh;
    int        mode;
} WorkerCtx;

/* GUI-side tab state. */
typedef struct {
    /* scrollback of finished output lines */
    char **lines;
    int    nlines, cap_lines;
    /* partially received output line */
    char   pending[MAX_LINE_LEN];
    int    pending_len;
    int    scroll;               /* 0 == pinned to bottom */
    /* current input line(s) being edited */
    char   input[MAX_CMD];
    int    len, cursor;
    int    hist_pos;             /* -1 when not browsing history */
    char   hist_draft[MAX_CMD];
    /* prompt / input state (driven by the worker's prompt markers) */
    int    in_prompt;
    int    prompt_len;
    char   prompt[256];
    int    awaiting;
    /* pipes */
    int    in_r, in_w, out_r, out_w, ctl_r, ctl_w;
    /* worker handle */
    pthread_t tid;
    pid_t  pid;
    int    alive;
    int    dirty;
    /* mouse selection (display coordinates) */
    int    sel_active;
    int    sel_l0, sel_c0, sel_l1, sel_c1;
    /* Ctrl+R search prompt */
    int    searching;
    char   search_buf[512];
    int    search_len;
    /* Tab-completion chooser */
    int    choosing;
    int    nchoices;
    char   choices[64][PATH_MAX];
    int    wstart, wend;
} Tab;

typedef enum {
    ACT_NONE = 0,
    ACT_LEFT, ACT_RIGHT, ACT_BOL, ACT_EOL,
    ACT_HIST_PREV, ACT_HIST_NEXT,
    ACT_KILL_START, ACT_KILL_END, ACT_KILL_WORD,
    ACT_CLEAR, ACT_EXIT, ACT_INTERRUPT, ACT_COPY, ACT_PASTE,
    ACT_NEW_TAB, ACT_CLOSE_TAB, ACT_NEXT_TAB, ACT_PREV_TAB,
    ACT_PAGE_UP, ACT_PAGE_DOWN,
    ACT_SUSPEND, ACT_SEARCH, ACT_COMPLETE
} Action;

typedef struct {
    unsigned int mods;
    KeySym       ks;
    Action       act;
} Binding;

/* ------------------------------------------------------------------ */
/* globals                                                            */
/* ------------------------------------------------------------------ */

static HistStore   g_hist_local;      /* thread mode backing store        */
static HistStore  *g_hist = NULL;     /* shared by every worker           */
static int         g_mode = MODE_THREAD;

/* worker-private */
TSTATIC int   g_out_fd = -1;
TSTATIC int   g_in_fd  = -1;
TSTATIC int   g_ctl_fd = -1;
TSTATIC int   g_last_status = 0;
TSTATIC int   g_exit_requested = 0;
TSTATIC char  g_arena[ARENA_SZ];
TSTATIC size_t g_arena_pos = 0;
TSTATIC char  g_cwd[PATH_MAX];      /* this tab's logical working directory */
static pthread_mutex_t g_cwd_mtx = PTHREAD_MUTEX_INITIALIZER;
TSTATIC Job   g_jobs[MAX_JOBS];
TSTATIC int   g_job_count = 0;
TSTATIC int   g_next_job_id = 1;
TSTATIC char  g_cur_cmd[JOB_CMD_LEN];  /* command line of the job being run */

/* ---- shell variables and positional parameters (script interpreter) ---- */
#define MAX_VARS  256
#define MAX_POS   32

typedef struct {
    char name[64];
    char val[512];
} ShellVar;

TSTATIC ShellVar g_vars[MAX_VARS];
TSTATIC int      g_nvars = 0;
TSTATIC char    *g_pos[MAX_POS];        /* g_pos[0] is $1 */
TSTATIC int      g_npos = 0;
TSTATIC char     g_script_name[PATH_MAX] = "terminator";
TSTATIC int      g_script_depth = 0;    /* >0 while interpreting a script */
TSTATIC int      g_script_exit = 0;     /* 'exit' asked the script to stop */

/* GUI-private */
static Display     *g_dpy = NULL;
static int          g_scr;
static Window       g_win;
static GC           g_gc;
static XFontStruct *g_font = NULL;
static int          g_cw = 8;          /* character cell width  */
static int          g_ch = LINE_HEIGHT;/* character cell height */
static Atom         g_wm_delete;
static Atom         g_clipboard, g_utf8, g_targets;
static Cursor       g_cursor;
static int          g_running = 1;
static int          g_win_w = WIN_W, g_win_h = WIN_H;

static Tab  g_tabs[MAX_TABS];
static int  g_tab_count = 0;
static int  g_active = 0;

static Binding g_user_binds[64];
static int     g_n_user_binds = 0;
static int     g_loading_binds = 0;   /* config-file load: duplicates are conflicts */
static Binding g_def_binds[64];
static int     g_n_def_binds = 0;

static char  g_sel_text[65536];
static int   g_sel_len = 0;

/* ------------------------------------------------------------------ */
/* prototypes                                                         */
/* ------------------------------------------------------------------ */

static void  write_all(int fd, const void *buf, size_t n);
static void  emit(const char *fmt, ...);
static void  emit_raw(const char *s, size_t n);
static char *xstrdup(const char *s);

static void history_init(int mode);
static void history_add(const char *cmd);
static int  history_count(void);
static int  history_get(int i, char *buf, size_t cap);
static void history_load_file(void);
static void history_clear_all(void);

static int  lex(const char *src, Token *toks, int max, char *err, size_t errcap);
static int  lex_core(const char *src, Token *toks, int max, char *err, size_t errcap, int reset);
static int  command_incomplete(const char *s);
static int  parse_stages(Token *toks, int nt, Stage *st, int *nst);
static int  cmd_substitute(const char *cmd, char *out, size_t cap);
static int  is_list_op(const Token *t);
static int  is_redir_op(const Token *t);

static void execute_line(const char *line);
static void execute_tokens(Token *toks, int nt);
static void run_one(Token *toks, int nt);
static int  run_pipeline(Stage *st, int n);
static void child_exec(Stage *st, int in_fd, int out_fd, int term_out, pid_t pgid,
                       const char *e_notfound, const char *e_noperm, const char *e_cannot);
static int  open_redirections(Stage *st);
static void expand_globs(Stage *st);
static int  glob_in_cwd(const char *pat, glob_t *g);
static int  wait_foreground(pid_t *pids, int n);
static void reap_background_jobs(void);
static int  run_capture(const char *cmd, char *out, size_t cap);
static void run_command_text(const char *cmd);
static int  try_assignment(const char *line);
static void run_script_file(const char *path, int argc, char **argv, int sourced);
static void resolve_path(const char *p, char *out, size_t cap);
static int  looks_like_script(const char *path);
static int  is_interp_name(const char *s);
static int  run_test_builtin(char **args, int argc);
static int  job_add(pid_t *pids, int n, pid_t pgid, const char *cmd, int state, int bg);
static int  job_pick(const char *spec, int *idx_out);
static const char *job_state_str(int st);

static int  is_builtin(const char *name);
static int  run_builtin_parent(char **args, int argc);
static void do_multiwatch(const char *arg);
static void do_watch(const char *arg);

static void worker_loop(WorkerCtx *ctx);
static void *worker_thread(void *p);
static char *worker_read_command(void);
static int  in_getc(void);

static void bindings_init(void);
static int  bindings_load(const char *path, int verbose);
static int  bindings_add_user(const char *keyspec, const char *action, char *err, size_t cap);
static Action bindings_lookup(unsigned int mods, KeySym ks);
static int  bindings_save(const char *path);
static const char *action_name(Action a);
static Action action_from_name(const char *n);
static int  parse_keyspec(const char *spec, unsigned int *mods, KeySym *ks);
static const char *config_path(void);

static void gui_run(void);
static int  gui_new_tab(void);
static void gui_close_tab(int idx);
static void gui_draw(void);
static void gui_handle_key(XKeyEvent *ke);
static void gui_append_output(Tab *t, const char *data, int n, int idx);
static void tab_append_line(Tab *t, const char *s);
static void gui_suspend(Tab *t, int idx);
static void gui_complete(Tab *t, int idx);
static int  lcs_len(const char *a, const char *b);
static void history_search(Tab *t, const char *term);

/* ------------------------------------------------------------------ */
/* small utilities                                                    */
/* ------------------------------------------------------------------ */

static void write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        p += k;
        n -= (size_t)k;
    }
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/*
 * out = a + b (b may be NULL).  Returns 0, leaving nothing behind, when the
 * result would not fit.  Doing the copy by hand keeps the compiler from
 * having to guess at snprintf() truncation, and the length check makes
 * over-long paths a reported error instead of a silent truncation.
 */
static int str_cat2(char *out, size_t cap, const char *a, const char *b)
{
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;

    if (la + lb + 1 > cap)
        return 0;
    if (la)
        memmove(out, a, la);
    if (lb)
        memmove(out + la, b, lb);
    out[la + lb] = '\0';
    return 1;
}

static void emit_raw(const char *s, size_t n)
{
    if (g_out_fd >= 0 && n)
        write_all(g_out_fd, s, n);
}

static void emit(const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    if (n <= 0)
        return;

    if ((size_t)n > sizeof buf - 1)
        n = (int)sizeof buf - 1;

    emit_raw(buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* shared history                                                     */
/* ------------------------------------------------------------------ */

static void history_init(int mode)
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

static void history_add(const char *cmd)
{
    if (!g_hist || !cmd || !*cmd)
        return;

    sem_wait(&g_hist->sem);
    hist_push(cmd);
    hist_file_append(cmd);
    sem_post(&g_hist->sem);
}

/* load the history file at start-up, keeping only the last HIST_MAX lines */
static void history_load_file(void)
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
static void history_clear_all(void)
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

static int history_count(void)
{
    int c;
    if (!g_hist)
        return 0;
    sem_wait(&g_hist->sem);
    c = g_hist->count;
    sem_post(&g_hist->sem);
    return c;
}

static int history_get(int i, char *buf, size_t cap)
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

/* ------------------------------------------------------------------ */
/* lexer / parser                                                     */
/* ------------------------------------------------------------------ */

static int apush(char c)
{
    if (g_arena_pos + 1 >= ARENA_SZ)
        return 0;
    g_arena[g_arena_pos++] = c;
    return 1;
}

static int astr(const char *s)
{
    while (s && *s) {
        if (!apush(*s++))
            return 0;
    }
    return 1;
}

static int match_paren(const char *s, int open)
{
    int depth = 0, i = open;
    char q = 0;

    for (; s[i]; i++) {
        char c = s[i];
        if (q == '\'') {
            if (c == '\'') q = 0;
            continue;
        }
        if (q == '"') {
            if (c == '\\' && s[i + 1]) { i++; continue; }
            if (c == '"') q = 0;
            continue;
        }
        if (c == '\'') { q = '\''; continue; }
        if (c == '"')  { q = '"';  continue; }
        if (c == '\\' && s[i + 1]) { i++; continue; }
        if (c == '(') depth++;
        else if (c == ')') { depth--; if (depth == 0) return i; }
    }
    return -1;
}

static int match_backtick(const char *s, int open)
{
    int i;
    for (i = open + 1; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1]) { i++; continue; }
        if (s[i] == '`') return i;
    }
    return -1;
}

/* run a command substitution; result has trailing newlines stripped */
static int cmd_substitute(const char *cmd, char *out, size_t cap)
{
    return run_capture(cmd, out, cap);
}

static int push_subst(const char *inner)
{
    char buf[MAX_EXPANDED];
    size_t len;

    if (!cmd_substitute(inner, buf, sizeof buf))
        return 0;

    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    /* split on whitespace like bash word-splitting would inside quotes it
     * does not, but the caller decides; here we keep it verbatim */
    return astr(buf);
}

/* ---- shell variable table ---- */

static void var_set(const char *name, const char *val)
{
    int i;

    if (!name || !*name || strlen(name) >= sizeof g_vars[0].name)
        return;
    for (i = 0; i < g_nvars; i++) {
        if (!strcmp(g_vars[i].name, name)) {
            snprintf(g_vars[i].val, sizeof g_vars[i].val, "%s", val ? val : "");
            return;
        }
    }
    if (g_nvars < MAX_VARS) {
        snprintf(g_vars[g_nvars].name, sizeof g_vars[0].name, "%s", name);
        snprintf(g_vars[g_nvars].val, sizeof g_vars[0].val, "%s", val ? val : "");
        g_nvars++;
    }
}

static const char *var_get(const char *name)
{
    for (int i = 0; i < g_nvars; i++)
        if (!strcmp(g_vars[i].name, name))
            return g_vars[i].val;
    return NULL;
}

/* look up a name: shell variables win over the environment */
static const char *lookup_name(const char *name)
{
    const char *v = var_get(name);
    if (v)
        return v;
    v = getenv(name);
    return v ? v : "";
}

static const char *positional(int idx)      /* idx 1..N */
{
    if (idx <= 0)
        return g_script_name;
    if (idx <= g_npos && g_pos[idx - 1])
        return g_pos[idx - 1];
    return "";
}

/* expand a $... reference starting at s[i]=='$'; returns new index */
static int expand_dollar(const char *s, int i)
{
    i++;                               /* past '$' */

    if (s[i] == '?') {
        char n[16];
        snprintf(n, sizeof n, "%d", g_last_status);
        astr(n);
        return i + 1;
    }
    if (s[i] == '$') {
        char n[16];
        snprintf(n, sizeof n, "%d", (int)getpid());
        astr(n);
        return i + 1;
    }
    if (s[i] == '#') {                 /* $# = number of positional params */
        char n[16];
        snprintf(n, sizeof n, "%d", g_npos);
        astr(n);
        return i + 1;
    }
    if (isdigit((unsigned char)s[i])) { /* $1 .. $N, $0 */
        int idx = 0;
        while (isdigit((unsigned char)s[i]))
            idx = idx * 10 + (s[i++] - '0');
        astr(positional(idx));
        return i;
    }
    if (s[i] == '(') {                 /* $(...) */
        int close = match_paren(s, i);
        char inner[MAX_EXPANDED];
        size_t L;
        if (close < 0)
            return -1;
        L = (size_t)(close - i - 1);
        if (L >= sizeof inner)
            L = sizeof inner - 1;
        memcpy(inner, s + i + 1, L);
        inner[L] = '\0';
        if (!push_subst(inner))
            return -1;
        return close + 1;
    }
    if (s[i] == '{') {                 /* ${name} */
        int j = i + 1;
        char name[256];
        int k = 0;
        while (s[j] && s[j] != '}' && k < (int)sizeof name - 1)
            name[k++] = s[j++];
        name[k] = '\0';
        if (s[j] == '}') {
            int alldig = (k > 0);
            for (int d = 0; d < k; d++)
                if (!isdigit((unsigned char)name[d])) { alldig = 0; break; }
            if (alldig)
                astr(positional(atoi(name)));
            else
                astr(lookup_name(name));
            return j + 1;
        }
        /* unterminated -> literal */
        apush('$');
        apush('{');
        astr(name);
        return j;
    }
    if (isalpha((unsigned char)s[i]) || s[i] == '_') {
        char name[256];
        int k = 0;
        while ((isalnum((unsigned char)s[i]) || s[i] == '_') &&
               k < (int)sizeof name - 1)
            name[k++] = s[i++];
        name[k] = '\0';
        astr(lookup_name(name));
        return i;
    }
    apush('$');
    return i;
}

static int word_end(char c)
{
    return c == '\0' || isspace((unsigned char)c) ||
           c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

static int lex_core(const char *src, Token *toks, int max, char *err,
                    size_t errcap, int reset)
{
    int nt = 0;
    int i = 0;

    if (reset)
        g_arena_pos = 0;

    while (src[i]) {
        /* skip separators */
        while (src[i] &&
               (isspace((unsigned char)src[i]) ||
                (src[i] == '\\' && src[i + 1] == '\n')))
            i++;
        if (!src[i])
            break;

        if (nt >= max) {
            if (err) snprintf(err, errcap, "too many tokens");
            return -1;
        }

        /* operators */
        {
            const char *op = NULL;
            int olen = 0;

            if (src[i] == '|' && src[i + 1] == '|') { op = "||"; olen = 2; }
            else if (src[i] == '&' && src[i + 1] == '&') { op = "&&"; olen = 2; }
            else if (src[i] == '|') { op = "|"; olen = 1; }
            else if (src[i] == ';') { op = ";"; olen = 1; }
            else if (src[i] == '&') { op = "&"; olen = 1; }
            else if (src[i] == '<' && src[i + 1] == '<') { op = "<<"; olen = 2; }
            else if (src[i] == '<') { op = "<"; olen = 1; }
            else if (src[i] == '>' && src[i + 1] == '>') { op = ">>"; olen = 2; }
            else if (src[i] == '>') { op = ">"; olen = 1; }
            else if (src[i] == '2' && src[i + 1] == '>' && src[i + 2] == '>') { op = "2>>"; olen = 3; }
            else if (src[i] == '2' && src[i + 1] == '>') { op = "2>"; olen = 2; }

            if (op) {
                size_t st = g_arena_pos;
                astr(op);
                apush('\0');
                toks[nt].text = &g_arena[st];
                toks[nt].quoted = 0;
                toks[nt].has_glob = 0;
                toks[nt].is_op = 1;
                nt++;
                i += olen;
                continue;
            }
        }

        /* a word */
        {
            size_t st = g_arena_pos;
            int quoted = 0, glob = 0;
            char q = 0;

            while (src[i] && !(q == 0 && word_end(src[i]))) {
                char c = src[i];

                if (q == '\'') {
                    if (c == '\'') { q = 0; i++; }
                    else { apush(c); i++; }
                    continue;
                }
                if (q == '"') {
                    if (c == '"') { q = 0; i++; continue; }
                    if (c == '\\') {
                        char nx = src[i + 1];
                        if (nx == '$' || nx == '`' || nx == '"' || nx == '\\') {
                            apush(nx); i += 2;
                        } else if (nx == '\n') {
                            i += 2;            /* line continuation */
                        } else {
                            apush('\\'); i++;
                        }
                        quoted = 1;
                        continue;
                    }
                    if (c == '$') {
                        int r = expand_dollar(src, i);
                        if (r < 0) goto fail;
                        i = r; quoted = 1; continue;
                    }
                    if (c == '`') {
                        int close = match_backtick(src, i);
                        char inner[MAX_EXPANDED];
                        size_t L;
                        if (close < 0) goto fail;
                        L = (size_t)(close - i - 1);
                        if (L >= sizeof inner) L = sizeof inner - 1;
                        memcpy(inner, src + i + 1, L);
                        inner[L] = '\0';
                        if (!push_subst(inner)) goto fail;
                        i = close + 1; quoted = 1; continue;
                    }
                    apush(c); i++; continue;
                }

                /* unquoted */
                if (c == '\'') { q = '\''; quoted = 1; i++; continue; }
                if (c == '"')  { q = '"';  quoted = 1; i++; continue; }
                if (c == '\\') {
                    char nx = src[i + 1];
                    if (nx == '\0') { i++; continue; }
                    if (nx == '\n') { i += 2; continue; }  /* continuation */
                    apush(nx); i += 2; quoted = 1; continue;
                }
                if (c == '$') {
                    int r = expand_dollar(src, i);
                    if (r < 0) goto fail;
                    i = r; quoted = 1; continue;
                }
                if (c == '`') {
                    int close = match_backtick(src, i);
                    char inner[MAX_EXPANDED];
                    size_t L;
                    if (close < 0) goto fail;
                    L = (size_t)(close - i - 1);
                    if (L >= sizeof inner) L = sizeof inner - 1;
                    memcpy(inner, src + i + 1, L);
                    inner[L] = '\0';
                    if (!push_subst(inner)) goto fail;
                    i = close + 1; quoted = 1; continue;
                }
                if (c == '*' || c == '?' || c == '[')
                    glob = 1;
                apush(c); i++;
            }

            if (q != 0) {
                if (err) snprintf(err, errcap, "unmatched quote");
                return -1;
            }

            apush('\0');
            toks[nt].text = &g_arena[st];
            toks[nt].quoted = quoted;
            toks[nt].has_glob = glob;
            toks[nt].is_op = 0;
            nt++;
        }
        continue;
fail:
        if (err) snprintf(err, errcap, "syntax error");
        return -1;
    }

    return nt;
}

static int lex(const char *src, Token *toks, int max, char *err, size_t errcap)
{
    return lex_core(src, toks, max, err, errcap, 1);
}

static int is_list_op(const Token *t)
{
    return t->is_op &&
           (!strcmp(t->text, "&&") || !strcmp(t->text, "||") || !strcmp(t->text, ";"));
}

static int is_redir_op(const Token *t)
{
    return t->is_op &&
           (!strcmp(t->text, "<") || !strcmp(t->text, ">") ||
            !strcmp(t->text, ">>") || !strcmp(t->text, "2>") ||
            !strcmp(t->text, "2>>"));
}

static int command_incomplete(const char *s)
{
    int i = 0;
    char q = 0;
    int paren = 0, btick = 0;
    int last_nonblank = -1;

    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (!isspace((unsigned char)c))
            last_nonblank = i;

        if (q == '\'') {
            if (c == '\'') q = 0;
            continue;
        }
        if (q == '"') {
            if (c == '\\' && s[i + 1]) { i++; continue; }
            if (c == '"') q = 0;
            continue;
        }
        if (c == '\\') {
            if (s[i + 1] == '\0')
                return 1;                 /* trailing backslash */
            i++;
            continue;
        }
        if (c == '\'') { q = '\''; continue; }
        if (c == '"')  { q = '"';  continue; }
        if (c == '$' && s[i + 1] == '(') { paren++; i++; continue; }
        if (c == '(' && paren > 0) { paren++; continue; }
        if (c == ')' && paren > 0) { paren--; continue; }
        if (c == '`') { btick = !btick; continue; }
    }

    if (q != 0 || paren > 0 || btick)
        return 1;

    if (last_nonblank >= 0) {
        char c = s[last_nonblank];
        if (c == '|')
            return 1;
        if (c == '&' && last_nonblank > 0 && s[last_nonblank - 1] == '&')
            return 1;
    }
    return 0;
}

static int parse_stages(Token *toks, int nt, Stage *st, int *nst)
{
    int s = 0, i;

    memset(st, 0, sizeof(Stage) * MAX_STAGES);
    *nst = 1;

    for (i = 0; i < nt; i++) {
        Token *t = &toks[i];

        if (is_list_op(t))
            return 0;                  /* caller must split on these first */

        if (t->is_op && !strcmp(t->text, "|")) {
            if (st[s].argc == 0)
                return 0;
            if (s + 1 >= MAX_STAGES)
                return 0;
            s++;
            (*nst)++;
            continue;
        }

        if (t->is_op && !strcmp(t->text, "&")) {
            st[s].background = 1;
            continue;
        }

        if (is_redir_op(t)) {
            if (++i >= nt || toks[i].is_op)
                return 0;
            if (!strcmp(t->text, "<"))
                st[s].input = toks[i].text;
            else if (!strcmp(t->text, ">")) {
                st[s].output = toks[i].text; st[s].append_out = 0;
            } else if (!strcmp(t->text, ">>")) {
                st[s].output = toks[i].text; st[s].append_out = 1;
            } else if (!strcmp(t->text, "2>")) {
                st[s].error = toks[i].text; st[s].append_err = 0;
            } else {
                st[s].error = toks[i].text; st[s].append_err = 1;
            }
            continue;
        }

        if (st[s].argc >= MAX_ARGS - 1)
            return 0;

        st[s].args[st[s].argc] = t->text;
        st[s].has_glob[st[s].argc] = t->has_glob;
        st[s].argc++;
    }

    for (i = 0; i < *nst; i++) {
        if (st[i].argc == 0)
            return 0;
        st[i].args[st[i].argc] = NULL;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* executor                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile pid_t fg_pgid;
    volatile pid_t mw_pgid;
    volatile int   mw_active;
    volatile int   mw_cancel;
} Shared;

static Shared *g_sh_self = NULL;      /* this worker's shared control block */

static void free_stage_args(Stage *st, int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < st[i].argc; j++)
            free(st[i].args[j]);
}

static void expand_globs(Stage *st)
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

static int open_redirections(Stage *st)
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

static void child_exec(Stage *st, int in_fd, int out_fd, int term_out, pid_t pgid,
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
static int wait_foreground(pid_t *pids, int n)
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

static const char *job_state_str(int st)
{
    return st == JOB_STOPPED ? "Stopped" : st == JOB_DONE ? "Done" : "Running";
}

static int job_add(pid_t *pids, int n, pid_t pgid, const char *cmd, int state, int bg)
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
static int job_pick(const char *spec, int *idx_out)
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

static void reap_background_jobs(void)
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
static int run_pipeline(Stage *st, int n)
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
static int run_capture(const char *cmd, char *out, size_t cap)
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
/* builtins                                                           */
/* ------------------------------------------------------------------ */

/*
 * Change this tab's working directory.  In thread mode every tab shares one
 * process, so we keep a per-tab logical cwd instead of relying on chdir();
 * child processes chdir() into it just before exec.
 */
static int change_dir(const char *arg)
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


/* ------------------------------------------------------------------ */
/* bash-subset script interpreter (Task 11)                           */
/* ------------------------------------------------------------------ */

/* growable string buffer */
typedef struct { char *p; size_t n, cap; } SB;

static void sb_put(SB *b, char c)
{
    if (b->n + 2 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    b->p[b->n++] = c;
    b->p[b->n] = '\0';
}

/* ---- resolve a path relative to this tab's cwd ---- */
static void resolve_path(const char *p, char *out, size_t cap)
{
    if (!p || !*p) { snprintf(out, cap, "%s", g_cwd[0] ? g_cwd : "."); return; }
    if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        const char *h = getenv("HOME");
        snprintf(out, cap, "%s%s", h ? h : "", p + 1);
    } else if (p[0] == '/') {
        snprintf(out, cap, "%s", p);
    } else {
        while (p[0] == '.' && p[1] == '/')       /* drop a leading "./" */
            p += 2;
        snprintf(out, cap, "%s/%s", g_cwd[0] ? g_cwd : ".", p);
    }
}

/* does this path hold a script (text) rather than an executable binary? */
static int looks_like_script(const char *path)
{
    int fd;
    char head[4];
    ssize_t n;
    struct stat sb;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (fstat(fd, &sb) < 0 || !S_ISREG(sb.st_mode)) { close(fd); return 0; }
    n = read(fd, head, sizeof head);
    close(fd);
    if (n >= 4 && !memcmp(head, "\x7f" "ELF", 4))
        return 0;                        /* a real binary: exec it normally */
    return 1;                            /* text / shebang script */
}

static int is_interp_name(const char *s)
{
    const char *base = strrchr(s, '/');
    base = base ? base + 1 : s;
    return !strcmp(base, "bash") || !strcmp(base, "sh") ||
           !strcmp(base, "dash") || !strcmp(base, "terminator");
}

/* ---- assignment: NAME=value ---- */
static int try_assignment(const char *line)
{
    Token toks[4];
    char err[64], name[64];
    char *eq;
    int n, nt;

    if (!line || !strchr(line, '='))
        return 0;

    nt = lex(line, toks, 4, err, sizeof err);
    if (nt != 1 || toks[0].is_op)
        return 0;

    eq = strchr(toks[0].text, '=');
    if (!eq || eq == toks[0].text)
        return 0;

    n = (int)(eq - toks[0].text);
    if (n >= (int)sizeof name)
        return 0;
    if (!isalpha((unsigned char)toks[0].text[0]) && toks[0].text[0] != '_')
        return 0;
    for (int i = 0; i < n; i++)
        if (!isalnum((unsigned char)toks[0].text[i]) && toks[0].text[i] != '_')
            return 0;

    memcpy(name, toks[0].text, (size_t)n);
    name[n] = '\0';
    var_set(name, eq + 1);
    g_last_status = 0;
    return 1;
}

/* run one command line: assignment, otherwise lex + execute */
static void run_command_text(const char *cmd)
{
    Token toks[MAX_ARGS * 2];
    char err[128];
    int nt;

    if (!cmd || !*cmd)
        return;
    if (try_assignment(cmd))
        return;

    nt = lex(cmd, toks, MAX_ARGS * 2, err, sizeof err);
    if (nt < 0) {
        emit("%s: %s\n", APP_NAME, err);
        g_last_status = 2;
        return;
    }
    if (nt == 0)
        return;
    execute_tokens(toks, nt);
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

static int run_test_builtin(char **args, int argc)
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

/* ---- script AST ---- */

#define ND_LIST   0
#define ND_SIMPLE 1
#define ND_IF     2

typedef struct Node Node;
typedef struct { Node *cond; Node *body; } Branch;

struct Node {
    int      kind;
    Node   **kids;      /* ND_LIST  */
    int      nkids;
    char    *cmd;       /* ND_SIMPLE */
    Branch  *br;        /* ND_IF    */
    int      nbr;
    Node    *else_body;
};

typedef struct { char *w; int quoted; int sep; } SItem;
typedef struct { SItem *it; int n; int i; char *err; size_t errcap; } PCtx;

static Node *node_new(int kind)
{
    Node *n = calloc(1, sizeof *n);
    if (n) n->kind = kind;
    return n;
}

static void node_kid(Node *list, Node *kid)
{
    Node **k = realloc(list->kids, sizeof(Node *) * (size_t)(list->nkids + 1));
    if (!k) return;
    list->kids = k;
    list->kids[list->nkids++] = kid;
}

static void node_branch(Node *n, Node *cond, Node *body)
{
    Branch *b = realloc(n->br, sizeof(Branch) * (size_t)(n->nbr + 1));
    if (!b) return;
    n->br = b;
    n->br[n->nbr].cond = cond;
    n->br[n->nbr].body = body;
    n->nbr++;
}

static void node_free(Node *n)
{
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
    free(n->cmd);
    for (int i = 0; i < n->nbr; i++) { node_free(n->br[i].cond); node_free(n->br[i].body); }
    free(n->br);
    node_free(n->else_body);
    free(n);
}

/* ---- scanner: split the script into words and separators ---- */

static int svec_push(SItem **v, int *n, int *cap, char *w, int quoted, int sep)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        SItem *nv = realloc(*v, sizeof(SItem) * (size_t)nc);
        if (!nv) return 0;
        *v = nv;
        *cap = nc;
    }
    (*v)[*n].w = w;
    (*v)[*n].quoted = quoted;
    (*v)[*n].sep = sep;
    (*n)++;
    return 1;
}

static int script_scan(const char *src, SItem **out, int *nout, char *err, size_t errcap)
{
    SItem *v = NULL;
    int n = 0, cap = 0, i = 0;

    while (src[i]) {
        char c = src[i];

        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '\\' && src[i + 1] == '\n') { i += 2; continue; }
        if (c == '\n' || c == ';') { svec_push(&v, &n, &cap, NULL, 0, 1); i++; continue; }
        if (c == '#') {                       /* comment to end of line */
            while (src[i] && src[i] != '\n') i++;
            continue;
        }

        {
            SB sb = { NULL, 0, 0 };
            int quoted = 0;
            char q = 0;
            int paren = 0, btick = 0;

            while (src[i]) {
                char ch = src[i];

                if (q == 0 && paren == 0 && !btick &&
                    (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ';'))
                    break;

                if (q == '\'') {
                    if (ch == '\'') q = 0;
                    sb_put(&sb, ch); i++; continue;
                }
                if (q == '"') {
                    if (ch == '\\' && src[i + 1]) {
                        sb_put(&sb, ch); sb_put(&sb, src[i + 1]); i += 2; continue;
                    }
                    if (ch == '"') q = 0;
                    sb_put(&sb, ch); i++; continue;
                }
                if (ch == '\\' && src[i + 1]) {
                    sb_put(&sb, ch); sb_put(&sb, src[i + 1]);
                    quoted = 1; i += 2; continue;
                }
                if (ch == '\'') { q = '\''; quoted = 1; sb_put(&sb, ch); i++; continue; }
                if (ch == '"')  { q = '"';  quoted = 1; sb_put(&sb, ch); i++; continue; }
                if (ch == '$' && src[i + 1] == '(') { paren++; sb_put(&sb, ch); i++; continue; }
                if (ch == '(' && paren) { paren++; sb_put(&sb, ch); i++; continue; }
                if (ch == ')' && paren) { paren--; sb_put(&sb, ch); i++; continue; }
                if (ch == '`') { btick = !btick; sb_put(&sb, ch); i++; continue; }
                sb_put(&sb, ch); i++;
            }

            if (q != 0 || paren != 0 || btick) {
                if (err) snprintf(err, errcap, "unterminated quote or substitution");
                free(sb.p);
                for (int k = 0; k < n; k++) free(v[k].w);
                free(v);
                return -1;
            }
            if (!sb.p)
                sb_put(&sb, '\0');
            svec_push(&v, &n, &cap, sb.p, quoted, 0);
        }
    }

    *out = v;
    *nout = n;
    return 0;
}

/* ---- parser ---- */

static void p_err(PCtx *p, const char *msg)
{
    if (p->err && !p->err[0])
        snprintf(p->err, p->errcap, "%s", msg);
}

static int p_end(PCtx *p) { return p->i >= p->n; }

static void p_skip_seps(PCtx *p)
{
    while (p->i < p->n && p->it[p->i].sep) p->i++;
}

static int p_res(PCtx *p, const char *w)
{
    return p->i < p->n && !p->it[p->i].sep && !p->it[p->i].quoted &&
           p->it[p->i].w && !strcmp(p->it[p->i].w, w);
}

static int p_terminator(PCtx *p)
{
    return p_res(p, "then") || p_res(p, "elif") || p_res(p, "else") || p_res(p, "fi");
}

static Node *parse_stmt_list(PCtx *p);

static Node *parse_simple(PCtx *p)
{
    SB sb = { NULL, 0, 0 };
    Node *n;

    while (p->i < p->n && !p->it[p->i].sep) {
        if (sb.n) sb_put(&sb, ' ');
        for (const char *q = p->it[p->i].w; q && *q; q++)
            sb_put(&sb, *q);
        p->i++;
    }
    if (!sb.p || sb.n == 0) { free(sb.p); return NULL; }
    n = node_new(ND_SIMPLE);
    if (!n) { free(sb.p); return NULL; }
    n->cmd = sb.p;
    return n;
}

static Node *parse_if(PCtx *p)
{
    Node *n = node_new(ND_IF);
    Node *cond, *body;

    if (!n) return NULL;
    p->i++;                                  /* 'if' */

    cond = parse_stmt_list(p);
    if (!cond) { node_free(n); return NULL; }
    if (!p_res(p, "then")) {
        p_err(p, "expected 'then' after 'if'");
        node_free(cond); node_free(n);
        return NULL;
    }
    p->i++;                                  /* 'then' */
    body = parse_stmt_list(p);
    if (!body) { node_free(cond); node_free(n); return NULL; }
    node_branch(n, cond, body);

    for (;;) {
        if (p_res(p, "elif")) {
            p->i++;
            cond = parse_stmt_list(p);
            if (!cond) { node_free(n); return NULL; }
            if (!p_res(p, "then")) {
                p_err(p, "expected 'then' after 'elif'");
                node_free(cond); node_free(n);
                return NULL;
            }
            p->i++;
            body = parse_stmt_list(p);
            if (!body) { node_free(n); return NULL; }
            node_branch(n, cond, body);
            continue;
        }
        if (p_res(p, "else")) {
            p->i++;
            n->else_body = parse_stmt_list(p);
            if (!n->else_body) { node_free(n); return NULL; }
            if (!p_res(p, "fi")) {
                p_err(p, "expected 'fi'");
                node_free(n);
                return NULL;
            }
            p->i++;
            return n;
        }
        if (p_res(p, "fi")) { p->i++; return n; }

        p_err(p, "expected 'fi'");
        node_free(n);
        return NULL;
    }
}

static Node *parse_stmt_list(PCtx *p)
{
    Node *list = node_new(ND_LIST);

    if (!list) return NULL;
    for (;;) {
        Node *st;
        p_skip_seps(p);
        if (p_end(p)) return list;
        if (p_terminator(p)) return list;
        if (p_res(p, "if"))
            st = parse_if(p);
        else
            st = parse_simple(p);
        if (!st) { node_free(list); return NULL; }
        node_kid(list, st);
    }
}

static int script_parse(SItem *it, int n, Node **root, char *err, size_t errcap)
{
    PCtx p;
    Node *r;

    memset(&p, 0, sizeof p);
    p.it = it; p.n = n; p.i = 0;
    p.err = err; p.errcap = errcap;
    if (err && errcap) err[0] = '\0';

    r = parse_stmt_list(&p);
    if (!r) return -1;

    p_skip_seps(&p);
    if (!p_end(&p)) {
        if (err) snprintf(err, errcap, "unexpected '%s'",
                          p.it[p.i].w ? p.it[p.i].w : "token");
        node_free(r);
        return -1;
    }
    *root = r;
    return 0;
}

/* ---- execute the AST ---- */

static void exec_node(Node *n)
{
    if (!n || g_script_exit)
        return;

    switch (n->kind) {
    case ND_LIST:
        for (int i = 0; i < n->nkids; i++) {
            exec_node(n->kids[i]);
            if (g_script_exit) return;
        }
        break;
    case ND_SIMPLE:
        if (n->cmd && *n->cmd)
            run_command_text(n->cmd);
        break;
    case ND_IF:
        for (int i = 0; i < n->nbr; i++) {
            exec_node(n->br[i].cond);
            if (g_script_exit) return;
            if (g_last_status == 0) { exec_node(n->br[i].body); return; }
        }
        exec_node(n->else_body);
        break;
    default:
        break;
    }
}

/*
 * Read, syntax-check and interpret a script.  Nothing runs unless the whole
 * file parses, so a missing 'fi' is reported instead of half-executing it.
 * `sourced` keeps cd/exit effects in the caller's shell.
 */
static void run_script_file(const char *path, int argc, char **argv, int sourced)
{
    FILE *f;
    char *buf;
    long sz;
    SItem *items = NULL;
    int nitems = 0, i;
    Node *root = NULL;
    char err[256] = "";
    char save_cwd[PATH_MAX] = "";
    char *save_pos[MAX_POS];
    int save_npos;
    char save_name[PATH_MAX];

    f = fopen(path, "r");
    if (!f) {
        emit("%s: %s: %s\n", APP_NAME, path, strerror(errno));
        g_last_status = 1;
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) sz = 0;
    else sz = ftell(f);
    if (sz < 0) sz = 0;
    if (sz > (1 << 20)) sz = 1 << 20;
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); g_last_status = 1; return; }
    {
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = '\0';
    }
    fclose(f);

    if (script_scan(buf, &items, &nitems, err, sizeof err) < 0 ||
        script_parse(items, nitems, &root, err, sizeof err) < 0) {
        emit("%s: %s: syntax error: %s\n", APP_NAME, path, err);
        g_last_status = 2;
        free(buf);
        for (i = 0; i < nitems; i++) free(items[i].w);
        free(items);
        return;
    }
    free(buf);
    for (i = 0; i < nitems; i++) free(items[i].w);
    free(items);

    /* positional parameters and $0 for the duration of the script */
    snprintf(save_name, sizeof save_name, "%s", g_script_name);
    save_npos = g_npos;
    for (i = 0; i < MAX_POS; i++) { save_pos[i] = g_pos[i]; g_pos[i] = NULL; }
    g_npos = argc > MAX_POS ? MAX_POS : argc;
    for (i = 0; i < g_npos; i++)
        g_pos[i] = xstrdup(argv[i] ? argv[i] : "");
    snprintf(g_script_name, sizeof g_script_name, "%s", path);

    if (!sourced)
        snprintf(save_cwd, sizeof save_cwd, "%s", g_cwd);

    g_script_depth++;
    g_script_exit = 0;
    exec_node(root);
    g_script_exit = 0;
    g_script_depth--;

    if (!sourced && save_cwd[0])
        snprintf(g_cwd, sizeof g_cwd, "%s", save_cwd);

    for (i = 0; i < MAX_POS; i++) { free(g_pos[i]); g_pos[i] = save_pos[i]; }
    g_npos = save_npos;
    snprintf(g_script_name, sizeof g_script_name, "%s", save_name);

    node_free(root);
}

static int is_builtin(const char *name)
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

static int run_builtin_parent(char **args, int argc)
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

/* ------------------------------------------------------------------ */
/* working-directory aware globbing (thread-safe)                     */
/* ------------------------------------------------------------------ */

static int glob_in_cwd(const char *pat, glob_t *g)
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

static void do_multiwatch(const char *arg)
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

static void do_watch(const char *arg)
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

/* ------------------------------------------------------------------ */
/* command dispatch                                                   */
/* ------------------------------------------------------------------ */

static void run_one(Token *toks, int nt)
{
    Stage st[MAX_STAGES];
    int nst;

    if (!parse_stages(toks, nt, st, &nst)) {
        emit("%s: syntax error near unexpected token\n", APP_NAME);
        g_last_status = 2;
        return;
    }
    run_pipeline(st, nst);
}

static void execute_tokens(Token *toks, int nt)
{
    int i = 0;
    int next_run = 1;

    while (i < nt) {
        int j = i;
        int executed;
        const char *op = NULL;

        while (j < nt && !is_list_op(&toks[j]))
            j++;

        executed = next_run;
        if (executed && j > i)
            run_one(&toks[i], j - i);

        if (j >= nt)
            break;

        op = toks[j].text;
        if (!strcmp(op, "&&"))
            next_run = executed ? (g_last_status == 0) : 0;
        else if (!strcmp(op, "||"))
            next_run = executed ? (g_last_status != 0) : 1;
        else
            next_run = 1;

        i = j + 1;
    }
}

static int word_is(const char *p, const char *w)
{
    size_t L = strlen(w);
    if (strncmp(p, w, L))
        return 0;
    return p[L] == '\0' || isspace((unsigned char)p[L]);
}

static const char *skip_word(const char *p)
{
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static void execute_line(const char *line)
{
    const char *p = line;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p)
        return;

    /* remember the command line for job listings (jobs/fg/bg, Ctrl+Z) */
    snprintf(g_cur_cmd, sizeof g_cur_cmd, "%s", p);

    if (word_is(p, "multiWatch")) {
        history_add(line);
        do_multiwatch(skip_word(p));
        return;
    }
    if (word_is(p, "watch")) {
        history_add(line);
        do_watch(skip_word(p));
        return;
    }

    history_add(line);
    run_command_text(p);
}

/* ------------------------------------------------------------------ */
/* worker                                                             */
/* ------------------------------------------------------------------ */

#define PMARK_START 0x01
#define PMARK_SEP   0x1f
#define PMARK_END   0x02

#define PROMPT_MAIN "user@terminator> "
#define PROMPT_CONT "> "

static int in_getc(void)
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

static char *worker_read_command(void)
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
    return xstrdup(buf);
}

static void worker_loop(WorkerCtx *ctx)
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

static void *worker_thread(void *p)
{
    worker_loop((WorkerCtx *)p);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* key bindings / config                                              */
/* ------------------------------------------------------------------ */

static const char *config_path(void)
{
    static char path[PATH_MAX];
    const char *h = getenv("HOME");
    snprintf(path, sizeof path, "%s/.terminatorrc", h ? h : ".");
    return path;
}

static const char *action_name(Action a)
{
    switch (a) {
    case ACT_LEFT: return "left";
    case ACT_RIGHT: return "right";
    case ACT_BOL: return "bol";
    case ACT_EOL: return "eol";
    case ACT_HIST_PREV: return "hist_prev";
    case ACT_HIST_NEXT: return "hist_next";
    case ACT_KILL_START: return "kill_start";
    case ACT_KILL_END: return "kill_end";
    case ACT_KILL_WORD: return "kill_word";
    case ACT_CLEAR: return "clear";
    case ACT_EXIT: return "exit";
    case ACT_INTERRUPT: return "interrupt";
    case ACT_COPY: return "copy";
    case ACT_PASTE: return "paste";
    case ACT_NEW_TAB: return "new_tab";
    case ACT_CLOSE_TAB: return "close_tab";
    case ACT_NEXT_TAB: return "next_tab";
    case ACT_PREV_TAB: return "prev_tab";
    case ACT_PAGE_UP: return "page_up";
    case ACT_PAGE_DOWN: return "page_down";
    case ACT_SUSPEND: return "suspend";
    case ACT_SEARCH: return "search";
    case ACT_COMPLETE: return "complete";
    default: return "none";
    }
}

static Action action_from_name(const char *n)
{
    static const struct { const char *n; Action a; } tab[] = {
        {"left", ACT_LEFT}, {"right", ACT_RIGHT}, {"bol", ACT_BOL}, {"eol", ACT_EOL},
        {"hist_prev", ACT_HIST_PREV}, {"hist_next", ACT_HIST_NEXT},
        {"kill_start", ACT_KILL_START}, {"kill_end", ACT_KILL_END},
        {"kill_word", ACT_KILL_WORD}, {"clear", ACT_CLEAR}, {"exit", ACT_EXIT},
        {"interrupt", ACT_INTERRUPT}, {"copy", ACT_COPY}, {"paste", ACT_PASTE},
        {"new_tab", ACT_NEW_TAB}, {"close_tab", ACT_CLOSE_TAB},
        {"next_tab", ACT_NEXT_TAB}, {"prev_tab", ACT_PREV_TAB},
        {"page_up", ACT_PAGE_UP}, {"page_down", ACT_PAGE_DOWN},
        {"suspend", ACT_SUSPEND}, {"search", ACT_SEARCH},
        {"complete", ACT_COMPLETE},
        {NULL, ACT_NONE}
    };
    for (int i = 0; tab[i].n; i++)
        if (!strcasecmp(n, tab[i].n))
            return tab[i].a;
    return ACT_NONE;
}

static int ks_equal(KeySym a, KeySym b)
{
    if (a == b)
        return 1;
    if (a >= 0x20 && a < 0x7f && b >= 0x20 && b < 0x7f)
        return tolower((int)a) == tolower((int)b);
    return 0;
}

static int parse_keyspec(const char *spec, unsigned int *mods, KeySym *ks)
{
    char buf[256];
    char *save = NULL, *tok, *last = NULL;

    snprintf(buf, sizeof buf, "%s", spec);
    *mods = 0;

    tok = strtok_r(buf, "+", &save);
    while (tok) {
        char *next = strtok_r(NULL, "+", &save);
        if (!next) { last = tok; break; }
        if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) *mods |= ControlMask;
        else if (!strcasecmp(tok, "shift")) *mods |= ShiftMask;
        else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "meta") || !strcasecmp(tok, "mod1")) *mods |= Mod1Mask;
        else return 0;
        tok = next;
    }
    if (!last || !*last)
        return 0;

    *ks = XStringToKeysym(last);
    if (*ks == NoSymbol && strlen(last) == 1)
        *ks = (KeySym)(unsigned char)last[0];
    return *ks != NoSymbol;
}

static void bindings_add_def(unsigned int mods, const char *spec, Action a)
{
    KeySym ks;
    unsigned int m;

    if (g_n_def_binds >= (int)(sizeof g_def_binds / sizeof g_def_binds[0]))
        return;
    if (!parse_keyspec(spec, &m, &ks))
        return;
    (void)mods;
    g_def_binds[g_n_def_binds].mods = m;
    g_def_binds[g_n_def_binds].ks = ks;
    g_def_binds[g_n_def_binds].act = a;
    g_n_def_binds++;
}

static void bindings_init(void)
{
    g_n_def_binds = 0;
    bindings_add_def(0, "Left", ACT_LEFT);
    bindings_add_def(0, "Right", ACT_RIGHT);
    bindings_add_def(0, "Up", ACT_HIST_PREV);
    bindings_add_def(0, "Down", ACT_HIST_NEXT);
    bindings_add_def(0, "Ctrl+A", ACT_BOL);
    bindings_add_def(0, "Ctrl+E", ACT_EOL);
    bindings_add_def(0, "Ctrl+U", ACT_KILL_START);
    bindings_add_def(0, "Ctrl+K", ACT_KILL_END);
    bindings_add_def(0, "Ctrl+W", ACT_KILL_WORD);
    bindings_add_def(0, "Ctrl+L", ACT_CLEAR);
    bindings_add_def(0, "Ctrl+D", ACT_EXIT);
    bindings_add_def(0, "Ctrl+C", ACT_INTERRUPT);
    bindings_add_def(0, "Ctrl+Shift+C", ACT_COPY);
    bindings_add_def(0, "Ctrl+Shift+V", ACT_PASTE);
    bindings_add_def(0, "Ctrl+Shift+T", ACT_NEW_TAB);
    bindings_add_def(0, "Ctrl+Shift+W", ACT_CLOSE_TAB);
    bindings_add_def(0, "Ctrl+Tab", ACT_NEXT_TAB);
    bindings_add_def(0, "Ctrl+Shift+Tab", ACT_PREV_TAB);
    bindings_add_def(0, "Page_Up", ACT_PAGE_UP);
    bindings_add_def(0, "Page_Down", ACT_PAGE_DOWN);
    bindings_add_def(0, "Ctrl+Z", ACT_SUSPEND);
    bindings_add_def(0, "Ctrl+R", ACT_SEARCH);
    bindings_add_def(0, "Tab", ACT_COMPLETE);
}

static int bindings_add_user(const char *keyspec, const char *action, char *err, size_t cap)
{
    unsigned int mods;
    KeySym ks;
    Action a = action_from_name(action);

    if (a == ACT_NONE) {
        snprintf(err, cap, "unknown action '%s'", action);
        return 0;
    }
    if (!parse_keyspec(keyspec, &mods, &ks)) {
        snprintf(err, cap, "invalid key '%s'", keyspec);
        return 0;
    }
    for (int i = 0; i < g_n_user_binds; i++) {
        if (g_user_binds[i].mods == mods && ks_equal(g_user_binds[i].ks, ks)) {
            if (g_user_binds[i].act == a)
                return 1;
            if (g_loading_binds) {
                /* duplicate key in the config file: report and keep the default */
                snprintf(err, cap, "'%s' already bound to '%s'", keyspec,
                         action_name(g_user_binds[i].act));
                return 0;
            }
            /* an explicit 'terminator keybind' may rebind a key */
            g_user_binds[i].act = a;
            return 1;
        }
    }
    if (g_n_user_binds >= (int)(sizeof g_user_binds / sizeof g_user_binds[0])) {
        snprintf(err, cap, "too many bindings");
        return 0;
    }
    g_user_binds[g_n_user_binds].mods = mods;
    g_user_binds[g_n_user_binds].ks = ks;
    g_user_binds[g_n_user_binds].act = a;
    g_n_user_binds++;
    return 1;
}

static Action bindings_lookup(unsigned int mods, KeySym ks)
{
    for (int i = 0; i < g_n_user_binds; i++)
        if (g_user_binds[i].mods == mods && ks_equal(g_user_binds[i].ks, ks))
            return g_user_binds[i].act;
    for (int i = 0; i < g_n_def_binds; i++)
        if (g_def_binds[i].mods == mods && ks_equal(g_def_binds[i].ks, ks))
            return g_def_binds[i].act;
    return ACT_NONE;
}

static int bindings_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fprintf(f, "# Terminator key bindings -- generated by 'terminator keybind'\n");
    for (int i = 0; i < g_n_user_binds; i++) {
        char spec[128];
        int p = 0;
        spec[0] = '\0';
        if (g_user_binds[i].mods & ControlMask) p += snprintf(spec + p, sizeof spec - p, "Ctrl+");
        if (g_user_binds[i].mods & ShiftMask)   p += snprintf(spec + p, sizeof spec - p, "Shift+");
        if (g_user_binds[i].mods & Mod1Mask)    p += snprintf(spec + p, sizeof spec - p, "Alt+");
        {
            const char *n = XKeysymToString(g_user_binds[i].ks);
            if (n) snprintf(spec + p, sizeof spec - p, "%s", n);
        }
        fprintf(f, "bind %s %s\n", spec, action_name(g_user_binds[i].act));
    }
    fclose(f);
    return 1;
}

static int bindings_load(const char *path, int verbose)
{
    FILE *f = fopen(path, "r");
    char line[512];

    g_n_user_binds = 0;
    g_loading_binds = 1;
    if (!f) {
        g_loading_binds = 0;
        if (verbose)
            fprintf(stderr, "%s: no config at %s\n", APP_NAME, path);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = line, *kw, *spec, *act;
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#')
            continue;
        kw = strtok(p, " \t");
        if (!kw || (strcasecmp(kw, "bind") && strcasecmp(kw, "keybind")))
            continue;
        spec = strtok(NULL, " \t");
        act = strtok(NULL, " \t");
        if (!spec || !act) {
            fprintf(stderr, "%s: %s: malformed binding: %s\n", APP_NAME, path, line);
            continue;
        }
        {
            char err[128];
            if (!bindings_add_user(spec, act, err, sizeof err))
                fprintf(stderr, "%s: %s: %s (keeping default)\n", APP_NAME, path, err);
        }
    }
    fclose(f);
    g_loading_binds = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* GUI                                                                */
/* ------------------------------------------------------------------ */

static Shared *g_sh[MAX_TABS];
static char    g_tab_cwd[MAX_TABS][PATH_MAX];
static unsigned char g_ctlbuf[MAX_TABS][1024];   /* partial control-message frames */
static int           g_ctlblen[MAX_TABS];

static Atom g_text_atom;
static Atom g_paste_prop;
static int  g_paste_tab = -1;

static void tab_append_line(Tab *t, const char *s)
{
    if (t->nlines == t->cap_lines) {
        int nc = t->cap_lines ? t->cap_lines * 2 : 256;
        char **nl = realloc(t->lines, (size_t)nc * sizeof(char *));
        if (!nl)
            return;
        t->lines = nl;
        t->cap_lines = nc;
    }
    if (t->nlines >= MAX_SCROLLBACK) {
        free(t->lines[0]);
        memmove(t->lines, t->lines + 1, (size_t)(t->nlines - 1) * sizeof(char *));
        t->nlines--;
    }
    t->lines[t->nlines++] = xstrdup(s);
}

static void short_path(const char *cwd, char *out, int cap)
{
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;

    if (home && hl && !strncmp(cwd, home, hl) &&
        (cwd[hl] == '/' || cwd[hl] == '\0'))
        snprintf(out, cap, "~%s", cwd + hl);
    else
        snprintf(out, cap, "%s", cwd);
}

static void gui_append_output(Tab *t, const char *data, int n, int idx)
{
    int i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];

        if (t->in_prompt) {
            if (c == PMARK_END) {
                t->in_prompt = 0;
                t->prompt[t->prompt_len] = '\0';
                t->awaiting = 1;
            } else if (c == PMARK_SEP) {
                t->prompt[t->prompt_len] = '\0';
                snprintf(g_tab_cwd[idx], sizeof g_tab_cwd[idx], "%s", t->prompt);
                t->prompt_len = 0;
            } else if (t->prompt_len < (int)sizeof t->prompt - 1) {
                t->prompt[t->prompt_len++] = (char)c;
            }
            continue;
        }

        if (c == PMARK_START) { t->in_prompt = 1; t->prompt_len = 0; continue; }
        if (c == '\f') { t->nlines = 0; t->pending_len = 0; continue; }
        if (c == '\r') continue;
        if (c == '\n') {
            t->pending[t->pending_len] = '\0';
            tab_append_line(t, t->pending);
            t->pending_len = 0;
            continue;
        }
        if (c == '\t') {
            for (int k = 0; k < 8 && t->pending_len < MAX_LINE_LEN - 1; k++)
                t->pending[t->pending_len++] = ' ';
            continue;
        }
        if (c < 32) continue;
        if (t->pending_len < MAX_LINE_LEN - 1)
            t->pending[t->pending_len++] = (char)c;
    }
    t->dirty = 1;
}

static int input_line_count(Tab *t)
{
    int n = 1;
    if (t->searching)
        return 1;
    for (int i = 0; i < t->len; i++)
        if (t->input[i] == '\n')
            n++;
    return n;
}

/* m-th display line of the input area, including its prompt */
static int input_line_at(Tab *t, int m, char *buf, int cap)
{
    int seg = 0, i = 0, len = 0;

    if (t->searching)
        return snprintf(buf, cap, "Enter search term: %s", t->search_buf);

    while (seg < m) {
        while (i < t->len && t->input[i] != '\n') i++;
        if (i < t->len) i++;
        seg++;
    }
    len = snprintf(buf, cap, "%s", m == 0 ? t->prompt : PROMPT_CONT);
    while (i < t->len && t->input[i] != '\n' && len < cap - 1)
        buf[len++] = t->input[i++];
    buf[len] = '\0';
    return len;
}

static int vline_count(Tab *t)
{
    int pend = t->pending_len > 0 ? 1 : 0;
    return t->nlines + pend + (t->awaiting ? input_line_count(t) : 0);
}

static int vline_at(Tab *t, int k, char *buf, int cap)
{
    int nb = t->nlines;
    int pend = t->pending_len > 0 ? 1 : 0;

    if (k < nb) {
        snprintf(buf, cap, "%s", t->lines[k]);
        return (int)strlen(buf);
    }
    if (pend && k == nb) {
        snprintf(buf, cap, "%s", t->pending);
        return (int)strlen(buf);
    }
    if (t->awaiting)
        return input_line_at(t, k - nb - pend, buf, cap);
    buf[0] = '\0';
    return 0;
}

static int text_width(const char *s, int len)
{
    if (g_font)
        return XTextWidth(g_font, s, len);
    return len * g_cw;
}

static void gui_draw(void)
{
    XWindowAttributes attr;
    char buf[MAX_LINE_LEN + 64];
    int i, visible, total, start, k;
    int pad = 6;
    int top = TAB_HEIGHT + 4;

    if (!XGetWindowAttributes(g_dpy, g_win, &attr))
        return;
    g_win_w = attr.width;
    g_win_h = attr.height;

    XSetForeground(g_dpy, g_gc, BlackPixel(g_dpy, g_scr));
    XFillRectangle(g_dpy, g_win, g_gc, 0, 0, attr.width, attr.height);

    /* ---- tab bar ---- */
    XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
    for (i = 0; i < g_tab_count; i++) {
        int x = i * TAB_WIDTH;
        char label[PATH_MAX + 32];
        char sp[PATH_MAX];
        short_path(g_tab_cwd[i][0] ? g_tab_cwd[i] : ".", sp, sizeof sp);
        snprintf(label, sizeof label, "[ %d: %s ]", i + 1, sp);
        if ((int)strlen(label) > 14)
            label[14] = '\0';

        if (i == g_active) {
            /* active tab: white block, black text */
            XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
            XFillRectangle(g_dpy, g_win, g_gc, x + 1, 2, TAB_WIDTH - 3, TAB_HEIGHT - 4);
            XSetForeground(g_dpy, g_gc, BlackPixel(g_dpy, g_scr));
        } else {
            XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
            XDrawRectangle(g_dpy, g_win, g_gc, x + 1, 2, TAB_WIDTH - 3, TAB_HEIGHT - 4);
        }
        XDrawString(g_dpy, g_win, g_gc, x + 5,
                    (TAB_HEIGHT + g_font->ascent) / 2 + 3, label, (int)strlen(label));
        /* close box */
        XDrawRectangle(g_dpy, g_win, g_gc, x + TAB_WIDTH - 20, 7, 12, 12);
        XDrawString(g_dpy, g_win, g_gc, x + TAB_WIDTH - 17, 17, "x", 1);
    }
    /* add-tab button */
    XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
    XDrawRectangle(g_dpy, g_win, g_gc, g_tab_count * TAB_WIDTH + 2, 2, 40, TAB_HEIGHT - 4);
    XDrawString(g_dpy, g_win, g_gc, g_tab_count * TAB_WIDTH + 12, 20, "+", 1);

    /* ---- text area ---- */
    {
        Tab *t = &g_tabs[g_active];
        int sel_a = -1, sel_b = -1;

        visible = (attr.height - top - pad) / g_ch;
        if (visible < 1)
            visible = 1;
        total = vline_count(t);
        if (t->scroll < 0) t->scroll = 0;
        if (t->scroll > (total > visible ? total - visible : 0))
            t->scroll = (total > visible ? total - visible : 0);
        start = total - visible - t->scroll;
        if (start < 0) start = 0;

        if (t->sel_active) {
            sel_a = t->sel_l0 * 100000 + t->sel_c0;
            sel_b = t->sel_l1 * 100000 + t->sel_c1;
            if (sel_a > sel_b) { int tmp = sel_a; sel_a = sel_b; sel_b = tmp; }
        }

        for (k = start; k < total && k < start + visible; k++) {
            int y = top + (k - start) * g_ch;
            int len = vline_at(t, k, buf, sizeof buf);

            if (t->sel_active && k * 100000 >= sel_a && k * 100000 <= sel_b) {
                /* selected line: white block, black text */
                XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
                XFillRectangle(g_dpy, g_win, g_gc, pad, y, attr.width - 2 * pad, g_ch);
                XSetForeground(g_dpy, g_gc, BlackPixel(g_dpy, g_scr));
            } else {
                XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
            }
            XDrawString(g_dpy, g_win, g_gc, pad, y + g_font->ascent, buf, len);
        }

        /* cursor */
        if (t->awaiting && t->scroll == 0) {
            const char *prompt_str;
            char seg[MAX_CMD];
            int m = 0, segstart = 0, i2, sl = 0, cx, cy;

            if (t->searching) {
                prompt_str = "Enter search term: ";
                snprintf(seg, sizeof seg, "%s", t->search_buf);
                sl = (int)strlen(seg);
            } else {
                for (i2 = 0; i2 < t->cursor; i2++)
                    if (t->input[i2] == '\n') { m++; segstart = i2 + 1; }
                for (i2 = segstart; i2 < t->cursor; i2++)
                    seg[sl++] = t->input[i2];
                seg[sl] = '\0';
                prompt_str = (m == 0) ? t->prompt : PROMPT_CONT;
            }
            cx = pad + text_width(prompt_str, (int)strlen(prompt_str));
            cx += text_width(seg, sl);
            cy = top + (t->nlines + (t->pending_len > 0 ? 1 : 0) + m - start) * g_ch;
            XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
            XDrawLine(g_dpy, g_win, g_gc, cx, cy, cx, cy + g_ch - 2);
        }
    }

    XFlush(g_dpy);
}

/* ------------------------------------------------------------------ */
/* selection / clipboard                                              */
/* ------------------------------------------------------------------ */

static int build_selection(Tab *t, char *out, int cap)
{
    int l0 = t->sel_l0, c0 = t->sel_c0, l1 = t->sel_l1, c1 = t->sel_c1;
    int a, b, len = 0;
    char buf[MAX_LINE_LEN + 64];

    if (!t->sel_active)
        return 0;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) { a = l1; b = l0; }
    else { a = l0; b = l1; }

    for (int k = a; k <= b; k++) {
        int n = vline_at(t, k, buf, sizeof buf);
        int s = (k == a) ? c0 : 0;
        int e = (k == b) ? c1 : n;
        if (s < 0) s = 0;
        if (e > n) e = n;
        if (e > s) {
            int take = e - s;
            if (len + take >= cap - 2) take = cap - 2 - len;
            if (take > 0) { memcpy(out + len, buf + s, (size_t)take); len += take; }
        }
        if (k != b && len < cap - 1)
            out[len++] = '\n';
    }
    out[len] = '\0';
    return len;
}

static void gui_copy(Tab *t)
{
    char *txt;
    int n;

    txt = malloc(65536);
    if (!txt)
        return;
    n = build_selection(t, txt, 65536);
    if (n <= 0) { free(txt); return; }
    snprintf(g_sel_text, sizeof g_sel_text, "%s", txt);
    g_sel_len = (int)strlen(g_sel_text);
    free(txt);

    XSetSelectionOwner(g_dpy, XA_PRIMARY, g_win, CurrentTime);
    XSetSelectionOwner(g_dpy, g_clipboard, g_win, CurrentTime);
}

static void gui_request_paste(int idx, Atom sel)
{
    g_paste_tab = idx;
    XConvertSelection(g_dpy, sel, g_utf8, g_paste_prop, g_win, CurrentTime);
}

static void gui_do_paste(const unsigned char *data, int n)
{
    Tab *t;
    int i;

    if (g_paste_tab < 0 || g_paste_tab >= g_tab_count)
        return;
    t = &g_tabs[g_paste_tab];
    for (i = 0; i < n; i++) {
        unsigned char c = data[i];
        if (c == '\r') continue;
        if (c != '\n' && c != '\t' && c < 32) continue;
        if (t->len < MAX_CMD - 1) {
            memmove(t->input + t->cursor + 1, t->input + t->cursor,
                    (size_t)(t->len - t->cursor));
            t->input[t->cursor] = (char)c;
            t->len++;
            t->cursor++;
        }
    }
    t->input[t->len] = '\0';
    t->dirty = 1;
}

static void handle_selection_request(XSelectionRequestEvent *r)
{
    XSelectionEvent resp;
    Atom prop = r->property == None ? r->target : r->property;

    resp.type = SelectionNotify;
    resp.display = g_dpy;
    resp.requestor = r->requestor;
    resp.selection = r->selection;
    resp.target = r->target;
    resp.time = r->time;
    resp.property = None;

    if (r->target == g_targets) {
        Atom list[4];
        list[0] = g_utf8;
        list[1] = XA_STRING;
        list[2] = g_text_atom;
        list[3] = g_targets;
        XChangeProperty(g_dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)list, 4);
        resp.property = prop;
    } else if (r->target == g_utf8 || r->target == XA_STRING ||
               r->target == g_text_atom) {
        XChangeProperty(g_dpy, r->requestor, prop, r->target, 8, PropModeReplace,
                        (unsigned char *)g_sel_text, g_sel_len);
        resp.property = prop;
    }
    XSendEvent(g_dpy, r->requestor, False, 0, (XEvent *)&resp);
}

static void handle_selection_notify(XSelectionEvent *s)
{
    Atom type;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;

    if (s->property == None)
        return;
    if (XGetWindowProperty(g_dpy, g_win, g_paste_prop, 0, (1 << 20), False,
                           AnyPropertyType, &type, &fmt, &nitems, &after,
                           &data) == Success && data) {
        gui_do_paste(data, (int)nitems);
        XFree(data);
    }
    XDeleteProperty(g_dpy, g_win, g_paste_prop);
}

/* ------------------------------------------------------------------ */
/* line editing actions                                               */
/* ------------------------------------------------------------------ */

static void hist_prev(Tab *t)
{
    int c = history_count();
    if (c == 0)
        return;
    if (t->hist_pos == -1) {
        snprintf(t->hist_draft, sizeof t->hist_draft, "%s", t->input);
        t->hist_pos = c - 1;
    } else if (t->hist_pos > 0) {
        t->hist_pos--;
    } else {
        return;
    }
    if (history_get(t->hist_pos, t->input, sizeof t->input)) {
        t->len = (int)strlen(t->input);
        t->cursor = t->len;
    }
}

static void hist_next(Tab *t)
{
    int c = history_count();
    if (t->hist_pos == -1)
        return;
    t->hist_pos++;
    if (t->hist_pos >= c) {
        t->hist_pos = -1;
        snprintf(t->input, sizeof t->input, "%s", t->hist_draft);
    } else {
        history_get(t->hist_pos, t->input, sizeof t->input);
    }
    t->len = (int)strlen(t->input);
    t->cursor = t->len;
}

static void kill_word(Tab *t)
{
    int i = t->cursor;
    while (i > 0 && t->input[i - 1] == ' ') i--;
    while (i > 0 && t->input[i - 1] != ' ') i--;
    memmove(t->input + i, t->input + t->cursor, (size_t)(t->len - t->cursor));
    t->len -= (t->cursor - i);
    t->cursor = i;
    t->input[t->len] = '\0';
}

static void gui_interrupt(Tab *t, int idx)
{
    Shared *sh = g_sh[idx];
    int had_job = 0;

    if (sh && sh->mw_active) {
        if (sh->mw_pgid > 0) kill(-sh->mw_pgid, SIGINT);
        sh->mw_cancel = 1;
        had_job = 1;
    } else if (sh && sh->fg_pgid > 0) {
        kill(-sh->fg_pgid, SIGINT);
        had_job = 1;
    }

    tab_append_line(t, "^C");
    t->input[0] = '\0';
    t->len = 0;
    t->cursor = 0;
    t->hist_pos = -1;
    t->choosing = 0;
    t->awaiting = had_job ? 0 : 1;
    t->dirty = 1;
}

static void gui_submit(Tab *t, int idx)
{
    char buf[MAX_LINE_LEN + 64];
    char *p = t->input;
    int first = 1;

    (void)idx;
    for (;;) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        snprintf(buf, sizeof buf, "%s%s", first ? t->prompt : PROMPT_CONT, p);
        tab_append_line(t, buf);
        if (!nl)
            break;
        p = nl + 1;
        first = 0;
    }

    if (t->in_w >= 0) {
        write_all(t->in_w, t->input, strlen(t->input));
        write_all(t->in_w, "\n", 1);
    }
    t->input[0] = '\0';
    t->len = 0;
    t->cursor = 0;
    t->hist_pos = -1;
    t->choosing = 0;
    t->awaiting = 0;
    t->dirty = 1;
}

/* ---------------- Ctrl+Z (Task 8) ---------------- */

static void gui_suspend(Tab *t, int idx)
{
    Shared *sh = g_sh[idx];

    if (!sh || sh->fg_pgid <= 0)
        return;                       /* nothing running in the foreground */
    kill(-sh->fg_pgid, SIGTSTP);      /* keys are X events: we send it ourselves */
    tab_append_line(t, "^Z");
    t->input[0] = '\0';
    t->len = 0;
    t->cursor = 0;
    t->hist_pos = -1;
    t->choosing = 0;
    t->awaiting = 0;                  /* the worker prints Stopped + a new prompt */
    t->dirty = 1;
}

/* ---------------- Ctrl+R search (Task 9) ---------------- */

/* length of the longest common *substring* of a and b */
static int lcs_len(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    static int prev[513], cur[513];
    int i, j, best = 0;

    if (la > 512) la = 512;
    if (lb > 512) lb = 512;
    if (la == 0 || lb == 0)
        return 0;

    memset(prev, 0, sizeof(int) * (size_t)(lb + 1));
    for (i = 1; i <= la; i++) {
        cur[0] = 0;
        for (j = 1; j <= lb; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best) best = cur[j];
            } else {
                cur[j] = 0;
            }
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return best;
}

static void history_search(Tab *t, const char *term)
{
    char b[HIST_ENTRY];
    int total = history_count();
    int i, start;

    if (total == 0 || !term || !*term) {
        tab_append_line(t, "No match for search term in history");
        return;
    }

    /* (a) the most recent command that exactly matches the term */
    for (i = total - 1; i >= 0; i--) {
        if (!history_get(i, b, sizeof b)) continue;
        if (!strcmp(b, term)) { tab_append_line(t, b); return; }
    }

    /* (b) the command(s) sharing the longest common substring with the term,
     *     if that match is longer than 2 characters */
    start = total > HIST_SCAN ? total - HIST_SCAN : 0;
    {
        int best = 0;
        for (i = start; i < total; i++) {
            int L;
            if (!history_get(i, b, sizeof b)) continue;
            L = lcs_len(term, b);
            if (L > best) best = L;
        }
        if (best > 2) {
            for (i = total - 1; i >= start; i--) {
                if (!history_get(i, b, sizeof b)) continue;
                if (lcs_len(term, b) == best)
                    tab_append_line(t, b);
            }
            return;
        }
    }

    /* (c) nothing matched */
    tab_append_line(t, "No match for search term in history");
}

/* keystrokes while the "Enter search term: " prompt is open */
static void gui_search_key(Tab *t, KeySym ks, int ctrl, int alt, char *text, int n)
{
    if (ks == XK_Return || ks == XK_KP_Enter) {
        char term[512], line[600];
        snprintf(term, sizeof term, "%s", t->search_buf);
        snprintf(line, sizeof line, "Enter search term: %s", term);
        tab_append_line(t, line);
        t->searching = 0;
        t->search_buf[0] = '\0';
        t->search_len = 0;
        history_search(t, term);
        t->dirty = 1;
        return;
    }
    if (ks == XK_BackSpace || ks == XK_Delete) {
        if (t->search_len > 0)
            t->search_buf[--t->search_len] = '\0';
        t->dirty = 1;
        return;
    }
    if (ctrl && (ks == XK_c || ks == XK_g || ks == XK_r)) {
        t->searching = 0;
        t->search_buf[0] = '\0';
        t->search_len = 0;
        t->dirty = 1;
        return;
    }
    if (!ctrl && !alt && n > 0) {
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c < 32 || c == 127) continue;
            if (t->search_len < (int)sizeof t->search_buf - 1)
                t->search_buf[t->search_len++] = (char)c;
        }
        t->search_buf[t->search_len] = '\0';
        t->dirty = 1;
    }
}

/* ---------------- Tab completion (Task 10) ---------------- */

static void replace_word(Tab *t, int start, int end, const char *s)
{
    int sl = (int)strlen(s);

    memmove(t->input + start + sl, t->input + end, (size_t)(t->len - end));
    memcpy(t->input + start, s, (size_t)sl);
    t->len = t->len - (end - start) + sl;
    t->cursor = start + sl;
    t->input[t->len] = '\0';
}

static void print_choice_list(Tab *t)
{
    char line[MAX_LINE_LEN];
    int lp = 0, i;

    line[0] = '\0';
    for (i = 0; i < t->nchoices; i++) {
        char item[PATH_MAX + 16];
        int need;
        snprintf(item, sizeof item, "%d. %s", i + 1, t->choices[i]);
        need = (int)strlen(item) + (lp ? 1 : 0);
        if (lp && lp + need > 76) {
            tab_append_line(t, line);
            lp = 0;
            line[0] = '\0';
        }
        if (lp) { strcat(line, " "); lp++; }
        strcat(line, item);
        lp += (int)strlen(item);
    }
    if (lp)
        tab_append_line(t, line);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void gui_complete(Tab *t, int idx)
{
    char word[MAX_CMD], dir[PATH_MAX], base[MAX_CMD], scan[PATH_MAX];
    char *matches[64];
    int nm = 0, i, ws, wl;
    const char *cwd = g_tab_cwd[idx][0] ? g_tab_cwd[idx] : ".";
    DIR *d;
    struct dirent *de;

    if (!t->awaiting || t->len >= MAX_CMD - 1)
        return;

    /* the word immediately before the cursor */
    ws = t->cursor;
    while (ws > 0 && !isspace((unsigned char)t->input[ws - 1]))
        ws--;
    wl = t->cursor - ws;
    if (wl >= (int)sizeof word)
        return;
    memcpy(word, t->input + ws, (size_t)wl);
    word[wl] = '\0';

    /* allow a path prefix, though the brief only needs the current directory */
    {
        const char *slash = strrchr(word, '/');
        if (slash) {
            int dl = (int)(slash - word) + 1;
            memcpy(dir, word, (size_t)dl);
            dir[dl] = '\0';
            snprintf(base, sizeof base, "%s", slash + 1);
        } else {
            dir[0] = '\0';
            snprintf(base, sizeof base, "%s", word);
        }
    }

    if (dir[0] == '/') {
        if (!str_cat2(scan, sizeof scan, dir, NULL))
            return;
    } else if (dir[0]) {
        char base2[PATH_MAX];
        if (!str_cat2(base2, sizeof base2, cwd, "/") ||
            !str_cat2(scan, sizeof scan, base2, dir))
            return;
    } else {
        if (!str_cat2(scan, sizeof scan, cwd, NULL))
            return;
    }

    d = opendir(scan);
    if (!d)
        return;
    while ((de = readdir(d)) != NULL && nm < 64) {
        char full[PATH_MAX];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (strncmp(de->d_name, base, strlen(base)))
            continue;
        if (!str_cat2(full, sizeof full, dir, de->d_name))
            continue;                    /* name too long to offer */
        matches[nm++] = xstrdup(full);
    }
    closedir(d);

    if (nm > 1)                       /* deterministic, bash-like order */
        qsort(matches, (size_t)nm, sizeof(char *), cmp_str);

    if (nm == 0) {                    /* no match: leave the line alone */
        return;
    }

    if (nm == 1) {                    /* exactly one: complete it */
        replace_word(t, ws, t->cursor, matches[0]);
        free(matches[0]);
        t->dirty = 1;
        return;
    }

    /* several: extend to the longest common prefix ... */
    {
        char lcp[PATH_MAX];
        snprintf(lcp, sizeof lcp, "%s", matches[0]);
        for (i = 1; i < nm; i++) {
            int k = 0;
            while (lcp[k] && matches[i][k] && lcp[k] == matches[i][k])
                k++;
            lcp[k] = '\0';
        }
        if (strlen(lcp) > strlen(word))
            replace_word(t, ws, t->cursor, lcp);

        /* ... and it is still ambiguous: offer a numbered list */
        t->choosing = 1;
        t->nchoices = nm;
        t->wstart = ws;
        t->wend = t->cursor;
        for (i = 0; i < nm; i++) {
            snprintf(t->choices[i], sizeof t->choices[i], "%s", matches[i]);
            free(matches[i]);
        }
        print_choice_list(t);
        t->dirty = 1;
    }
}

static void gui_do_action(Action a, Tab *t, int idx)
{
    switch (a) {
    case ACT_LEFT:      if (t->cursor > 0) t->cursor--; break;
    case ACT_RIGHT:     if (t->cursor < t->len) t->cursor++; break;
    case ACT_BOL:       t->cursor = 0; break;
    case ACT_EOL:       t->cursor = t->len; break;
    case ACT_HIST_PREV: hist_prev(t); break;
    case ACT_HIST_NEXT: hist_next(t); break;
    case ACT_KILL_START:
        memmove(t->input, t->input + t->cursor, (size_t)(t->len - t->cursor));
        t->len -= t->cursor;
        t->cursor = 0;
        t->input[t->len] = '\0';
        break;
    case ACT_KILL_END:
        t->len = t->cursor;
        t->input[t->len] = '\0';
        break;
    case ACT_KILL_WORD: kill_word(t); break;
    case ACT_CLEAR:     t->nlines = 0; t->pending_len = 0; t->scroll = 0; break;
    case ACT_EXIT:
        if (t->len == 0) { gui_close_tab(idx); return; }
        break;
    case ACT_INTERRUPT: gui_interrupt(t, idx); return;
    case ACT_COPY:      gui_copy(t); return;
    case ACT_PASTE:     gui_request_paste(idx, g_clipboard); return;
    case ACT_NEW_TAB:   gui_new_tab(); return;
    case ACT_CLOSE_TAB: gui_close_tab(idx); return;
    case ACT_NEXT_TAB:  if (g_tab_count > 1) { g_active = (g_active + 1) % g_tab_count; } break;
    case ACT_PREV_TAB:  if (g_tab_count > 1) { g_active = (g_active - 1 + g_tab_count) % g_tab_count; } break;
    case ACT_PAGE_UP:   g_tabs[g_active].scroll += 5; break;
    case ACT_PAGE_DOWN: g_tabs[g_active].scroll -= 5; if (g_tabs[g_active].scroll < 0) g_tabs[g_active].scroll = 0; break;
    case ACT_SUSPEND:   gui_suspend(t, idx); return;
    case ACT_SEARCH:
        if (t->awaiting) {
            t->searching = 1;
            t->search_len = 0;
            t->search_buf[0] = '\0';
            t->choosing = 0;
            t->dirty = 1;
        }
        return;
    case ACT_COMPLETE:  gui_complete(t, idx); return;
    default: break;
    }
    g_tabs[g_active].dirty = 1;
}

static void gui_handle_key(XKeyEvent *ke)
{
    Tab *t = &g_tabs[g_active];
    char text[64];
    KeySym ks = NoSymbol;
    int n, ctrl, shift, alt;
    unsigned int mods;
    Action a;

    n = XLookupString(ke, text, sizeof text, &ks, NULL);
    ctrl = (ke->state & ControlMask) != 0;
    shift = (ke->state & ShiftMask) != 0;
    alt = (ke->state & Mod1Mask) != 0;
    mods = 0;
    if (ctrl) mods |= ControlMask;
    if (shift) mods |= ShiftMask;
    if (alt) mods |= Mod1Mask;

    /* the Ctrl+R search prompt owns the keyboard while it is open */
    if (t->searching) {
        gui_search_key(t, ks, ctrl, alt, text, n);
        gui_draw();
        return;
    }

    a = bindings_lookup(mods, ks);
    if (a != ACT_NONE) {
        gui_do_action(a, t, g_active);
        gui_draw();
        return;
    }

    /* the numbered completion chooser: a digit picks an entry */
    if (t->choosing && t->awaiting) {
        if (n > 0 && text[0] >= '1' && text[0] <= '9' &&
            text[0] - '0' <= t->nchoices) {
            int pick = text[0] - '0';
            int nl = (int)strlen(t->choices[pick - 1]);
            memmove(t->input + t->wstart + nl, t->input + t->wend,
                    (size_t)(t->len - t->wend));
            memcpy(t->input + t->wstart, t->choices[pick - 1], (size_t)nl);
            t->len = t->wstart + nl + (t->len - t->wend);
            t->cursor = t->wstart + nl;
            t->input[t->len] = '\0';
        }
        t->choosing = 0;
        t->dirty = 1;
        gui_draw();
        return;
    }

    if (!t->awaiting) {
        /* still allow tab switching / redraw hotkeys while a command runs */
        return;
    }

    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (command_incomplete(t->input)) {
            if (t->len < MAX_CMD - 1) {
                memmove(t->input + t->cursor + 1, t->input + t->cursor,
                        (size_t)(t->len - t->cursor));
                t->input[t->cursor] = '\n';
                t->len++;
                t->cursor++;
                t->input[t->len] = '\0';
            }
        } else {
            gui_submit(t, g_active);
        }
        gui_draw();
        return;
    }

    if (ks == XK_BackSpace || ks == XK_Delete) {
        if (t->cursor > 0) {
            memmove(t->input + t->cursor - 1, t->input + t->cursor,
                    (size_t)(t->len - t->cursor));
            t->cursor--;
            t->len--;
            t->input[t->len] = '\0';
        }
        gui_draw();
        return;
    }

    if (ctrl || alt)
        return;

    if (n > 0 && (unsigned char)text[0] >= 32) {
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c < 32 || c == 127)
                continue;
            if (t->len < MAX_CMD - 1) {
                memmove(t->input + t->cursor + 1, t->input + t->cursor,
                        (size_t)(t->len - t->cursor));
                t->input[t->cursor] = (char)c;
                t->len++;
                t->cursor++;
            }
        }
        t->input[t->len] = '\0';
        t->dirty = 1;
        gui_draw();
    }
}

/* ------------------------------------------------------------------ */
/* tabs                                                               */
/* ------------------------------------------------------------------ */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int gui_new_tab(void)
{
    int idx = g_tab_count;
    Tab *t;
    int in[2], out[2], ctl[2];
    Shared *sh;
    WorkerCtx *ctx;

    if (idx >= MAX_TABS)
        return -1;

    if (pipe(in) < 0 || pipe(out) < 0 || pipe(ctl) < 0)
        return -1;

    /* The GUI must never block in read(): it drains these two ends between
     * poll() wakeups.  The worker's ends stay blocking on purpose. */
    set_nonblock(out[0]);
    set_nonblock(ctl[0]);

    sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        close(ctl[0]); close(ctl[1]);
        return -1;
    }
    memset(sh, 0, sizeof *sh);
    g_sh[idx] = sh;
    g_tab_cwd[idx][0] = '\0';

    t = &g_tabs[idx];
    memset(t, 0, sizeof *t);
    t->hist_pos = -1;
    t->in_r = in[0]; t->in_w = in[1];
    t->out_r = out[0]; t->out_w = out[1];
    t->ctl_r = ctl[0]; t->ctl_w = ctl[1];
    t->alive = 1;
    t->dirty = 1;

    ctx = malloc(sizeof *ctx);
    if (!ctx)
        return -1;
    ctx->in_fd = in[0];
    ctx->out_fd = out[1];
    ctx->ctl_fd = ctl[1];
    ctx->hist = g_hist;
    ctx->sh = sh;
    ctx->mode = g_mode;

    if (g_mode == MODE_PROCESS) {
        pid_t pid = fork();
        if (pid == 0) {
            close(in[1]); close(out[0]); close(ctl[0]);
            signal(SIGCHLD, SIG_DFL);
            worker_loop(ctx);
            _exit(0);
        }
        if (pid < 0) {
            free(ctx);
            return -1;
        }
        t->pid = pid;
        t->tid = 0;
        close(in[0]); close(out[1]); close(ctl[1]);
    } else {
        if (pthread_create(&t->tid, NULL, worker_thread, ctx) != 0) {
            free(ctx);
            return -1;
        }
        /* threads share the fd table, so the worker's ends stay open */
    }

    g_tab_count++;
    g_active = idx;
    return idx;
}

static void gui_close_tab(int idx)
{
    Tab *t;
    int i;

    if (idx < 0 || idx >= g_tab_count)
        return;

    if (g_tab_count == 1) {
        g_running = 0;
        return;
    }

    t = &g_tabs[idx];

    if (g_sh[idx]) {
        if (g_sh[idx]->fg_pgid > 0) kill(-g_sh[idx]->fg_pgid, SIGHUP);
        if (g_sh[idx]->mw_active && g_sh[idx]->mw_pgid > 0)
            kill(-g_sh[idx]->mw_pgid, SIGINT);
    }

    if (t->in_w >= 0) { close(t->in_w); t->in_w = -1; }
    if (t->out_r >= 0) { close(t->out_r); t->out_r = -1; }
    if (t->ctl_r >= 0) { close(t->ctl_r); t->ctl_r = -1; }

    if (g_mode == MODE_PROCESS) {
        if (t->pid > 0) { kill(t->pid, SIGKILL); while (waitpid(t->pid, NULL, 0) < 0 && errno == EINTR) {} }
    }
    /* thread mode: the worker exits on its own once its read() sees EOF */

    for (i = 0; i < t->nlines; i++)
        free(t->lines[i]);
    free(t->lines);

    for (i = idx; i < g_tab_count - 1; i++) {
        g_tabs[i] = g_tabs[i + 1];
        g_sh[i] = g_sh[i + 1];
        memcpy(g_tab_cwd[i], g_tab_cwd[i + 1], PATH_MAX);
    }
    memset(&g_tabs[g_tab_count - 1], 0, sizeof(Tab));
    g_sh[g_tab_count - 1] = NULL;
    g_tab_count--;

    if (g_active >= g_tab_count)
        g_active = g_tab_count - 1;
    g_tabs[g_active].dirty = 1;
}

/* ------------------------------------------------------------------ */
/* control messages from a worker                                     */
/* ------------------------------------------------------------------ */

static void gui_read_ctl(int idx)
{
    Tab *t = &g_tabs[idx];
    unsigned char *b = g_ctlbuf[idx];
    int *bl = &g_ctlblen[idx];

    for (;;) {
        ssize_t n;
        int off = 0;

        if (*bl >= (int)sizeof g_ctlbuf[idx])
            *bl = 0;                    /* desync: drop what we have */

        n = read(t->ctl_r, b + *bl, sizeof g_ctlbuf[idx] - (size_t)*bl);
        if (n > 0)
            *bl += (int)n;
        else if (n == 0) { gui_close_tab(idx); return; }
        else if (errno == EINTR) continue;
        else break;                     /* EAGAIN: nothing more right now */

        /* parse every complete frame in the buffer */
        while (off < *bl) {
            unsigned char type = b[off];

            if (type == CTL_EXIT) { gui_close_tab(idx); return; }
            if (type == CTL_RELOAD) {
                bindings_load(config_path(), 0);
                tab_append_line(t, "[configuration reloaded]");
                t->dirty = 1;
                off += 1;
                continue;
            }
            if (type == CTL_BIND) {
                unsigned short l1, l2;
                char spec[257], act[129], err[160];
                if (*bl - off < 5)
                    break;              /* need the two length fields */
                memcpy(&l1, b + off + 1, 2);
                memcpy(&l2, b + off + 3, 2);
                if (l1 > 256) l1 = 256;
                if (l2 > 128) l2 = 128;
                if (*bl - off < 5 + (int)l1 + (int)l2)
                    break;              /* frame not complete yet */
                memcpy(spec, b + off + 5, l1);
                spec[l1] = '\0';
                memcpy(act, b + off + 5 + l1, l2);
                act[l2] = '\0';
                if (bindings_add_user(spec, act, err, sizeof err))
                    bindings_save(config_path());
                off += 5 + l1 + l2;
                continue;
            }
            off += 1;                   /* unknown byte: resynchronise */
        }

        if (off > 0) {
            memmove(b, b + off, (size_t)(*bl - off));
            *bl -= off;
        }
    }
}

static void gui_read_output(int idx)
{
    Tab *t = &g_tabs[idx];
    char buf[8192];

    for (;;) {
        ssize_t n = read(t->out_r, buf, sizeof buf);
        if (n > 0) {
            gui_append_output(t, buf, (int)n, idx);
            continue;
        }
        if (n == 0) {
            /* worker closed the pipe -> session ended */
            gui_close_tab(idx);
            return;
        }
        if (errno == EINTR)
            continue;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* mouse                                                              */
/* ------------------------------------------------------------------ */

static void gui_tabbar_click(int x, int y)
{
    int i;
    (void)y;

    for (i = 0; i < g_tab_count; i++) {
        int tx = i * TAB_WIDTH;
        if (x >= tx && x < tx + TAB_WIDTH) {
            if (x >= tx + TAB_WIDTH - 22) {
                gui_close_tab(i);
            } else {
                g_active = i;
                g_tabs[i].dirty = 1;
            }
            return;
        }
    }
    if (x >= g_tab_count * TAB_WIDTH + 2 && x < g_tab_count * TAB_WIDTH + 44)
        gui_new_tab();
}

static void gui_mouse_text(Tab *t, int x, int y, int release)
{
    int top = TAB_HEIGHT + 4, pad = 6;
    int visible = (g_win_h - top - 6) / g_ch;
    int total = vline_count(t);
    int start;
    int line, col;
    char buf[MAX_LINE_LEN + 64];

    if (visible < 1) visible = 1;
    start = total - visible - t->scroll;
    if (start < 0) start = 0;

    line = start + (y - top) / g_ch;
    if (line < 0) line = 0;
    if (line >= total) line = total - 1;
    if (line < 0) return;

    vline_at(t, line, buf, sizeof buf);
    col = (x - pad) / (g_cw > 0 ? g_cw : 8);
    if (col < 0) col = 0;
    if (col > (int)strlen(buf)) col = (int)strlen(buf);

    if (!release) {
        t->sel_l0 = line; t->sel_c0 = col;
        t->sel_l1 = line; t->sel_c1 = col;
        t->sel_active = 1;
    } else {
        t->sel_l1 = line; t->sel_c1 = col;
        if (t->sel_l0 == t->sel_l1 && t->sel_c0 == t->sel_c1)
            t->sel_active = 0;
        else
            gui_copy(t);
    }
    t->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* event loop                                                         */
/* ------------------------------------------------------------------ */

static void gui_init(void)
{
    int screen;
    XGCValues v;
    unsigned long mask;
    Window win;

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "%s: cannot open display\n", APP_NAME);
        exit(1);
    }
    screen = DefaultScreen(g_dpy);
    g_scr = screen;

    win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen), 60, 60,
                              WIN_W, WIN_H, 2, BlackPixel(g_dpy, screen),
                              BlackPixel(g_dpy, screen));
    g_win = win;

    XSelectInput(g_dpy, win,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
                 EnterWindowMask);

    g_wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, win, &g_wm_delete, 1);
    XStoreName(g_dpy, win, "Terminator");

    g_font = XLoadQueryFont(g_dpy, "fixed");
    if (!g_font)
        g_font = XLoadQueryFont(g_dpy, "6x13");
    if (!g_font) {
        fprintf(stderr, "%s: cannot load a font\n", APP_NAME);
        exit(1);
    }
    g_cw = g_font->max_bounds.width;
    if (g_cw <= 0) g_cw = 8;
    g_ch = LINE_HEIGHT;
    if (g_font->ascent + g_font->descent + 2 > g_ch)
        g_ch = g_font->ascent + g_font->descent + 2;

    memset(&v, 0, sizeof v);
    v.foreground = WhitePixel(g_dpy, screen);   /* white text ... */
    v.background = BlackPixel(g_dpy, screen);   /* ... on black */
    v.font = g_font->fid;
    mask = GCForeground | GCBackground | GCFont;
    g_gc = XCreateGC(g_dpy, win, mask, &v);

    g_clipboard = XInternAtom(g_dpy, "CLIPBOARD", False);
    g_utf8 = XInternAtom(g_dpy, "UTF8_STRING", False);
    g_targets = XInternAtom(g_dpy, "TARGETS", False);
    g_text_atom = XInternAtom(g_dpy, "TEXT", False);
    g_paste_prop = XInternAtom(g_dpy, "TERMINATOR_PASTE", False);

    g_cursor = XCreateFontCursor(g_dpy, XC_xterm);
    {
        /* make the text cursor visible on the black background */
        XColor fg, bg;
        memset(&fg, 0, sizeof fg);
        memset(&bg, 0, sizeof bg);
        fg.pixel = WhitePixel(g_dpy, screen);
        bg.pixel = BlackPixel(g_dpy, screen);
        XQueryColor(g_dpy, DefaultColormap(g_dpy, screen), &fg);
        XQueryColor(g_dpy, DefaultColormap(g_dpy, screen), &bg);
        XRecolorCursor(g_dpy, g_cursor, &fg, &bg);
    }
    XDefineCursor(g_dpy, win, g_cursor);

    XMapWindow(g_dpy, win);
    XFlush(g_dpy);

    {
        XEvent ev;
        do { XNextEvent(g_dpy, &ev); } while (ev.type != MapNotify);
    }
    XSetInputFocus(g_dpy, win, RevertToParent, CurrentTime);
}

static void gui_run(void)
{
    gui_init();

    if (gui_new_tab() < 0) {
        fprintf(stderr, "%s: cannot create initial tab\n", APP_NAME);
        exit(1);
    }

    gui_draw();

    while (g_running) {
        struct pollfd pf[MAX_TABS * 2 + 2];
        int idx[MAX_TABS * 2 + 2];
        int nf = 0, i, r;

        pf[nf].fd = ConnectionNumber(g_dpy);
        pf[nf].events = POLLIN;
        idx[nf] = -1;
        nf++;

        for (i = 0; i < g_tab_count; i++) {
            if (g_tabs[i].out_r >= 0) {
                pf[nf].fd = g_tabs[i].out_r; pf[nf].events = POLLIN; idx[nf] = i * 2; nf++;
            }
            if (g_tabs[i].ctl_r >= 0) {
                pf[nf].fd = g_tabs[i].ctl_r; pf[nf].events = POLLIN; idx[nf] = i * 2 + 1; nf++;
            }
        }

        r = poll(pf, (nfds_t)nf, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Process X events.  We pull from the socket only when poll() says it
         * is readable, and otherwise drain events Xlib has already buffered.
         * XEventsQueued(QueuedAlready) never reads from the connection, so the
         * GUI can never block here. */
        {
            int have_x = (pf[0].revents & POLLIN) != 0;
            if (!have_x && XEventsQueued(g_dpy, QueuedAlready) > 0)
                have_x = 1;
            while (have_x) {
                XEvent ev;
                XNextEvent(g_dpy, &ev);
                switch (ev.type) {
                case Expose:
                    if (ev.xexpose.count == 0)
                        g_tabs[g_active].dirty = 1;
                    break;
                case ConfigureNotify:
                    g_win_w = ev.xconfigure.width;
                    g_win_h = ev.xconfigure.height;
                    g_tabs[g_active].dirty = 1;
                    break;
                case ClientMessage:
                    if ((Atom)ev.xclient.data.l[0] == g_wm_delete)
                        g_running = 0;
                    break;
                case KeyPress:
                    gui_handle_key(&ev.xkey);
                    break;
                case ButtonPress:
                    if (ev.xbutton.button == Button4) {
                        g_tabs[g_active].scroll += 3;
                        g_tabs[g_active].dirty = 1;
                    } else if (ev.xbutton.button == Button5) {
                        g_tabs[g_active].scroll -= 3;
                        if (g_tabs[g_active].scroll < 0) g_tabs[g_active].scroll = 0;
                        g_tabs[g_active].dirty = 1;
                    } else if (ev.xbutton.button == Button1) {
                        if (ev.xbutton.y < TAB_HEIGHT)
                            gui_tabbar_click(ev.xbutton.x, ev.xbutton.y);
                        else
                            gui_mouse_text(&g_tabs[g_active], ev.xbutton.x,
                                           ev.xbutton.y, 0);
                    } else if (ev.xbutton.button == Button2) {
                        gui_request_paste(g_active, XA_PRIMARY);
                    }
                    break;
                case ButtonRelease:
                    if (ev.xbutton.button == Button1 && ev.xbutton.y >= TAB_HEIGHT)
                        gui_mouse_text(&g_tabs[g_active], ev.xbutton.x,
                                       ev.xbutton.y, 1);
                    break;
                case MotionNotify:
                    if (ev.xmotion.y >= TAB_HEIGHT && ev.xmotion.state & Button1Mask)
                        gui_mouse_text(&g_tabs[g_active], ev.xmotion.x,
                                       ev.xmotion.y, 0);
                    break;
                case SelectionRequest:
                    handle_selection_request(&ev.xselectionrequest);
                    break;
                case SelectionNotify:
                    handle_selection_notify(&ev.xselection);
                    break;
                default:
                    break;
                }
                have_x = XEventsQueued(g_dpy, QueuedAlready) > 0;
            }
        }

        /* process pipe data */
        {
            int tabs_before = g_tab_count;
            for (i = 1; i < nf; i++) {
                int t;
                if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                t = idx[i] / 2;
                if (t < 0 || t >= g_tab_count)
                    continue;
                if (idx[i] % 2 == 0)
                    gui_read_output(t);
                else
                    gui_read_ctl(t);
                if (!g_running)
                    break;
                if (g_tab_count != tabs_before)
                    break;              /* indices moved: re-poll */
            }
        }

        for (i = 0; i < g_tab_count; i++)
            if (g_tabs[i].dirty) {
                g_tabs[i].dirty = 0;
                gui_draw();
                break;
            }
    }

    /* shut the workers down cleanly */
    for (int ci = 0; ci < g_tab_count; ci++) {
        Tab *t = &g_tabs[ci];
        int k;
        if (g_sh[ci]) {
            if (g_sh[ci]->fg_pgid > 0) kill(-g_sh[ci]->fg_pgid, SIGHUP);
            if (g_sh[ci]->mw_active && g_sh[ci]->mw_pgid > 0)
                kill(-g_sh[ci]->mw_pgid, SIGINT);
        }
        if (g_mode == MODE_PROCESS && t->pid > 0)
            kill(t->pid, SIGTERM);
        if (t->in_w >= 0) close(t->in_w);
        if (t->out_r >= 0) close(t->out_r);
        if (t->ctl_r >= 0) close(t->ctl_r);
        for (k = 0; k < t->nlines; k++)
            free(t->lines[k]);
        free(t->lines);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "useProcess=True"))
            g_mode = MODE_PROCESS;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    /* the emulator itself must never be stopped or backgrounded */
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    history_init(g_mode);
    history_load_file();
    bindings_init();
    bindings_load(config_path(), 0);

    gui_run();

    XFreeGC(g_dpy, g_gc);
    if (g_font) XFreeFont(g_dpy, g_font);
    if (g_cursor) XFreeCursor(g_dpy, g_cursor);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);

    return 0;
}
