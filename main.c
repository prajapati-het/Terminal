#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <err.h>
#include <ctype.h>
#include <pthread.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>

#define POSY 100
#define POSX 100
#define WIDTH 700
#define HEIGHT 500
#define BORDER 2
#define MAX_CMD 1024
#define MAX_ARGS 128
#define MAX_LINES 10000
#define MAX_LINE_LEN 2048
#define LINE_HEIGHT 20
#define CMD_HISTORY_SIZE 1000
#define CMD_SIZE 2048

char cmd_history[CMD_HISTORY_SIZE][CMD_SIZE];
int cmd_history_count = 0;
int last_status = 0;
int shell_running = 1;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

static Display *dpy;
static int scr;
static Window root;
static Atom wm_delete_window;
static int use_process_mode = 0;

static char history[MAX_LINES][MAX_LINE_LEN];
static int line_count = 0;
static int scroll_offset = 0;

static void add_line(const char *s)
{
    if (line_count == MAX_LINES) {
        memmove(history, history + 1, sizeof(history[0]) * (MAX_LINES - 1));
        line_count--;
    }
    snprintf(history[line_count], MAX_LINE_LEN, "%s", s);
    line_count++;
}

static void save_command(const char *cmd)
{
    if (cmd_history_count < CMD_HISTORY_SIZE) {
        snprintf(cmd_history[cmd_history_count++], CMD_SIZE, "%s", cmd);
    } else {
        memmove(cmd_history, cmd_history + 1,
                (CMD_HISTORY_SIZE - 1) * sizeof(cmd_history[0]));
        snprintf(cmd_history[CMD_HISTORY_SIZE - 1], CMD_SIZE, "%s", cmd);
    }
}

static Window create_win(int x, int y, int w, int h, int b)
{
    Window win = XCreateSimpleWindow(dpy, root, x, y, w, h, b,
                                     WhitePixel(dpy, scr), BlackPixel(dpy, scr));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask |
                 ButtonPressMask | StructureNotifyMask);
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    XStoreName(dpy, win, use_process_mode ? "Terminator (process mode)" : "Terminator (thread mode)");
    return win;
}

static int run_builtin(char **args, int argc)
{
    if (argc == 0) {
        last_status = 0;
        return 1;
    }

    if (strcmp(args[0], "cd") == 0) {
        const char *path = argc < 2 ? getenv("HOME") : args[1];
        char expanded[CMD_SIZE];

        if (path == NULL) {
            add_line("terminator: cd: HOME not set");
            last_status = 1;
            return 1;
        }

        if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
            const char *home = getenv("HOME");
            if (home == NULL) {
                add_line("terminator: cd: HOME not set");
                last_status = 1;
                return 1;
            }
            snprintf(expanded, sizeof(expanded), "%s%s", home, path + 1);
            path = expanded;
        }

        if (chdir(path) == -1) {
            char msg[CMD_SIZE];
            snprintf(msg, sizeof(msg), "terminator: cd: %s: %s",
                     path, strerror(errno));
            add_line(msg);
            last_status = 1;
        } else {
            last_status = 0;
        }
        return 1;
    }

    if (strcmp(args[0], "exit") == 0) {
        if (argc > 1) {
            char *end;
            long value = strtol(args[1], &end, 10);
            if (*end == '\0')
                last_status = (unsigned char)value;
            else
                last_status = 2;
        }
        shell_running = 0;
        return 1;
    }

    if (strcmp(args[0], "history") == 0) {
        for (int i = 0; i < cmd_history_count; i++) {
            char msg[CMD_SIZE + 32];
            snprintf(msg, sizeof(msg), "%5d  %s", i + 1, cmd_history[i]);
            add_line(msg);
        }
        last_status = 0;
        return 1;
    }

    return 0;
}

