/* edit.c -- line-editor operations: completion, Ctrl+R, suspend */
#define _GNU_SOURCE
#include "terminator.h"

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
    t->awaiting = 0;
    t->sel_active = 0;          /* a submitted line drops the highlight */
    t->dirty = 1;
}

/* ---------------- Ctrl+Z (Task 8) ---------------- */

void gui_suspend(Tab *t, int idx)
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
    t->awaiting = 0;                  /* the worker prints Stopped + a new prompt */
    t->dirty = 1;
}

/* ---------------- Ctrl+R search (Task 9) ---------------- */

/* length of the longest common *substring* of a and b */
int lcs_len(const char *a, const char *b)
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

void history_search(Tab *t, const char *term)
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

/* ---------------- Ctrl+R: readline-style reverse search ---------------- */

/* look backwards from `from` for a command containing the term */
static void search_step(Tab *t, int from)
{
    int i;

    if (from >= history_count())
        from = history_count() - 1;

    for (i = from; i >= 0; i--) {
        char b[HIST_ENTRY];
        if (!history_get(i, b, sizeof b))
            continue;
        if (t->search_len == 0 || strstr(b, t->search_buf)) {
            snprintf(t->search_match, sizeof t->search_match, "%s", b);
            t->search_pos = i;
            t->search_failed = 0;
            return;
        }
    }
    t->search_failed = 1;          /* keep showing the last match */
}

/* Ctrl+R pressed: remember the line, start searching from the newest entry */
static void search_begin(Tab *t)
{
    snprintf(t->search_orig, sizeof t->search_orig, "%s", t->input);
    t->searching = 1;
    t->search_len = 0;
    t->search_buf[0] = '\0';
    t->search_match[0] = '\0';
    t->search_failed = 0;
    t->search_pos = history_count() - 1;
    search_step(t, t->search_pos);
    t->dirty = 1;
}

/* put the match on the line and leave search mode */
static void search_accept(Tab *t)
{
    snprintf(t->input, sizeof t->input, "%s", t->search_match);
    t->len = (int)strlen(t->input);
    t->cursor = t->len;
    t->hist_pos = -1;
    t->searching = 0;
    t->dirty = 1;
}

/* leave search mode, restoring the line as it was before Ctrl+R */
static void search_cancel(Tab *t)
{
    snprintf(t->input, sizeof t->input, "%s", t->search_orig);
    t->len = (int)strlen(t->input);
    t->cursor = t->len;
    t->searching = 0;
    t->dirty = 1;
}

/* keystrokes while the (reverse-i-search) prompt is open */
static void gui_search_key(Tab *t, KeySym ks, int ctrl, int alt, char *text, int n)
{
    if (ks == XK_Return || ks == XK_KP_Enter) {
        search_accept(t);              /* accept and run it, as bash does */
        if (t->len > 0)
            gui_submit(t, g_active);
        return;
    }
    if (ks == XK_Right || ks == XK_End) {
        search_accept(t);              /* accept, leave it on the line */
        return;
    }
    if (ks == XK_Escape || (ctrl && (ks == XK_g || ks == XK_c))) {
        search_cancel(t);              /* abandon the search */
        return;
    }
    if (ctrl && ks == XK_r) {          /* Ctrl+R again: next older match */
        if (t->search_pos > 0)
            search_step(t, t->search_pos - 1);
        else
            t->search_failed = 1;
        t->dirty = 1;
        return;
    }
    if (ks == XK_BackSpace || ks == XK_Delete) {
        if (t->search_len > 0) {
            t->search_buf[--t->search_len] = '\0';
            search_step(t, history_count() - 1);
        } else {
            search_cancel(t);
        }
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
        search_step(t, history_count() - 1);   /* re-search from the newest */
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
        if (!t->awaiting)
            return;
        if (!t->searching) {
            search_begin(t);
        } else if (t->search_pos > 0) {
            search_step(t, t->search_pos - 1);   /* next older match */
            t->dirty = 1;
        } else {
            t->search_failed = 1;
            t->dirty = 1;
        }
        return;
    default: break;
    }
    g_tabs[g_active].dirty = 1;
}

void gui_handle_key(XKeyEvent *ke)
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
