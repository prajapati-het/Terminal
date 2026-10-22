#define _POSIX_C_SOURCE 200810L
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
#include <fcntl.h>
#include <glob.h>

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
#define MAX_STAGES 128
#define MAX_EXPANDED_CMD (MAX_CMD * 4)

typedef struct {
    char *args[MAX_ARGS];
    int argc;
    char *input;
    char *output;
    char *error;
    int append;
    int background;
} Stage;

static char cmd_history[CMD_HISTORY_SIZE][CMD_SIZE];
static int cmd_history_count;
static int last_status;
static int shell_running = 1;

static Display *dpy;
static int scr;
static Window root;
static Atom wm_delete_window;

static char expanded_cmd[MAX_EXPANDED_CMD];
static char history[MAX_LINES][MAX_LINE_LEN];
static char token_storage[MAX_ARGS * 2][MAX_CMD];
static int line_count;
static int scroll_offset;

static void add_line(const char *s)
{
    if (line_count == MAX_LINES) {
        memmove(history, history + 1,
                sizeof(history[0]) * (MAX_LINES - 1));
        line_count--;
    }

    snprintf(history[line_count++], MAX_LINE_LEN, "%s", s);
}

static void save_command(const char *cmd)
{
    if (cmd_history_count == CMD_HISTORY_SIZE) {
        memmove(cmd_history, cmd_history + 1,
                sizeof(cmd_history[0]) * (CMD_HISTORY_SIZE - 1));
        cmd_history_count--;
    }

    snprintf(cmd_history[cmd_history_count++], CMD_SIZE, "%s", cmd);
}

static void append_char(char **w, char c)
{
    if (*w < expanded_cmd + sizeof(expanded_cmd) - 1)
        *(*w)++ = c;
}

static void append_text(char **w, const char *s)
{
    if (!s)
        return;

    while (*s)
        append_char(w, *s++);
}

static Window create_win(int x, int y, int w, int h, int b)
{
    Window win = XCreateSimpleWindow(
        dpy, root, x, y, w, h, b,
        WhitePixel(dpy, scr), BlackPixel(dpy, scr));

    XSelectInput(dpy, win, ExposureMask | KeyPressMask |
                 ButtonPressMask | StructureNotifyMask);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    XStoreName(dpy, win, "Terminator");

    return win;
}

static int run_builtin(char **args, int argc)
{
    if (argc == 0)
        return 1;

    if (!strcmp(args[0], "cd")) {
        const char *path = argc > 1 ? args[1] : getenv("HOME");

        if (!path) {
            add_line("terminator: cd: HOME not set");
            last_status = 1;
        } else if (chdir(path) == -1) {
            char msg[CMD_SIZE];
            snprintf(msg, sizeof(msg), "cd: %s", strerror(errno));
            add_line(msg);
            last_status = 1;
        } else {
            last_status = 0;
        }

        return 1;
    }

    if (!strcmp(args[0], "exit")) {
        if (argc > 1)
            last_status = atoi(args[1]);
        shell_running = 0;
        return 1;
    }

    if (!strcmp(args[0], "history")) {
        for (int i = 0; i < cmd_history_count; i++) {
            char msg[CMD_SIZE + 32];
            snprintf(msg, sizeof(msg), "%5d  %s",
                     i + 1, cmd_history[i]);
            add_line(msg);
        }

        last_status = 0;
        return 1;
    }

    return 0;
}

/*
 * Tokenizer:
 * - Handles single and double quotes.
 * - Handles backslash escapes outside single quotes.
 * - Recognizes |, <, >, >>, 2>, 2>> and &.
 * - Operators are returned as separate tokens.
 */
