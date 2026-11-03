/*
 * terminator.h -- shared internal interface for Terminator.
 *
 * Every module includes this.  It holds the tunables, the shared types, the
 * cross-module state (defined in state.c) and the internal API.  This is an
 * application-private header, not a public library interface.
 */
#ifndef TERMINATOR_H
#define TERMINATOR_H

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
#define TAB_WIDTH       170
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

/* Worker state is thread-local: one copy per tab shell.  In thread mode each
 * tab has its own worker thread; in process mode each tab is its own process,
 * so one copy per process is equally correct.  These are defined in state.c
 * and declared `extern __thread` in terminator.h. */


/* ---- shell variables and positional parameters (script interpreter) ---- */
#define MAX_VARS  256
#define MAX_POS   32

typedef struct {
    char name[64];
    char val[512];
} ShellVar;

/* ---- types -------------------------------------------------------- */
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
    char   prompt[PATH_MAX + 96];   /* user@terminator:<cwd>> */
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
    /* readline-style reverse-i-search (Ctrl+R) */
    int    searching;                 /* search mode is active          */
    int    search_len;                /* length of the search term      */
    int    search_pos;                /* history index we last matched  */
    int    search_failed;             /* no match for the current term  */
    char   search_buf[512];           /* the term typed so far          */
    char   search_orig[MAX_CMD];      /* the line as it was before Ctrl+R */
    char   search_match[MAX_CMD];     /* the command we are showing     */
} Tab;

typedef enum {
    ACT_NONE = 0,
    ACT_LEFT, ACT_RIGHT, ACT_BOL, ACT_EOL,
    ACT_HIST_PREV, ACT_HIST_NEXT,
    ACT_KILL_START, ACT_KILL_END, ACT_KILL_WORD,
    ACT_CLEAR, ACT_EXIT, ACT_INTERRUPT, ACT_COPY, ACT_PASTE,
    ACT_NEW_TAB, ACT_CLOSE_TAB, ACT_NEXT_TAB, ACT_PREV_TAB,
    ACT_PAGE_UP, ACT_PAGE_DOWN,
    ACT_SUSPEND, ACT_SEARCH
} Action;

typedef struct {
    unsigned int mods;
    KeySym       ks;
    Action       act;
} Binding;


/* ---- per-tab shared control block (mmap'd, GUI <-> worker) -------- */
typedef struct {
    volatile pid_t fg_pgid;     /* foreground job's process group        */
    volatile pid_t mw_pgid;     /* multiWatch / watch process group      */
    volatile int   mw_active;   /* a multiWatch loop is running          */
    volatile int   mw_cancel;   /* GUI asked multiWatch to stop          */
} Shared;

/* ---- shared state (defined in state.c) ---------------------------- */
extern HistStore   g_hist_local;              /* thread mode backing store        */
extern HistStore  *g_hist;                    /* shared by every worker           */
extern int         g_mode;
extern __thread int   g_out_fd;
extern __thread int   g_in_fd;
extern __thread int   g_ctl_fd;
extern __thread int   g_last_status;
extern __thread int   g_exit_requested;
extern __thread char  g_arena[ARENA_SZ];
extern __thread size_t g_arena_pos;
extern __thread char  g_cwd[PATH_MAX];        /* this tab's logical working directory */
extern pthread_mutex_t g_cwd_mtx;
extern __thread Job   g_jobs[MAX_JOBS];
extern __thread int   g_job_count;
extern __thread int   g_next_job_id;
extern __thread char  g_cur_cmd[JOB_CMD_LEN]; /* command line of the job being run */
extern __thread ShellVar g_vars[MAX_VARS];
extern __thread int      g_nvars;
extern __thread char    *g_pos[MAX_POS];      /* g_pos[0] is $1 */
extern __thread int      g_npos;
extern __thread char     g_script_name[PATH_MAX];
extern __thread int      g_script_depth;      /* >0 while interpreting a script */
extern __thread int      g_script_exit;       /* 'exit' asked the script to stop */
extern Display     *g_dpy;
extern int          g_scr;
extern Window       g_win;
extern GC           g_gc;
extern XFontStruct *g_font;
extern int          g_cw;                     /* character cell width  */
extern int          g_ch;                     /* character cell height */
extern Atom         g_wm_delete;
extern Atom         g_clipboard, g_utf8, g_targets;
extern Cursor       g_cursor;
extern int          g_running;
extern int g_win_w, g_win_h;
extern Tab  g_tabs[MAX_TABS];
extern int  g_tab_count;
extern int  g_active;
extern Binding g_user_binds[64];
extern int     g_n_user_binds;
extern int     g_loading_binds;               /* config-file load: duplicates are conflicts */
extern Binding g_def_binds[64];
extern int     g_n_def_binds;
extern char  g_sel_text[65536];
extern int   g_sel_len;

