/* script.c -- bash-subset script interpreter */
#define _GNU_SOURCE
#include "terminator.h"

/* ------------------------------------------------------------------ */
/* bash-subset script interpreter (Task 11)                           */
/* ------------------------------------------------------------------ */

/* growable string buffer */
typedef struct { char *p; size_t n, cap; } SB;

static void sb_put(SB *b, char c)
{
    if (b->n + 2 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char *np = realloc(b->p, nc);
        if (!np) return;
        b->p = np;
        b->cap = nc;
    }
    b->p[b->n++] = c;
    b->p[b->n] = '\0';
}

/* ---- resolve a path relative to this tab's cwd ---- */
void resolve_path(const char *p, char *out, size_t cap)
{
    if (!p || !*p) { snprintf(out, cap, "%s", g_cwd[0] ? g_cwd : "."); return; }
    if (p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        const char *h = getenv("HOME");
        snprintf(out, cap, "%s%s", h ? h : "", p + 1);
    } else if (p[0] == '/') {
        snprintf(out, cap, "%s", p);
    } else {
        while (p[0] == '.' && p[1] == '/')       /* drop a leading "./" */
            p += 2;
        snprintf(out, cap, "%s/%s", g_cwd[0] ? g_cwd : ".", p);
    }
}

/* does this path hold a script (text) rather than an executable binary? */
int looks_like_script(const char *path)
{
    int fd;
    char head[4];
    ssize_t n;
    struct stat sb;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    if (fstat(fd, &sb) < 0 || !S_ISREG(sb.st_mode)) { close(fd); return 0; }
    n = read(fd, head, sizeof head);
    close(fd);
    if (n >= 4 && !memcmp(head, "\x7f" "ELF", 4))
        return 0;                        /* a real binary: exec it normally */
    return 1;                            /* text / shebang script */
}

int is_interp_name(const char *s)
{
    const char *base = strrchr(s, '/');
    base = base ? base + 1 : s;
    return !strcmp(base, "bash") || !strcmp(base, "sh") ||
           !strcmp(base, "dash") || !strcmp(base, "terminator");
}



/* ---- script AST ---- */

#define ND_LIST   0
#define ND_SIMPLE 1
#define ND_IF     2

typedef struct Node Node;
typedef struct { Node *cond; Node *body; } Branch;

struct Node {
    int      kind;
    Node   **kids;      /* ND_LIST  */
    int      nkids;
    char    *cmd;       /* ND_SIMPLE */
    Branch  *br;        /* ND_IF    */
    int      nbr;
    Node    *else_body;
};

typedef struct { char *w; int quoted; int sep; } SItem;
typedef struct { SItem *it; int n; int i; char *err; size_t errcap; } PCtx;

static Node *node_new(int kind)
{
    Node *n = calloc(1, sizeof *n);
    if (n) n->kind = kind;
    return n;
}

static void node_kid(Node *list, Node *kid)
{
    Node **k = realloc(list->kids, sizeof(Node *) * (size_t)(list->nkids + 1));
    if (!k) return;
    list->kids = k;
    list->kids[list->nkids++] = kid;
}

static void node_branch(Node *n, Node *cond, Node *body)
{
    Branch *b = realloc(n->br, sizeof(Branch) * (size_t)(n->nbr + 1));
    if (!b) return;
    n->br = b;
    n->br[n->nbr].cond = cond;
    n->br[n->nbr].body = body;
    n->nbr++;
}

static void node_free(Node *n)
{
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
    free(n->cmd);
    for (int i = 0; i < n->nbr; i++) { node_free(n->br[i].cond); node_free(n->br[i].body); }
    free(n->br);
    node_free(n->else_body);
    free(n);
}

/* ---- scanner: split the script into words and separators ---- */

static int svec_push(SItem **v, int *n, int *cap, char *w, int quoted, int sep)
{
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        SItem *nv = realloc(*v, sizeof(SItem) * (size_t)nc);
        if (!nv) return 0;
        *v = nv;
        *cap = nc;
    }
    (*v)[*n].w = w;
    (*v)[*n].quoted = quoted;
    (*v)[*n].sep = sep;
    (*n)++;
    return 1;
}