static int tokenize(char *src, char *tokens[], int max_tokens)
{
    char *r = src;
    char *w = expanded_cmd;
    int count = 0;

    while (*r) {
        while (isspace((unsigned char)*r))
            r++;

        if (!*r)
            break;

        if (count >= max_tokens - 1)
            return -1;

        if (*r == '2' && r[1] == '>') {
            tokens[count++] = r[2] == '>' ? "2>>" : "2>";
            r += r[2] == '>' ? 3 : 2;
            continue;
        }

        if (*r == '|') {
            tokens[count++] = "|";
            r++;
            continue;
        }

        if (*r == '<') {
            tokens[count++] = "<";
            r++;
            continue;
        }

        if (*r == '>') {
            tokens[count++] = r[1] == '>' ? ">>" : ">";
            r += r[1] == '>' ? 2 : 1;
            continue;
        }

        if (*r == '&') {
            tokens[count++] = "&";
            r++;
            continue;
        }

        tokens[count++] = w;
        char quote = 0;

        while (*r) {
            if (!quote && (isspace((unsigned char)*r) ||
                *r == '|' || *r == '<' || *r == '>' ||
                *r == '&'))
                break;

            if (!quote && (*r == '\'' || *r == '"')) {
                quote = *r++;
                continue;
            }

            if (quote && *r == quote) {
                quote = 0;
                r++;
                continue;
            }

            if (*r == '\\' && quote != '\'' && r[1]) {
                r++;
                append_char(&w, *r++);
                continue;
            }

            if (*r == '$' && quote != '\'') {
                r++;

                if (*r == '?') {
                    char value[32];
                    snprintf(value, sizeof(value), "%d", last_status);
                    append_text(&w, value);
                    r++;
                    continue;
                }

                char name[256];
                int j = 0;

                if (*r == '{') {
                    r++;

                    while (*r && *r != '}' &&
                           j < (int)sizeof(name) - 1)
                        name[j++] = *r++;

                    if (*r == '}')
                        r++;
                    else {
                        append_char(&w, '$');
                        append_char(&w, '{');
                        name[j] = '\0';
                        append_text(&w, name);
                        continue;
                    }
                } else {
                    while ((*r == '_' ||
                           (*r >= 'a' && *r <= 'z') ||
                           (*r >= 'A' && *r <= 'Z') ||
                           (*r >= '0' && *r <= '9')) &&
                           j < (int)sizeof(name) - 1)
                        name[j++] = *r++;
                }

                if (j == 0) {
                    append_char(&w, '$');
                    continue;
                }

                name[j] = '\0';
                append_text(&w, getenv(name));
                continue;
            }

            append_char(&w, *r++);
        }

        if (quote) {
            add_line("terminator: unmatched quote");
            return -1;
        }

        append_char(&w, '\0');
    }

    tokens[count] = NULL;
    return count;
}


static int is_operator(const char *s)
{
    return !strcmp(s, "|") || !strcmp(s, "<") ||
           !strcmp(s, ">") || !strcmp(s, ">>") ||
           !strcmp(s, "2>") || !strcmp(s, "2>>") ||
           !strcmp(s, "&");
}

static int parse_pipeline(char *tokens[], int nt,
                          Stage stages[], int *nstages)
{
    int s = 0;

    memset(stages, 0, sizeof(Stage) * MAX_STAGES);
    *nstages = 1;

    for (int i = 0; i < nt; i++) {
        Stage *st = &stages[s];

        if (!strcmp(tokens[i], "|")) {
            if (st->argc == 0 || s + 1 >= MAX_STAGES) {
                add_line("terminator: invalid pipeline");
                return 0;
            }

            s++;
            (*nstages)++;
            continue;
        }

        if (!strcmp(tokens[i], "&")) {
            if (i != nt - 1) {
                add_line("terminator: unexpected &");
                return 0;
            }

            st->background = 1;
            continue;
        }

        if (!strcmp(tokens[i], "<") ||
            !strcmp(tokens[i], ">") ||
            !strcmp(tokens[i], ">>") ||
            !strcmp(tokens[i], "2>") ||
            !strcmp(tokens[i], "2>>")) {

            char *op = tokens[i];

            if (++i >= nt || is_operator(tokens[i])) {
                add_line("terminator: missing redirection filename");
                return 0;
            }

            if (!strcmp(op, "<"))
                st->input = tokens[i];
            else if (!strcmp(op, ">") || !strcmp(op, ">>")) {
                st->output = tokens[i];
                st->append = !strcmp(op, ">>");
            } else {
                st->error = tokens[i];

                if (!strcmp(op, "2>>"))
                    st->append = 1;
            }

            continue;
        }

        if (st->argc >= MAX_ARGS - 1) {
            add_line("terminator: too many arguments");
            return 0;
        }

        st->args[st->argc++] = tokens[i];
    }

    for (int i = 0; i < *nstages; i++) {
        if (stages[i].argc == 0) {
            add_line("terminator: empty pipeline stage");
            return 0;
        }

        stages[i].args[stages[i].argc] = NULL;
    }

    return 1;
}

