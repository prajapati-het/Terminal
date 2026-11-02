/* state.c -- definitions of the shared state declared in terminator.h. */
#define _GNU_SOURCE
#include "terminator.h"

HistStore   g_hist_local;      /* thread mode backing store        */
HistStore  *g_hist = NULL;     /* shared by every worker           */
int         g_mode = MODE_THREAD;
__thread int   g_out_fd = -1;
__thread int   g_in_fd  = -1;
__thread int   g_ctl_fd = -1;
__thread int   g_last_status = 0;
__thread int   g_exit_requested = 0;
__thread char  g_arena[ARENA_SZ];
__thread size_t g_arena_pos = 0;
__thread char  g_cwd[PATH_MAX];      /* this tab's logical working directory */
pthread_mutex_t g_cwd_mtx = PTHREAD_MUTEX_INITIALIZER;
__thread Job   g_jobs[MAX_JOBS];
__thread int   g_job_count = 0;
__thread int   g_next_job_id = 1;
__thread char  g_cur_cmd[JOB_CMD_LEN];  /* command line of the job being run */
__thread ShellVar g_vars[MAX_VARS];
__thread int      g_nvars = 0;
__thread char    *g_pos[MAX_POS];        /* g_pos[0] is $1 */
__thread int      g_npos = 0;
__thread char     g_script_name[PATH_MAX] = "terminator";
__thread int      g_script_depth = 0;    /* >0 while interpreting a script */
__thread int      g_script_exit = 0;     /* 'exit' asked the script to stop */
Display     *g_dpy = NULL;
int          g_scr;
Window       g_win;
GC           g_gc;
XFontStruct *g_font = NULL;
int          g_cw = 8;          /* character cell width  */
int          g_ch = LINE_HEIGHT;/* character cell height */
Atom         g_wm_delete;
Atom         g_clipboard, g_utf8, g_targets;
Cursor       g_cursor;
int          g_running = 1;
int          g_win_w = WIN_W, g_win_h = WIN_H;
Tab  g_tabs[MAX_TABS];
int  g_tab_count = 0;
int  g_active = 0;
Binding g_user_binds[64];
int     g_n_user_binds = 0;
int     g_loading_binds = 0;   /* config-file load: duplicates are conflicts */
Binding g_def_binds[64];
int     g_n_def_binds = 0;
char  g_sel_text[65536];
int   g_sel_len = 0;

Shared *g_sh[MAX_TABS];
char    g_tab_cwd[MAX_TABS][PATH_MAX];
