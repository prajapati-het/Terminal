/* bindings.c -- key bindings and ~/.terminatorrc */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* key bindings / config                                              */
/* ------------------------------------------------------------------ */

const char *config_path(void)
{
    static char path[PATH_MAX];
    const char *h = getenv("HOME");
    snprintf(path, sizeof path, "%s/.terminatorrc", h ? h : ".");
    return path;
}

const char *action_name(Action a)
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
    default: return "none";
    }
}

Action action_from_name(const char *n)
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

int parse_keyspec(const char *spec, unsigned int *mods, KeySym *ks)
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

void bindings_init(void)
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
}

int bindings_add_user(const char *keyspec, const char *action, char *err, size_t cap)
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

Action bindings_lookup(unsigned int mods, KeySym ks)
{
    for (int i = 0; i < g_n_user_binds; i++)
        if (g_user_binds[i].mods == mods && ks_equal(g_user_binds[i].ks, ks))
            return g_user_binds[i].act;
    for (int i = 0; i < g_n_def_binds; i++)
        if (g_def_binds[i].mods == mods && ks_equal(g_def_binds[i].ks, ks))
            return g_def_binds[i].act;
    return ACT_NONE;
}

int bindings_save(const char *path)
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

int bindings_load(const char *path, int verbose)
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