static int quotes_balanced(const char *cmd)
{
    char quote = '\0';
    char outer_quote[128];
    int subst_depth = 0;
    int backtick_depth = 0;
    int escaped = 0;
    int stack_top = 0;
    size_t len = strlen(cmd);

    if (len > 0 && cmd[len - 1] == '\\') {
        size_t slashes = 0;
        while (len > slashes && cmd[len - 1 - slashes] == '\\')
            slashes++;
        if (slashes % 2 == 1)
            return 1;
    }

    for (size_t i = 0; cmd[i]; i++) {
        char c = cmd[i];

        if (escaped) {
            escaped = 0;
            continue;
        }

        if (c == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }

        if (quote == '\'') {
            if (c == '\'')
                quote = '\0';
            continue;
        }

        if (quote == '"') {
            if (c == '"') {
                quote = '\0';
                continue;
            }

            if (c == '`') {
                if (backtick_depth == 0 && stack_top < 128) {
                    outer_quote[stack_top++] = quote;
                    backtick_depth++;
                    quote = '\0';
                } else if (backtick_depth > 0) {
                    backtick_depth--;
                    quote = stack_top > 0
                        ? outer_quote[--stack_top] : '"';
                }
                continue;
            }

            if (c == '$' && cmd[i + 1] == '(') {
                if (stack_top < 128) {
                    outer_quote[stack_top++] = quote;
                    subst_depth++;
                    quote = '\0';
                    i++;
                }
                continue;
            }

            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }

        if (c == '`') {
            if (backtick_depth == 0 && stack_top < 128) {
                outer_quote[stack_top++] = quote;
                backtick_depth++;
            } else if (backtick_depth > 0) {
                backtick_depth--;
                quote = stack_top > 0
                    ? outer_quote[--stack_top] : '\0';
            }
            continue;
        }

        if (c == '$' && cmd[i + 1] == '(') {
            if (stack_top < 128) {
                outer_quote[stack_top++] = quote;
                subst_depth++;
                quote = '\0';
                i++;
            }
            continue;
        }

        if (subst_depth > 0 && c == '(') {
            subst_depth++;
            continue;
        }

        if (subst_depth > 0 && c == ')') {
            subst_depth--;
            if (stack_top > 0)
                quote = outer_quote[--stack_top];
        }
    }

    return quote != '\0' || subst_depth > 0 ||
           backtick_depth > 0 || escaped;
}

static int tokenize_command(char *cmd, char *args[MAX_ARGS])
{
    char *r = cmd;
    char *w = cmd;
    char quote;
    int argc = 0;

    while (*r) {
        while (isspace((unsigned char)*r))
            r++;
        if (!*r)
            break;
        if (argc >= MAX_ARGS - 1)
            return -1;

        args[argc++] = w;
        quote = '\0';

        while (*r) {
            if (!quote && isspace((unsigned char)*r))
                break;
            if (!quote && (*r == '\'' || *r == '"')) {
                quote = *r++;
                continue;
            }
            if (quote && *r == quote) {
                quote = '\0';
                r++;
                continue;
            }
            if (*r == '\\' && quote != '\'' && r[1]) {
                r++;
                *w++ = *r++;
                continue;
            }
            *w++ = *r++;
        }

        if (*r && isspace((unsigned char)*r))
            r++;
        *w++ = '\0';
    }
    args[argc] = NULL;
    return argc;
}

static void expand_status(char *cmd, size_t cap)
{
    char out[MAX_CMD];
    size_t i = 0, j = 0;
    char status_text[32];

    snprintf(status_text, sizeof(status_text), "%d", last_status);
    while (cmd[i] && j + 1 < sizeof(out)) {
        if (cmd[i] == '$' && cmd[i + 1] == '?') {
            size_t n = strlen(status_text);
            if (j + n >= sizeof(out))
                break;
            memcpy(out + j, status_text, n);
            j += n;
            i += 2;
        } else {
            out[j++] = cmd[i++];
        }
    }
    out[j] = '\0';
    snprintf(cmd, cap, "%s", out);
}