extern __thread Shared *g_sh_self;   /* this worker's control block */
extern Shared *g_sh[MAX_TABS];
extern char    g_tab_cwd[MAX_TABS][PATH_MAX];

/* ---- worker <-> GUI prompt markers ---- */
#define PMARK_START 0x01
#define PMARK_SEP   0x1f
#define PMARK_END   0x02

#define PROMPT_MAIN "het52@terminator> "
#define PROMPT_CONT "> "

/* ---- internal API ------------------------------------------------- */
/* small helpers shared by several modules */
int  str_cat2(char *out, size_t cap, const char *a, const char *b);
int  change_dir(const char *arg);
void var_set(const char *name, const char *val);
void gui_copy(Tab *t);
void gui_request_paste(int idx, Atom sel);

/* ------------------------------------------------------------------ */
/* prototypes                                                         */
/* ------------------------------------------------------------------ */

void  write_all(int fd, const void *buf, size_t n);
void  emit(const char *fmt, ...);
void  emit_raw(const char *s, size_t n);
char *xstrdup(const char *s);

void history_init(int mode);
void history_add(const char *cmd);
int  history_count(void);
int  history_get(int i, char *buf, size_t cap);
void history_load_file(void);
void history_clear_all(void);

int  lex(const char *src, Token *toks, int max, char *err, size_t errcap);
int  lex_core(const char *src, Token *toks, int max, char *err, size_t errcap, int reset);
int  command_incomplete(const char *s);
int  parse_stages(Token *toks, int nt, Stage *st, int *nst);
int  cmd_substitute(const char *cmd, char *out, size_t cap);
int  is_list_op(const Token *t);
int  is_redir_op(const Token *t);

void execute_line(const char *line);
void execute_tokens(Token *toks, int nt);
void run_one(Token *toks, int nt);
int  run_pipeline(Stage *st, int n);
void child_exec(Stage *st, int in_fd, int out_fd, int term_out, pid_t pgid,
                       const char *e_notfound, const char *e_noperm, const char *e_cannot);
int  open_redirections(Stage *st);
void expand_globs(Stage *st);
int  glob_in_cwd(const char *pat, glob_t *g);
int  wait_foreground(pid_t *pids, int n);
void reap_background_jobs(void);
int  run_capture(const char *cmd, char *out, size_t cap);
void run_command_text(const char *cmd);
int  try_assignment(const char *line);
void run_script_file(const char *path, int argc, char **argv, int sourced);
void resolve_path(const char *p, char *out, size_t cap);
int  looks_like_script(const char *path);
int  is_interp_name(const char *s);
int  run_test_builtin(char **args, int argc);
int  job_add(pid_t *pids, int n, pid_t pgid, const char *cmd, int state, int bg);
int  job_pick(const char *spec, int *idx_out);
const char *job_state_str(int st);

int  is_builtin(const char *name);
int  run_builtin_parent(char **args, int argc);
void do_multiwatch(const char *arg);
void do_watch(const char *arg);

void worker_loop(WorkerCtx *ctx);
void *worker_thread(void *p);
char *worker_read_command(void);
int  in_getc(void);

void bindings_init(void);
int  bindings_load(const char *path, int verbose);
int  bindings_add_user(const char *keyspec, const char *action, char *err, size_t cap);
Action bindings_lookup(unsigned int mods, KeySym ks);
int  bindings_save(const char *path);
const char *action_name(Action a);
Action action_from_name(const char *n);
int  parse_keyspec(const char *spec, unsigned int *mods, KeySym *ks);
const char *config_path(void);

void gui_run(void);
int  gui_new_tab(void);
void gui_close_tab(int idx);
void gui_draw(void);
void gui_handle_key(XKeyEvent *ke);
void gui_append_output(Tab *t, const char *data, int n, int idx);
void tab_append_line(Tab *t, const char *s);
void gui_suspend(Tab *t, int idx);
int  lcs_len(const char *a, const char *b);
void history_search(Tab *t, const char *term);


#endif /* TERMINATOR_H */