/* Expand wildcard arguments without using a shell. */
static void expand_globs(Stage *st)
{
    char *expanded[MAX_ARGS];
    int n = 0;

    for (int i = 0; i < st->argc; i++) {
        glob_t g;
        memset(&g, 0, sizeof(g));

        int rc = glob(st->args[i], 0, NULL, &g);

        if (rc == 0) {
            for (size_t j = 0; j < g.gl_pathc &&
                 n < MAX_ARGS - 1; j++)
                expanded[n++] = strdup(g.gl_pathv[j]);
        } else {
            if (n < MAX_ARGS - 1)
                expanded[n++] = strdup(st->args[i]);
        }

        globfree(&g);
    }

    for (int i = 0; i < n; i++)
        st->args[i] = expanded[i];

    st->argc = n;
    st->args[n] = NULL;
}

static int open_redirections(Stage *st)
{
    int fd;

    if (st->input) {
        fd = open(st->input, O_RDONLY);

        if (fd == -1) {
            dprintf(STDERR_FILENO, "%s: %s\n",
                    st->input, strerror(errno));
            return 0;
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            close(fd);
            return 0;
        }

        close(fd);
    }

    if (st->output) {
        int flags = O_WRONLY | O_CREAT |
                    (st->append ? O_APPEND : O_TRUNC);

        fd = open(st->output, flags, 0666);

        if (fd == -1) {
            dprintf(STDERR_FILENO, "%s: %s\n",
                    st->output, strerror(errno));
            return 0;
        }

        if (dup2(fd, STDOUT_FILENO) == -1) {
            close(fd);
            return 0;
        }

        close(fd);
    }

    if (st->error) {
        int flags = O_WRONLY | O_CREAT |
                    (st->append ? O_APPEND : O_TRUNC);

        fd = open(st->error, flags, 0666);

        if (fd == -1) {
            dprintf(STDERR_FILENO, "%s: %s\n",
                    st->error, strerror(errno));
            return 0;
        }

        if (dup2(fd, STDERR_FILENO) == -1) {
            close(fd);
            return 0;
        }

        close(fd);
    }

    return 1;
}