static int check_input_file(const char *cmd)
{
    char quote = '\0';
    int escaped = 0;

    for (size_t i = 0; cmd[i]; i++) {
        char c = cmd[i];

        if (escaped) {
            escaped = 0;
            continue;
        }

        if (c == '\\' && quote != '\'') {
            escaped = 1;
            continue;
        }

        if (quote) {
            if (c == quote)
                quote = '\0';
            continue;
        }

        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }

        if (c == '<' && cmd[i + 1] != '<' &&
            (i == 0 || cmd[i - 1] != '<')) {
            size_t j = i + 1;

            while (cmd[j] && isspace((unsigned char)cmd[j]))
                j++;

            if (cmd[j] == '\0') {
                add_line("terminator: missing input filename");
                last_status = 2;
                return 0;
            }

            if (cmd[j] == '\'' || cmd[j] == '"') {
                char q = cmd[j++];
                size_t start = j;

                while (cmd[j] && cmd[j] != q)
                    j++;

                if (cmd[j] == '\0') {
                    add_line("terminator: invalid input filename");
                    last_status = 2;
                    return 0;
                }

                char filename[CMD_SIZE];
                size_t n = j - start;

                if (n >= sizeof(filename))
                    n = sizeof(filename) - 1;

                memcpy(filename, cmd + start, n);
                filename[n] = '\0';

                if (access(filename, F_OK) == -1 ||
                    access(filename, R_OK) == -1) {
                    char msg[CMD_SIZE];
                    snprintf(msg, sizeof(msg),
                             "terminator: %.1000s: %.1000s",
                             filename, strerror(errno));
                    add_line(msg);
                    last_status = 1;
                    return 0;
                }
            } else {
                size_t start = j;

                while (cmd[j] &&
                       !isspace((unsigned char)cmd[j]) &&
                       cmd[j] != '|' && cmd[j] != '&' &&
                       cmd[j] != '<' && cmd[j] != '>')
                    j++;

                if (j == start) {
                    add_line("terminator: missing input filename");
                    last_status = 2;
                    return 0;
                }

                char filename[CMD_SIZE];
                size_t n = j - start;

                if (n >= sizeof(filename))
                    n = sizeof(filename) - 1;

                memcpy(filename, cmd + start, n);
                filename[n] = '\0';

                if (access(filename, F_OK) == -1 ||
                    access(filename, R_OK) == -1) {
                    char msg[CMD_SIZE];
                    snprintf(msg, sizeof(msg),
                             "terminator: %.1000s: %.1000s",
                             filename, strerror(errno));
                    add_line(msg);
                    last_status = 1;
                    return 0;
                }
            }
        }
    }

    return 1;
}

static void execute_command(char *cmd)
{
    int fd[2], status;
    pid_t pid;
    char *args[MAX_ARGS];
    char buffer[256], output[MAX_LINE_LEN];
    char shell_cmd[MAX_CMD];
    int argc;
    size_t out_len = 0;
    FILE *fp;

    if (cmd[0] == '\0')
        return;

    save_command(cmd);

    if (strlen(cmd) >= MAX_CMD) {
        add_line("terminator: command too long");
        last_status = 2;
        return;
    }

    expand_status(cmd, MAX_CMD);
    snprintf(shell_cmd, sizeof(shell_cmd), "%s", cmd);
    argc = tokenize_command(cmd, args);

    if (argc < 0) {
        add_line("terminator: too many arguments");
        last_status = 2;
        return;
    }

    if (argc == 0) {
        last_status = 0;
        return;
    }

    if (run_builtin(args, argc))
        return;
        
    if (!check_input_file(shell_cmd))
    	return;

    /*
     * Let /bin/bash parse compound shell syntax (pipelines, redirections,
     * &&, ||, globbing, variables and scripts). The outer shell still owns
     * the X11 UI and captures stdout/stderr for display in its output area.
     */
    if (pipe(fd) == -1) {
        add_line("terminator: pipe failed");
        last_status = 1;
        return;
    }

    pid = fork();
    if (pid == -1) {
        close(fd[0]);
        close(fd[1]);
        add_line("terminator: fork failed");
        last_status = 1;
        return;
    }

    if (pid == 0) {
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) == -1 ||
            dup2(fd[1], STDERR_FILENO) == -1)
            _exit(126);
        close(fd[1]);
        execlp("/bin/bash", "bash", "-c", shell_cmd, (char *)NULL);
        dprintf(STDERR_FILENO, "terminator: bash: %s\n", strerror(errno));
        _exit(127);
    }

    close(fd[1]);
    fp = fdopen(fd[0], "r");
    if (fp == NULL) {
        close(fd[0]);
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        add_line("terminator: fdopen failed");
        last_status = 1;
        return;
    }

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t i;
        for (i = 0; buffer[i] != '\0'; i++) {
            if (buffer[i] == '\r') continue;
            if (buffer[i] == '\n') {
                output[out_len] = '\0';
                add_line(output);
                out_len = 0;
            } else {
                output[out_len++] = buffer[i];
                if (out_len == MAX_LINE_LEN - 1) {
                    output[out_len] = '\0';
                    add_line(output);
                    out_len = 0;
                }
            }
        }
    }
    if (out_len > 0) {
        output[out_len] = '\0';
        add_line(output);
    }
    fclose(fp);

    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            last_status = 1;
            return;
        }
    }
    if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
    else last_status = 1;
}