static int script_scan(const char *src, SItem **out, int *nout, char *err, size_t errcap)
{
    SItem *v = NULL;
    int n = 0, cap = 0, i = 0;

    while (src[i]) {
        char c = src[i];

        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '\\' && src[i + 1] == '\n') { i += 2; continue; }
        if (c == '\n' || c == ';') { svec_push(&v, &n, &cap, NULL, 0, 1); i++; continue; }
        if (c == '#') {                       /* comment to end of line */
            while (src[i] && src[i] != '\n') i++;
            continue;
        }

        {
            SB sb = { NULL, 0, 0 };
            int quoted = 0;
            char q = 0;
            int paren = 0, btick = 0;

            while (src[i]) {
                char ch = src[i];

                if (q == 0 && paren == 0 && !btick &&
                    (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ';'))
                    break;

                if (q == '\'') {
                    if (ch == '\'') q = 0;
                    sb_put(&sb, ch); i++; continue;
                }
                if (q == '"') {
                    if (ch == '\\' && src[i + 1]) {
                        sb_put(&sb, ch); sb_put(&sb, src[i + 1]); i += 2; continue;
                    }
                    if (ch == '"') q = 0;
                    sb_put(&sb, ch); i++; continue;
                }
                if (ch == '\\' && src[i + 1]) {
                    sb_put(&sb, ch); sb_put(&sb, src[i + 1]);
                    quoted = 1; i += 2; continue;
                }
                if (ch == '\'') { q = '\''; quoted = 1; sb_put(&sb, ch); i++; continue; }
                if (ch == '"')  { q = '"';  quoted = 1; sb_put(&sb, ch); i++; continue; }
                if (ch == '$' && src[i + 1] == '(') {
                    paren++; sb_put(&sb, ch); sb_put(&sb, '('); i += 2; continue;
                }
                if (ch == '(' && paren) { paren++; sb_put(&sb, ch); i++; continue; }
                if (ch == ')' && paren) { paren--; sb_put(&sb, ch); i++; continue; }
                if (ch == '`') { btick = !btick; sb_put(&sb, ch); i++; continue; }
                sb_put(&sb, ch); i++;
            }

            if (q != 0 || paren != 0 || btick) {
                if (err) snprintf(err, errcap, "unterminated quote or substitution");
                free(sb.p);
                for (int k = 0; k < n; k++) free(v[k].w);
                free(v);
                return -1;
            }
            if (!sb.p)
                sb_put(&sb, '\0');
            svec_push(&v, &n, &cap, sb.p, quoted, 0);
        }
    }

    *out = v;
    *nout = n;
    return 0;
}

/* ---- parser ---- */

static void p_err(PCtx *p, const char *msg)
{
    if (p->err && !p->err[0])
        snprintf(p->err, p->errcap, "%s", msg);
}

static int p_end(PCtx *p) { return p->i >= p->n; }

static void p_skip_seps(PCtx *p)
{
    while (p->i < p->n && p->it[p->i].sep) p->i++;
}

static int p_res(PCtx *p, const char *w)
{
    return p->i < p->n && !p->it[p->i].sep && !p->it[p->i].quoted &&
           p->it[p->i].w && !strcmp(p->it[p->i].w, w);
}

static int p_terminator(PCtx *p)
{
    return p_res(p, "then") || p_res(p, "elif") || p_res(p, "else") || p_res(p, "fi");
}

static Node *parse_stmt_list(PCtx *p);

static Node *parse_simple(PCtx *p)
{
    SB sb = { NULL, 0, 0 };
    Node *n;

    while (p->i < p->n && !p->it[p->i].sep) {
        if (sb.n) sb_put(&sb, ' ');
        for (const char *q = p->it[p->i].w; q && *q; q++)
            sb_put(&sb, *q);
        p->i++;
    }
    if (!sb.p || sb.n == 0) { free(sb.p); return NULL; }
    n = node_new(ND_SIMPLE);
    if (!n) { free(sb.p); return NULL; }
    n->cmd = sb.p;
    return n;
}

static Node *parse_if(PCtx *p)
{
    Node *n = node_new(ND_IF);
    Node *cond, *body;

    if (!n) return NULL;
    p->i++;                                  /* 'if' */

    cond = parse_stmt_list(p);
    if (!cond) { node_free(n); return NULL; }
    if (!p_res(p, "then")) {
        p_err(p, "expected 'then' after 'if'");
        node_free(cond); node_free(n);
        return NULL;
    }
    p->i++;                                  /* 'then' */
    body = parse_stmt_list(p);
    if (!body) { node_free(cond); node_free(n); return NULL; }
    node_branch(n, cond, body);

    for (;;) {
        if (p_res(p, "elif")) {
            p->i++;
            cond = parse_stmt_list(p);
            if (!cond) { node_free(n); return NULL; }
            if (!p_res(p, "then")) {
                p_err(p, "expected 'then' after 'elif'");
                node_free(cond); node_free(n);
                return NULL;
            }
            p->i++;
            body = parse_stmt_list(p);
            if (!body) { node_free(n); return NULL; }
            node_branch(n, cond, body);
            continue;
        }
        if (p_res(p, "else")) {
            p->i++;
            n->else_body = parse_stmt_list(p);
            if (!n->else_body) { node_free(n); return NULL; }
            if (!p_res(p, "fi")) {
                p_err(p, "expected 'fi'");
                node_free(n);
                return NULL;
            }
            p->i++;
            return n;
        }
        if (p_res(p, "fi")) { p->i++; return n; }

        p_err(p, "expected 'fi'");
        node_free(n);
        return NULL;
    }
}

