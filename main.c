#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <err.h>

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

static Display *dpy;
static int scr;
static Window root;

static char history[MAX_LINES][MAX_LINE_LEN];
static int line_count = 0;
static int scroll_offset = 0;

static Window create_win(int x, int y, int w, int h, int b)
{
    Window win;
    XSetWindowAttributes xwa;

    xwa.background_pixel = BlackPixel(dpy, scr);
    xwa.border_pixel = WhitePixel(dpy, scr);
    xwa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask;

    win = XCreateWindow(dpy, root, x, y, w, h, b,
        DefaultDepth(dpy, scr), InputOutput, DefaultVisual(dpy, scr),
        CWBackPixel | CWBorderPixel | CWEventMask, &xwa);

    return win;
}

static void add_line(const char *s)
{
    if (line_count == MAX_LINES)
    {
        memmove(history, history + 1,
            sizeof(history[0]) * (MAX_LINES - 1));
        line_count--;
    }

    snprintf(history[line_count], MAX_LINE_LEN, "%s", s);
    line_count++;
}

static void draw_terminal(Window win, GC gc, const char *cmd, int len)
{
    int visible = (HEIGHT - 20) / LINE_HEIGHT;
    int total = line_count + 1;
    int max_scroll = total > visible ? total - visible : 0;
    int start;
    int i;

    if (scroll_offset > max_scroll)
        scroll_offset = max_scroll;

    start = total - visible - scroll_offset;
    if (start < 0)
        start = 0;

    XClearWindow(dpy, win);

    for (i = start; i < total && i < start + visible; i++)
    {
        int y = 20 + (i - start) * LINE_HEIGHT;

        if (i < line_count)
        {
            XDrawString(dpy, win, gc, 10, y,
                history[i], (int)strlen(history[i]));
        }
        else
        {
            XDrawString(dpy, win, gc, 10, y, "$ ", 2);
            XDrawString(dpy, win, gc, 25, y, cmd, len);
        }
    }

    XFlush(dpy);
}

static void execute_command(char *cmd)
{
    int fd[2];
    int status;
    pid_t pid;
    char *args[MAX_ARGS];
    char buffer[256];
    char output[MAX_LINE_LEN];
    int argc = 0;
    size_t out_len = 0;
    FILE *fp;

    char *token = strtok(cmd, " \t\r\n");

    while (token != NULL && argc < MAX_ARGS - 1)
    {
        args[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }

    args[argc] = NULL;

    if (argc == 0)
        return;

    if (token != NULL)
    {
        add_line("Too many arguments");
        return;
    }

    if (pipe(fd) == -1)
    {
        add_line("pipe() failed");
        return;
    }

    pid = fork();

    if (pid == -1)
    {
        close(fd[0]);
        close(fd[1]);
        add_line("fork() failed");
        return;
    }

    if (pid == 0)
    {
        close(fd[0]);

        if (dup2(fd[1], STDOUT_FILENO) == -1 ||
            dup2(fd[1], STDERR_FILENO) == -1)
            _exit(126);

        close(fd[1]);

        execvp(args[0], args);

        perror(args[0]);
        _exit(127);
    }

    close(fd[1]);

    fp = fdopen(fd[0], "r");

    if (fp == NULL)
    {
        close(fd[0]);
        waitpid(pid, &status, 0);
        add_line("fdopen() failed");
        return;
    }

    while (fgets(buffer, sizeof(buffer), fp) != NULL)
    {
        size_t i;

        for (i = 0; buffer[i] != '\0'; i++)
        {
            if (buffer[i] == '\r')
                continue;

            if (buffer[i] == '\n')
            {
                output[out_len] = '\0';
                add_line(output);
                out_len = 0;
            }
            else
            {
                output[out_len++] = buffer[i];

                if (out_len == MAX_LINE_LEN - 1)
                {
                    output[out_len] = '\0';
                    add_line(output);
                    out_len = 0;
                }
            }
        }
    }

    if (out_len > 0)
    {
        output[out_len] = '\0';
        add_line(output);
    }

    fclose(fp);
    waitpid(pid, &status, 0);
}


static void run(Window win)
{
    XEvent ev;
    XGCValues values;
    GC gc;

    char cmd[MAX_CMD];
    int len = 0;

    cmd[0] = '\0';

    values.foreground = WhitePixel(dpy, scr);
    values.background = BlackPixel(dpy, scr);

    gc = XCreateGC(dpy, win, GCForeground | GCBackground, &values);

    XMapWindow(dpy, win);
    XFlush(dpy);

    while (1)
    {
        XNextEvent(dpy, &ev);

        switch (ev.type)
        {
            case Expose:
                if (ev.xexpose.count == 0)
                    draw_terminal(win, gc, cmd, len);
                break;

            case ButtonPress:
                if (ev.xbutton.button == Button4)
                {
                    scroll_offset += 3;
                }
                else if (ev.xbutton.button == Button5)
                {
                    scroll_offset -= 3;

                    if (scroll_offset < 0)
                        scroll_offset = 0;
                }
                else
                {
                    XSetInputFocus(dpy, win, RevertToParent,
                        CurrentTime);
                }

                draw_terminal(win, gc, cmd, len);
                break;

            case KeyPress:
            {
                char text[32];
                KeySym key;
                int n;

                n = XLookupString(&ev.xkey, text, sizeof(text),
                    &key, NULL);

                if (key == XK_Return || key == XK_KP_Enter)
                {
                    char prompt[MAX_LINE_LEN];

                    cmd[len] = '\0';
                    snprintf(prompt, sizeof(prompt), "$ %s", cmd);
                    add_line(prompt);

                    if (len > 0)
                        execute_command(cmd);

                    len = 0;
                    cmd[0] = '\0';
                    scroll_offset = 0;
                }
                else if (key == XK_BackSpace || key == XK_Delete)
                {
                    if (len > 0)
                    {
                        len--;
                        cmd[len] = '\0';
                    }

                    scroll_offset = 0;
                }
                else if (n > 0 && text[0] >= 32 && text[0] <= 126)
                {
                    if (len < MAX_CMD - 1)
                    {
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
    Window win;

    if ((dpy = XOpenDisplay(NULL)) == NULL)
        err(1, "Can't open display");

    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);

    win = create_win(POSX, POSY, WIDTH, HEIGHT, BORDER);

    run(win);

    XUnmapWindow(dpy, win);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return 0;
}