static void draw_terminal(Window win, GC gc, const char *cmd, int len)
{
    XWindowAttributes attr;
    int visible, cmd_lines = 1, total, max_scroll, start, i, pos;

    if (!XGetWindowAttributes(dpy, win, &attr))
        return;

    visible = (attr.height - 20) / LINE_HEIGHT;
    if (visible < 1)
        visible = 1;

    for (i = 0; i < len; i++)
        if (cmd[i] == '\n')
            cmd_lines++;

    total = line_count + cmd_lines;
    max_scroll = total > visible ? total - visible : 0;
    if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;
    if (scroll_offset < 0)
        scroll_offset = 0;

    start = total - visible - scroll_offset;
    if (start < 0)
        start = 0;

    XClearWindow(dpy, win);

    for (i = start; i < total && i < start + visible; i++) {
        int y = 20 + (i - start) * LINE_HEIGHT;

        if (i < line_count) {
            XDrawString(dpy, win, gc, 10, y, history[i], (int)strlen(history[i]));
        } else {
            int line_no = i - line_count;
            int begin, end, j;
            pos = 0;
            for (j = 0; j < line_no; j++) {
                while (pos < len && cmd[pos] != '\n')
                    pos++;
                if (pos < len)
                    pos++;
            }
            begin = pos;
            end = begin;
            while (end < len && cmd[end] != '\n')
                end++;

            XDrawString(dpy, win, gc, 10, y, line_no == 0 ? "$ " : "> ", 2);
            if (end > begin)
                XDrawString(dpy, win, gc, 25, y, cmd + begin, end - begin);
        }
    }
    XFlush(dpy);
}

static void run(Window win)
{
    XEvent ev;
    XGCValues values;
    GC gc;
    char cmd[MAX_CMD];
    int len = 0;
    int running = 1;

    cmd[0] = '\0';
    values.foreground = WhitePixel(dpy, scr);
    values.background = BlackPixel(dpy, scr);
    gc = XCreateGC(dpy, win, GCForeground | GCBackground, &values);
    XMapWindow(dpy, win);

    while (running && shell_running) {
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                draw_terminal(win, gc, cmd, len);
            break;

        case ConfigureNotify:
            draw_terminal(win, gc, cmd, len);
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete_window) {
                running = 0;
                shell_running = 0;
            }
            break;

        case ButtonPress:
            if (ev.xbutton.button == Button4)
                scroll_offset += 3;
            else if (ev.xbutton.button == Button5) {
                scroll_offset -= 3;
                if (scroll_offset < 0)
                    scroll_offset = 0;
            }
            draw_terminal(win, gc, cmd, len);
            break;

        case KeyPress: {
            char text[32];
            KeySym key;
            int n = XLookupString(&ev.xkey, text, sizeof(text), &key, NULL);

            if (key == XK_Return || key == XK_KP_Enter) {
                if (ev.xkey.state & ShiftMask) {
			if (len < MAX_CMD - 1) {
			    cmd[len++] = '\n';
			    cmd[len] = '\0';
			}
		    } else if (quotes_balanced(cmd)) {
			if (len < MAX_CMD - 1) {
			    cmd[len++] = '\n';
			    cmd[len] = '\0';
			}
		    } else {
                    char command[MAX_CMD];
                    char prompt[MAX_LINE_LEN];
                    char *line, *next;

                    cmd[len] = '\0';
                    snprintf(command, sizeof(command), "%s", cmd);

                    line = cmd;
                    do {
                        next = strchr(line, '\n');
                        if (next)
                            *next = '\0';
                        snprintf(prompt, sizeof(prompt), "%s%s",
                                 line == cmd ? "$ " : "> ", line);
                        add_line(prompt);
                        if (!next)
                            break;
                        line = next + 1;
                    } while (1);

                    if (len > 0)
                        execute_command(command);

                    len = 0;
                    cmd[0] = '\0';
                    scroll_offset = 0;
                }
            } else if (key == XK_BackSpace || key == XK_Delete) {
                if (len > 0) {
                    len--;
                    cmd[len] = '\0';
                }
                scroll_offset = 0;
            } else if (n > 0 && text[0] >= 32 && text[0] <= 126) {
                if (len < MAX_CMD - 1) {
                    cmd[len++] = text[0];
                    cmd[len] = '\0';
                }
                scroll_offset = 0;
            }

            draw_terminal(win, gc, cmd, len);
            break;
        }
        }
    }

    XFreeGC(dpy, gc);
}

int main(int argc, char **argv)
{
    Window win;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0) use_process_mode = 1;
        else if (strcmp(argv[i], "useProcess=False") == 0) use_process_mode = 0;
        else { fprintf(stderr, "Usage: %s [useProcess=True|useProcess=False]\n", argv[0]); return 2; }
    }

    if ((dpy = XOpenDisplay(NULL)) == NULL)
        err(1, "Can't open display");

    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    win = create_win(POSX, POSY, WIDTH, HEIGHT, BORDER);

    run(win);

    XUnmapWindow(dpy, win);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return last_status;
}