static Node *parse_stmt_list(PCtx *p)
{
    Node *list = node_new(ND_LIST);

    if (!list) return NULL;
    for (;;) {
        Node *st;
        p_skip_seps(p);
        if (p_end(p)) return list;
        if (p_terminator(p)) return list;
        if (p_res(p, "if"))
            st = parse_if(p);
        else
            st = parse_simple(p);
        if (!st) { node_free(list); return NULL; }
        node_kid(list, st);
    }
}

static int script_parse(SItem *it, int n, Node **root, char *err, size_t errcap)
{
    PCtx p;
    Node *r;

    memset(&p, 0, sizeof p);
    p.it = it; p.n = n; p.i = 0;
    p.err = err; p.errcap = errcap;
    if (err && errcap) err[0] = '\0';

    r = parse_stmt_list(&p);
    if (!r) return -1;

    p_skip_seps(&p);
    if (!p_end(&p)) {
        if (err) snprintf(err, errcap, "unexpected '%s'",
                          p.it[p.i].w ? p.it[p.i].w : "token");
        node_free(r);
        return -1;
    }
    *root = r;
    return 0;
}

/* ---- execute the AST ---- */

static void exec_node(Node *n)
{
    if (!n || g_script_exit)
        return;

    switch (n->kind) {
    case ND_LIST:
        for (int i = 0; i < n->nkids; i++) {
            exec_node(n->kids[i]);
            if (g_script_exit) return;
        }
        break;
    case ND_SIMPLE:
        if (n->cmd && *n->cmd)
            run_command_text(n->cmd);
        break;
    case ND_IF:
        for (int i = 0; i < n->nbr; i++) {
            exec_node(n->br[i].cond);
            if (g_script_exit) return;
            if (g_last_status == 0) { exec_node(n->br[i].body); return; }
        }
        exec_node(n->else_body);
        break;
    default:
        break;
    }
}

/*
 * Read, syntax-check and interpret a script.  Nothing runs unless the whole
 * file parses, so a missing 'fi' is reported instead of half-executing it.
 * `sourced` keeps cd/exit effects in the caller's shell.
 */
void run_script_file(const char *path, int argc, char **argv, int sourced)
{
    FILE *f;
    char *buf;
    long sz;
    SItem *items = NULL;
    int nitems = 0, i;
    Node *root = NULL;
    char err[256] = "";
    char save_cwd[PATH_MAX] = "";
    char *save_pos[MAX_POS];
    int save_npos;
    char save_name[PATH_MAX];

    f = fopen(path, "r");
    if (!f) {
        emit("%s: %s: %s\n", APP_NAME, path, strerror(errno));
        g_last_status = 1;
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) sz = 0;
    else sz = ftell(f);
    if (sz < 0) sz = 0;
    if (sz > (1 << 20)) sz = 1 << 20;
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); g_last_status = 1; return; }
    {
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = '\0';
    }
    fclose(f);

    if (script_scan(buf, &items, &nitems, err, sizeof err) < 0 ||
        script_parse(items, nitems, &root, err, sizeof err) < 0) {
        emit("%s: %s: syntax error: %s\n", APP_NAME, path, err);
        g_last_status = 2;
        free(buf);
        for (i = 0; i < nitems; i++) free(items[i].w);
        free(items);
        return;
    }
    free(buf);
    for (i = 0; i < nitems; i++) free(items[i].w);
    free(items);

    /* positional parameters and $0 for the duration of the script */
    snprintf(save_name, sizeof save_name, "%s", g_script_name);
    save_npos = g_npos;
    for (i = 0; i < MAX_POS; i++) { save_pos[i] = g_pos[i]; g_pos[i] = NULL; }
    g_npos = argc > MAX_POS ? MAX_POS : argc;
    for (i = 0; i < g_npos; i++)
        g_pos[i] = xstrdup(argv[i] ? argv[i] : "");
    snprintf(g_script_name, sizeof g_script_name, "%s", path);

    if (!sourced)
        snprintf(save_cwd, sizeof save_cwd, "%s", g_cwd);

    g_script_depth++;
    g_script_exit = 0;
    exec_node(root);
    g_script_exit = 0;
    g_script_depth--;

    if (!sourced && save_cwd[0])
        snprintf(g_cwd, sizeof g_cwd, "%s", save_cwd);

    for (i = 0; i < MAX_POS; i++) { free(g_pos[i]); g_pos[i] = save_pos[i]; }
    g_npos = save_npos;
    snprintf(g_script_name, sizeof g_script_name, "%s", save_name);

    node_free(root);
}

