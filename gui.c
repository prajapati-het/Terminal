/* gui.c -- the X11 front end */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* GUI                                                                */
/* ------------------------------------------------------------------ */

static unsigned char g_ctlbuf[MAX_TABS][1024];   /* partial control-message frames */
static int           g_ctlblen[MAX_TABS];

static Atom g_text_atom;
static Atom g_paste_prop;
static int  g_paste_tab = -1;
static Time g_last_time;          /* timestamp of the last X event   */
static int  g_paste_as_string;    /* retry the paste with XA_STRING  */

void tab_append_line(Tab *t, const char *s)
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

void gui_append_output(Tab *t, const char *data, int n, int idx)
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

void gui_draw(void)
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
        int sa_l = 0, sa_c = 0, sb_l = 0, sb_c = 0;

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
            sa_l = t->sel_l0; sa_c = t->sel_c0;
            sb_l = t->sel_l1; sb_c = t->sel_c1;
            if (sa_l > sb_l || (sa_l == sb_l && sa_c > sb_c)) {
                int tl = sa_l, tc = sa_c;
                sa_l = sb_l; sa_c = sb_c; sb_l = tl; sb_c = tc;
            }
        }

        for (k = start; k < total && k < start + visible; k++) {
            int y = top + (k - start) * g_ch;
            int len = vline_at(t, k, buf, sizeof buf);

            /* draw the line, then invert exactly the selected columns */
            XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
            XDrawString(g_dpy, g_win, g_gc, pad, y + g_font->ascent, buf, len);

            if (t->sel_active && k >= sa_l && k <= sb_l) {
                int hs = (k == sa_l) ? sa_c : 0;
                int he = (k == sb_l) ? sb_c : len;
                if (hs < 0) hs = 0;
                if (he > len) he = len;
                if (he > hs) {
                    int x0 = pad + text_width(buf, hs);
                    int x1 = pad + text_width(buf, he);
                    XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
                    XFillRectangle(g_dpy, g_win, g_gc, x0, y,
                                   (unsigned)(x1 - x0), (unsigned)g_ch);
                    XSetForeground(g_dpy, g_gc, BlackPixel(g_dpy, g_scr));
                    XDrawString(g_dpy, g_win, g_gc, x0, y + g_font->ascent,
                                buf + hs, he - hs);
                }
            }
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

            /* a solid block, with the character underneath drawn inverted */
            {
                char under = 0;
                if (t->searching) {
                    if (t->search_len < (int)sizeof t->search_buf)
                        under = t->search_buf[t->search_len];
                } else if (t->cursor < t->len && t->input[t->cursor] != '\n') {
                    under = t->input[t->cursor];
                }
                XSetForeground(g_dpy, g_gc, WhitePixel(g_dpy, g_scr));
                XFillRectangle(g_dpy, g_win, g_gc, cx, cy,
                               (unsigned)g_cw, (unsigned)g_ch);
                if (under) {
                    char one[2];
                    one[0] = under;
                    one[1] = '\0';
                    XSetForeground(g_dpy, g_gc, BlackPixel(g_dpy, g_scr));
                    XDrawString(g_dpy, g_win, g_gc, cx,
                                cy + g_font->ascent, one, 1);
                }
            }
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

void gui_copy(Tab *t)
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

    /* ICCCM: use the timestamp of the event that caused the copy, never
     * CurrentTime -- some clients ignore ownership set with CurrentTime. */
    XSetSelectionOwner(g_dpy, XA_PRIMARY, g_win, g_last_time);
    XSetSelectionOwner(g_dpy, g_clipboard, g_win, g_last_time);
}

void gui_request_paste(int idx, Atom sel)
{
    g_paste_tab = idx;
    g_paste_as_string = 0;
    XConvertSelection(g_dpy, sel, g_utf8, g_paste_prop, g_win, g_last_time);
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

    if (s->property == None) {
        /* the owner did not offer UTF8_STRING -- many apps only offer STRING */
        if (!g_paste_as_string) {
            g_paste_as_string = 1;
            XConvertSelection(g_dpy, s->selection, XA_STRING, g_paste_prop,
                              g_win, g_last_time);
        }
        return;
    }
    if (XGetWindowProperty(g_dpy, g_win, g_paste_prop, 0, (1 << 20), False,
                           AnyPropertyType, &type, &fmt, &nitems, &after,
                           &data) == Success && data) {
        gui_do_paste(data, (int)nitems);
        XFree(data);
    }
    XDeleteProperty(g_dpy, g_win, g_paste_prop);
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

int gui_new_tab(void)
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

void gui_close_tab(int idx)
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

void gui_run(void)
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
                    g_last_time = ev.xkey.time;
                    gui_handle_key(&ev.xkey);
                    break;
                case ButtonPress:
                    g_last_time = ev.xbutton.time;
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
                    g_last_time = ev.xbutton.time;
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
                    g_last_time = ev.xselectionrequest.time;
                    handle_selection_request(&ev.xselectionrequest);
                    break;
                case SelectionNotify:
                    g_last_time = ev.xselection.time;
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
