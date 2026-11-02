/* main.c -- entry point */
#define _GNU_SOURCE
#include "terminator.h"

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

