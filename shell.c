/* shell.c -- command dispatch */
#define _GNU_SOURCE
#include "terminator.h"

/* ---- assignment: NAME=value ---- */
int try_assignment(const char *line)
{
    Token toks[4];
    char err[64], name[64];
    char *eq;
    int n, nt;

    if (!line || !strchr(line, '='))
        return 0;

    nt = lex(line, toks, 4, err, sizeof err);
    if (nt != 1 || toks[0].is_op)
        return 0;

    eq = strchr(toks[0].text, '=');
    if (!eq || eq == toks[0].text)
        return 0;

    n = (int)(eq - toks[0].text);
    if (n >= (int)sizeof name)
        return 0;
    if (!isalpha((unsigned char)toks[0].text[0]) && toks[0].text[0] != '_')
        return 0;
    for (int i = 0; i < n; i++)
        if (!isalnum((unsigned char)toks[0].text[i]) && toks[0].text[i] != '_')
            return 0;

    memcpy(name, toks[0].text, (size_t)n);
    name[n] = '\0';
    var_set(name, eq + 1);
    g_last_status = 0;
    return 1;
}

/* run one command line: assignment, otherwise lex + execute */
void run_command_text(const char *cmd)
{
    Token toks[MAX_ARGS * 2];
    char err[128];
    int nt;

    if (!cmd || !*cmd)
        return;
    if (try_assignment(cmd))
        return;

    nt = lex(cmd, toks, MAX_ARGS * 2, err, sizeof err);
    if (nt < 0) {
        emit("%s: %s\n", APP_NAME, err);
        g_last_status = 2;
        return;
    }
    if (nt == 0)
        return;
    execute_tokens(toks, nt);
}



/* ------------------------------------------------------------------ */
/* command dispatch                                                   */
/* ------------------------------------------------------------------ */

void run_one(Token *toks, int nt)
{
    Stage st[MAX_STAGES];
    int nst;

    if (!parse_stages(toks, nt, st, &nst)) {
        emit("%s: syntax error near unexpected token\n", APP_NAME);
        g_last_status = 2;
        return;
    }
    run_pipeline(st, nst);
}

void execute_tokens(Token *toks, int nt)
{
    int i = 0;
    int next_run = 1;

    while (i < nt) {
        int j = i;
        int executed;
        const char *op = NULL;

        while (j < nt && !is_list_op(&toks[j]))
            j++;

        executed = next_run;
        if (executed && j > i)
            run_one(&toks[i], j - i);

        if (j >= nt)
            break;

        op = toks[j].text;
        if (!strcmp(op, "&&"))
            next_run = executed ? (g_last_status == 0) : 0;
        else if (!strcmp(op, "||"))
            next_run = executed ? (g_last_status != 0) : 1;
        else
            next_run = 1;

        i = j + 1;
    }
}

static int word_is(const char *p, const char *w)
{
    size_t L = strlen(w);
    /* case-insensitive: the brief spells it multiWatch, but accept multiwatch */
    if (strncasecmp(p, w, L))
        return 0;
    return p[L] == '\0' || isspace((unsigned char)p[L]);
}

static const char *skip_word(const char *p)
{
    while (*p && !isspace((unsigned char)*p)) p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

void execute_line(const char *line)
{
    const char *p = line;

    while (*p && isspace((unsigned char)*p))
        p++;
    if (!*p)
        return;

    /* remember the command line for job listings (jobs/fg/bg, Ctrl+Z) */
    snprintf(g_cur_cmd, sizeof g_cur_cmd, "%s", p);

    if (word_is(p, "multiWatch")) {
        history_add(line);
        do_multiwatch(skip_word(p));
        return;
    }
    if (word_is(p, "watch")) {
        history_add(line);
        do_watch(skip_word(p));
        return;
    }

    history_add(line);
    run_command_text(p);
}
