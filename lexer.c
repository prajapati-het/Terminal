/* lexer.c -- tokeniser, quoting, expansion and pipelines */
#define _GNU_SOURCE
#include "terminator.h"

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
int cmd_substitute(const char *cmd, char *out, size_t cap)
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

void var_set(const char *name, const char *val)
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

int lex_core(const char *src, Token *toks, int max, char *err,
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

int lex(const char *src, Token *toks, int max, char *err, size_t errcap)
{
    return lex_core(src, toks, max, err, errcap, 1);
}

int is_list_op(const Token *t)
{
    return t->is_op &&
           (!strcmp(t->text, "&&") || !strcmp(t->text, "||") || !strcmp(t->text, ";"));
}

int is_redir_op(const Token *t)
{
    return t->is_op &&
           (!strcmp(t->text, "<") || !strcmp(t->text, ">") ||
            !strcmp(t->text, ">>") || !strcmp(t->text, "2>") ||
            !strcmp(t->text, "2>>"));
}

int command_incomplete(const char *s)
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

int parse_stages(Token *toks, int nt, Stage *st, int *nst)
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