static void child_exec(Stage *st, int input_fd, int output_fd,
                       int capture_fd, int pipes[][2], int npipes)
{
    if (input_fd != -1 && dup2(input_fd, STDIN_FILENO) == -1)
        _exit(126);

    if (output_fd != -1) {
        if (dup2(output_fd, STDOUT_FILENO) == -1)
            _exit(126);
    } else if (capture_fd != -1) {
        if (dup2(capture_fd, STDOUT_FILENO) == -1)
            _exit(126);
    }

if (capture_fd != -1 &&
    dup2(capture_fd, STDERR_FILENO) == -1)
    _exit(126);

    for (int i = 0; i < npipes; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (input_fd != -1 && input_fd != STDIN_FILENO)
        close(input_fd);

    if (output_fd != -1 && output_fd != STDOUT_FILENO)
        close(output_fd);

    if (capture_fd != -1 &&
        capture_fd != STDOUT_FILENO &&
        capture_fd != STDERR_FILENO)
        close(capture_fd);

    if (!open_redirections(st))
        _exit(1);

    if (!strcmp(st->args[0], "cd")) {
        const char *path = st->argc > 1
                         ? st->args[1] : getenv("HOME");

        if (!path || chdir(path) == -1) {
            dprintf(STDERR_FILENO, "cd: %s\n", strerror(errno));
            _exit(1);
        }

        _exit(0);
    }

    if (!strcmp(st->args[0], "history")) {
        for (int i = 0; i < cmd_history_count; i++)
            dprintf(STDOUT_FILENO, "%5d  %s\n",
                    i + 1, cmd_history[i]);

        _exit(0);
    }

    execvp(st->args[0], st->args);

    dprintf(STDERR_FILENO, "%s: %s\n",
            st->args[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}


static void execute_pipeline(Stage stages[], int n)
{
    int pipes[MAX_STAGES - 1][2];
    pid_t pids[MAX_STAGES];
    int capture[2];
    int npipes = n - 1;
    int background = stages[n - 1].background;
    int status = 0;

    /*
     * A single command with no redirections can run a stateful
     * built-in directly in the parent shell.
     */
    if (n == 1 && !stages[0].input &&
        !stages[0].output && !stages[0].error &&
        !background && run_builtin(stages[0].args,
                                   stages[0].argc))
        return;

    for (int i = 0; i < n; i++)
        expand_globs(&stages[i]);

    for (int i = 0; i < npipes; i++) {
        if (pipe(pipes[i]) == -1) {
            add_line("terminator: pipe failed");
            last_status = 1;

            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            return;
        }
    }

    if (pipe(capture) == -1) {
        add_line("terminator: output capture pipe failed");

        for (int i = 0; i < npipes; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }

        last_status = 1;
        return;
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();

        if (pid == -1) {
            add_line("terminator: fork failed");
            last_status = 1;

            for (int j = 0; j < npipes; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            close(capture[0]);
            close(capture[1]);

            for (int j = 0; j < i; j++)
                kill(pids[j], SIGTERM);

            for (int j = 0; j < i; j++)
                while (waitpid(pids[j], NULL, 0) == -1 &&
                       errno == EINTR) {}

            return;
        }

        
	if (pid == 0) {
	    int in = i > 0 ? pipes[i - 1][0] : -1;
	    int out = i < n - 1 ? pipes[i][1] : -1;
	    int cap = i == n - 1 ? capture[1] : -1;

	    close(capture[0]);

	    if (cap == -1)
		close(capture[1]);

	    child_exec(&stages[i], in, out, cap, pipes, npipes);
	}

    }

    for (int i = 0; i < npipes; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    close(capture[1]);

    if (background) {
        close(capture[0]);

        char msg[128];
        snprintf(msg, sizeof(msg), "[background] PID %ld",
                 (long)pids[n - 1]);
        add_line(msg);

        last_status = 0;
        return;
    }

    /*
     * Read output before waiting: otherwise a child producing
     * lots of output could block on a full capture pipe.
     */
    FILE *fp = fdopen(capture[0], "r");

    if (fp) {
        char buf[MAX_LINE_LEN];
        char out[MAX_LINE_LEN];
        size_t len = 0;

        while (fgets(buf, sizeof(buf), fp)) {
            for (size_t i = 0; buf[i]; i++) {
                if (buf[i] == '\r')
                    continue;

                if (buf[i] == '\n' || len == MAX_LINE_LEN - 1) {
                    out[len] = '\0';
                    add_line(out);
                    len = 0;

                    if (buf[i] == '\n')
                        continue;
                }

                if (len < MAX_LINE_LEN - 1)
                    out[len++] = buf[i];
            }
        }

        if (len) {
            out[len] = '\0';
            add_line(out);
        }

        fclose(fp);
    } else {
        close(capture[0]);
    }

    for (int i = 0; i < n; i++) {
        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno == EINTR)
                continue;

            status = -1;
            break;
        }

        if (i == n - 1) {
            if (status != -1 && WIFEXITED(status))
                last_status = WEXITSTATUS(status);
            else if (status != -1 && WIFSIGNALED(status))
                last_status = 128 + WTERMSIG(status);
            else
                last_status = 1;
        }
    }
}

static void execute_command(char *cmd)
{
    char copy[MAX_CMD];
    char *tokens[MAX_ARGS * 2];
    Stage stages[MAX_STAGES];
    int nt, nstages;

    if (!cmd || !*cmd)
        return;

    if (strlen(cmd) >= MAX_CMD) {
        add_line("terminator: command too long");
        last_status = 2;
        return;
    }

    save_command(cmd);
    snprintf(copy, sizeof(copy), "%s", cmd);

    nt = tokenize(copy, tokens, MAX_ARGS * 2);

    if (nt < 0) {
        last_status = 2;
        return;
    }

    if (nt == 0) {
        last_status = 0;
        return;
    }

    if (!parse_pipeline(tokens, nt, stages, &nstages)) {
        last_status = 2;
        return;
    }

    execute_pipeline(stages, nstages);
}

static int quotes_balanced(const char *cmd)
{
    char quote = 0;
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
                quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        }
    }

    return quote != 0 || escaped;
}

static void draw_terminal(Window win, GC gc,
                          const char *cmd, int len)
{
    XWindowAttributes attr;

    if (!XGetWindowAttributes(dpy, win, &attr))
        return;

    int visible = (attr.height - 20) / LINE_HEIGHT;
    if (visible < 1)
        visible = 1;

    int cmd_lines = 1;

    for (int i = 0; i < len; i++)
        if (cmd[i] == '\n')
            cmd_lines++;

    int total = line_count + cmd_lines;
    int max_scroll = total > visible ? total - visible : 0;

    if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;
    if (scroll_offset < 0)
        scroll_offset = 0;

    int start = total - visible - scroll_offset;
    if (start < 0)
        start = 0;

    XClearWindow(dpy, win);

    for (int i = start; i < total && i < start + visible; i++) {
        int y = 20 + (i - start) * LINE_HEIGHT;

        if (i < line_count) {
            XDrawString(dpy, win, gc, 10, y,
                        history[i], (int)strlen(history[i]));
        } else {
            int line_no = i - line_count;
            int pos = 0;

            for (int j = 0; j < line_no; j++) {
                while (pos < len && cmd[pos] != '\n')
                    pos++;
                if (pos < len)
                    pos++;
            }

            int begin = pos;

            while (pos < len && cmd[pos] != '\n')
                pos++;

            XDrawString(dpy, win, gc, 10, y,
                        line_no == 0 ? "$ " : "> ", 2);

            if (pos > begin)
                XDrawString(dpy, win, gc, 25, y,
                            cmd + begin, pos - begin);
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
    
    XFlush(dpy);

    XEvent event;
    do {
        XNextEvent(dpy, &event);
    } while (event.type != MapNotify);

    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    XFlush(dpy);

    while (running && shell_running) {
        XNextEvent(dpy, &ev);

        switch (ev.type) {
        case Expose:
        case ConfigureNotify:
            if (ev.type == ConfigureNotify ||
                ev.xexpose.count == 0)
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
            else if (ev.xbutton.button == Button5)
                scroll_offset -= 3;

            draw_terminal(win, gc, cmd, len);
            break;

        case KeyPress: {
            char text[32];
            KeySym key;
            int n = XLookupString(&ev.xkey, text,
                                  sizeof(text), &key, NULL);

            if (key == XK_Return || key == XK_KP_Enter) {
                if ((ev.xkey.state & ShiftMask) ||
                    quotes_balanced(cmd)) {
                    if (len < MAX_CMD - 1) {
                        cmd[len++] = '\n';
                        cmd[len] = '\0';
                    }
                } else {
                    char command[MAX_CMD];
                    snprintf(command, sizeof(command), "%s", cmd);

                    char *line = cmd;
                    char *next;

                    do {
                        next = strchr(line, '\n');
                        if (next)
                            *next = '\0';

                        char prompt[MAX_LINE_LEN];
                        snprintf(prompt, sizeof(prompt), "%s%s",
                                 line == cmd ? "$ " : "> ", line);
                        add_line(prompt);

                        if (!next)
                            break;

                        line = next + 1;
                    } while (1);

                    if (len)
                        execute_command(command);

                    len = 0;
                    cmd[0] = '\0';
                    scroll_offset = 0;
                }
            } else if (key == XK_BackSpace ||
                       key == XK_Delete) {
                if (len > 0)
                    cmd[--len] = '\0';

                scroll_offset = 0;
            } else if (n > 0 && text[0] >= 32 &&
                       text[0] <= 126) {
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

int main(void)
{
    if ((dpy = XOpenDisplay(NULL)) == NULL)
        err(1, "Can't open display");

    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);

    Window win = create_win(POSX, POSY, WIDTH, HEIGHT, BORDER);

    run(win);

    XUnmapWindow(dpy, win);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return last_status;
}
