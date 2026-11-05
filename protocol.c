/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  protocol.c - Semantic pass, part 2: conversation protocol checking.
 *
 *  See protocol.h for the rules. Implementation outline:
 *
 *    A Tracker is created for every endpoint when it comes into scope
 *    (two per nali, one per sira parameter). Its `state` is the number of
 *    protocol steps already performed, or STATE_GONE once it was handed to
 *    a banda with paida. Trackers live on a stack: a block remembers the
 *    stack height on entry, checks the trackers it created on exit and pops
 *    them. A loop remembers the stack height on entry (loop_base); any
 *    operation on a tracker below loop_base inside the loop is rejected.
 *    agar/warna snapshot all tracker states, check each branch from the same
 *    snapshot and require identical results.
 */
#include "protocol.h"
#include "semantic.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define STATE_GONE (-2)

typedef struct Tracker {
    Node       *decl;        /* N_CHANDECL or N_PARAM                          */
    int         role;        /* ROLE_CLIENT / ROLE_SERVER                      */
    Node       *proto;       /* N_PROTOCOL                                     */
    char        display[160];/* "line.client" or "s"                           */
    int         state;       /* steps done, or STATE_GONE                      */
    int         used;        /* any bolo / suno / paida on this endpoint       */
    int         reported;    /* an error was already reported (no cascades)    */
    int         gone_line;   /* line of the paida that handed it over          */
    const char *gone_to;     /* name of that banda                             */
} Tracker;

typedef struct Snapshot {
    int  n;
    int *state;
    int *used;
} Snapshot;

static Tracker    *tr = NULL;
static int         ntr = 0, captr = 0;
static int         loop_base = -1;          /* -1: not inside a loop         */
static const char *loop_kind = NULL;        /* "jab_tak" or "ghoom"          */
static int         loop_line = 0;
static int         terminated = 0;          /* current path ended in waapis  */
static char        where[160];              /* "main", "banda 'server'", ... */
static int         errors = 0;

static void perr(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void perr(int line, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    sema_error(line, "protocol violation: %s", msg);
    errors++;
}

/* ------------------------------------------------------------------------ */
/*  Trackers                                                                */
/* ------------------------------------------------------------------------ */

static const char *role_name(int role) { return role == ROLE_CLIENT ? "client" : "server"; }

static int nsteps(const Tracker *t) { return t->proto->list->count; }

static Node *step_at(const Tracker *t, int i) { return t->proto->list->items[i]; }

static int new_tracker(Node *decl, int role, Node *proto, const char *display)
{
    if (ntr == captr) {
        captr = captr ? captr * 2 : 16;
        tr = realloc(tr, (size_t)captr * sizeof(Tracker));
        if (!tr) {
            fprintf(stderr, "rabta: out of memory\n");
            exit(3);
        }
    }
    Tracker *t = &tr[ntr];
    memset(t, 0, sizeof *t);
    t->decl = decl;
    t->role = role;
    t->proto = proto;
    snprintf(t->display, sizeof t->display, "%s", display);
    return ntr++;
}

/* Tracker for an endpoint reference (N_ENDPOINT or N_VAR), or NULL. */
static Tracker *tracker_of(const Node *ep)
{
    if (!ep || !ep->ref || ep->ty.kind != TY_ENDPOINT)
        return NULL;
    const Node *d = ep->ref;
    if (d->tid < 0)
        return NULL;
    int idx = d->kind == N_CHANDECL ? d->tid + ep->role : d->tid;
    if (idx < 0 || idx >= ntr)
        return NULL;
    return &tr[idx];
}

static const char *msg_type(Type t, char *buf, size_t n)
{
    return type_str(t, buf, n);
}

/* Name of what the endpoint should do next, e.g. "bolo i64". */
static const char *expected_action(const Tracker *t, char *buf, size_t n)
{
    char tb[64];
    if (t->state < 0 || t->state >= nsteps(t)) {
        snprintf(buf, n, "nothing");
        return buf;
    }
    Node *st = step_at(t, t->state);
    snprintf(buf, n, "%s %s", st->role == t->role ? "bolo" : "suno",
             msg_type(st->ty, tb, sizeof tb));
    return buf;
}

/* Common checks before an endpoint is used. Returns 1 if the use may go on. */
static int usable(Tracker *t, int line, const char *action)
{
    if (t->reported)
        return 0;
    if (t->state == STATE_GONE) {
        perr(line, "%s (%s.%s) was handed to banda '%s' with paida on line %d "
             "and cannot be used here any more (found %s)",
             t->display, t->proto->name, role_name(t->role), t->gone_to,
             t->gone_line, action);
        t->reported = 1;
        return 0;
    }
    if (loop_base >= 0 && (int)(t - tr) < loop_base) {
        perr(line, "%s (%s.%s) is used inside the %s loop on line %d; loop bodies must be "
             "protocol-neutral (a protocol has a fixed number of steps, a loop does not)",
             t->display, t->proto->name, role_name(t->role), loop_kind, loop_line);
        t->reported = 1;
        return 0;
    }
    return 1;
}

/* bolo (is_send = 1) or suno (is_send = 0) of a value of type `have`. */
static void endpoint_op(Node *ep, int is_send, Type have, int line)
{
    char exp[96], tb[64], hb[64];
    Tracker *t = tracker_of(ep);
    if (!t)
        return;
    if (!usable(t, line, is_send ? "bolo" : "suno"))
        return;
    t->used = 1;

    const char *P = t->proto->name, *R = role_name(t->role);
    int total = nsteps(t);

    if (t->state >= total) {
        perr(line, "%s.%s is used after the conversation finished "
             "(found %s on %s, but all %d step(s) of %s are done)",
             P, R, is_send ? "bolo" : "suno", t->display, total, P);
        t->reported = 1;
        return;
    }

    Node *st = step_at(t, t->state);
    int must_send = (st->role == t->role);
    expected_action(t, exp, sizeof exp);

    if (must_send && !is_send) {
        if (t->state == 0)
            perr(line, "%s.%s must speak first (expected %s, found suno)", P, R, exp);
        else
            perr(line, "%s.%s must speak at step %d (expected %s, found suno)",
                 P, R, t->state + 1, exp);
        t->reported = 1;
        return;
    }
    if (!must_send && is_send) {
        if (t->state == 0)
            perr(line, "%s.%s must listen first (expected %s, found bolo)", P, R, exp);
        else
            perr(line, "%s.%s must listen at step %d (expected %s, found bolo)",
                 P, R, t->state + 1, exp);
        t->reported = 1;
        return;
    }

    /* message type */
    if (have.kind != TY_NONE && have.kind != TY_ERROR && !type_equal(have, st->ty)) {
        if (is_send)
            perr(line, "%s.%s sends the wrong message type at step %d "
                 "(expected bolo %s, found bolo %s)",
                 P, R, t->state + 1, msg_type(st->ty, tb, sizeof tb),
                 msg_type(have, hb, sizeof hb));
        else
            perr(line, "%s.%s receives the wrong message type at step %d "
                 "(expected suno %s, found suno into %s)",
                 P, R, t->state + 1, msg_type(st->ty, tb, sizeof tb),
                 msg_type(have, hb, sizeof hb));
        t->reported = 1;
        return;
    }
    t->state++;
}

/* paida hands an endpoint to a new banda. */
static void endpoint_transfer(Node *ep, const char *actor, int line)
{
    Tracker *t = tracker_of(ep);
    if (!t)
        return;
    if (!usable(t, line, "paida"))
        return;
    if (t->used || t->state != 0) {
        perr(line, "cannot hand %s (%s.%s) to banda '%s' after it has been used here "
             "(%d of %d step(s) already done)",
             t->display, t->proto->name, role_name(t->role), actor, t->state, nsteps(t));
        t->reported = 1;
        return;
    }
    t->used = 1;
    t->state = STATE_GONE;
    t->gone_line = line;
    t->gone_to = actor;
}

/* Is the conversation of this tracker in an acceptable final state? */
static void check_finished(Tracker *t, int line, const char *at)
{
    char exp[96];
    if (t->reported || t->state == STATE_GONE)
        return;
    if (!t->used) {
        perr(line, "endpoint %s (%s.%s) is never used in %s",
             t->display, t->proto->name, role_name(t->role), where);
        t->reported = 1;
    } else if (t->state < nsteps(t)) {
        perr(line, "conversation on %s (%s.%s) is unfinished at %s "
             "(next expected: %s, step %d of %d)",
             t->display, t->proto->name, role_name(t->role), at,
             expected_action(t, exp, sizeof exp), t->state + 1, nsteps(t));
        t->reported = 1;
    }
}

/* ------------------------------------------------------------------------ */
/*  Snapshots (for agar / warna)                                            */
/* ------------------------------------------------------------------------ */

static Snapshot snap_take(void)
{
    Snapshot s;
    s.n = ntr;
    s.state = malloc(sizeof(int) * (size_t)(ntr ? ntr : 1));
    s.used = malloc(sizeof(int) * (size_t)(ntr ? ntr : 1));
    if (!s.state || !s.used) {
        fprintf(stderr, "rabta: out of memory\n");
        exit(3);
    }
    for (int i = 0; i < ntr; i++) {
        s.state[i] = tr[i].state;
        s.used[i] = tr[i].used;
    }
    return s;
}

static void snap_restore(const Snapshot *s)
{
    for (int i = 0; i < s->n && i < ntr; i++) {
        tr[i].state = s->state[i];
        tr[i].used = s->used[i];
    }
}

static void snap_free(Snapshot *s)
{
    free(s->state);
    free(s->used);
}

static void state_desc(int state, char *buf, size_t n)
{
    if (state == STATE_GONE)
        snprintf(buf, n, "handed to a banda");
    else
        snprintf(buf, n, "%d step(s) done", state);
}

/* ------------------------------------------------------------------------ */
/*  Walk                                                                    */
/* ------------------------------------------------------------------------ */

static void walk_stmts(NodeList *stmts);

/* A nested block: trackers created inside must be finished at its end. */
static void walk_block(Node *block, const char *what)
{
    int base = ntr;
    walk_stmts(block->list);
    if (!terminated) {
        char at[96];
        int end = block->ival > 0 ? (int)block->ival : block->line;
        snprintf(at, sizeof at, "the end of the %s (line %d)", what, end);
        for (int i = base; i < ntr; i++)
            check_finished(&tr[i], end, at);
    }
    ntr = base;
}

static void walk_if(Node *s)
{
    Snapshot before = snap_take();
    int term_before = terminated;

    walk_block(s->b, "agar block");
    int term_then = terminated;
    Snapshot after_then = snap_take();

    snap_restore(&before);
    terminated = term_before;
    if (s->c) {
        if (s->c->kind == N_IF)
            walk_if(s->c);
        else
            walk_block(s->c, "warna block");
    }
    int term_else = terminated;

    if (term_then && term_else) {
        terminated = 1;
    } else if (term_then) {
        /* only the warna path continues: keep its state */
    } else if (term_else) {
        snap_restore(&after_then);
    } else {
        for (int i = 0; i < before.n; i++) {
            Tracker *t = &tr[i];
            if (t->reported)
                continue;
            if (after_then.state[i] != t->state) {
                char a[48], b[48];
                state_desc(after_then.state[i], a, sizeof a);
                state_desc(t->state, b, sizeof b);
                if (!s->c)
                    perr(s->line, "agar without warna changes the state of %s (%s.%s) "
                         "(%s inside agar, %s when it is skipped); add a warna branch "
                         "that does the same conversation",
                         t->display, t->proto->name, role_name(t->role), a, b);
                else
                    perr(s->line, "the branches of agar leave %s (%s.%s) in different states "
                         "(agar: %s, warna: %s); both branches must do the same conversation",
                         t->display, t->proto->name, role_name(t->role), a, b);
                t->reported = 1;
            }
            t->used = t->used || after_then.used[i];
        }
        terminated = 0;
    }
    snap_free(&before);
    snap_free(&after_then);
}

static void walk_loop(Node *body, const char *kind, int line)
{
    int saved_base = loop_base, saved_line = loop_line, saved_term = terminated;
    const char *saved_kind = loop_kind;

    loop_base = ntr;
    loop_kind = kind;
    loop_line = line;
    walk_block(body, strcmp(kind, "ghoom") == 0 ? "ghoom body" : "jab_tak body");

    loop_base = saved_base;
    loop_kind = saved_kind;
    loop_line = saved_line;
    terminated = saved_term;   /* the loop may run zero times */
}

static void walk_stmt(Node *s)
{
    switch (s->kind) {
    case N_VARDECL:
    case N_ARRDECL:
    case N_ASSIGN:
        if (s->a && s->a->kind == N_RECV)
            endpoint_op(s->a->a, 0, s->a->ty, s->line);
        break;

    case N_IDXASSIGN:
        if (s->b->kind == N_RECV)
            endpoint_op(s->b->a, 0, s->b->ty, s->line);
        break;

    case N_RECVSTMT:
        endpoint_op(s->a->a, 0, type_make(TY_NONE), s->line);
        break;

    case N_SEND:
        endpoint_op(s->a, 1, s->b->ty, s->line);
        break;

    case N_SPAWN:
        for (int i = 0; i < s->list->count; i++) {
            Node *a = s->list->items[i];
            if (a->ty.kind == TY_ENDPOINT)
                endpoint_transfer(a, s->name, a->line);
        }
        break;

    case N_CHANDECL: {
        if (!s->ref)
            break;
        char d[160];
        snprintf(d, sizeof d, "%s.client", s->name);
        s->tid = new_tracker(s, ROLE_CLIENT, s->ref, d);
        snprintf(d, sizeof d, "%s.server", s->name);
        new_tracker(s, ROLE_SERVER, s->ref, d);
        break;
    }

    case N_IF:
        walk_if(s);
        break;

    case N_WHILE:
        walk_loop(s->b, "jab_tak", s->line);
        break;

    case N_FOR:
        walk_loop(s->c, "ghoom", s->line);
        break;

    case N_RETURN:
        for (int i = 0; i < ntr; i++)
            check_finished(&tr[i], s->line, "waapis");
        terminated = 1;
        break;

    default:
        break;   /* expressions never touch endpoints */
    }
}

static void walk_stmts(NodeList *stmts)
{
    for (int i = 0; i < stmts->count && !terminated; i++)
        walk_stmt(stmts->items[i]);
}

static void check_callable(Node *fn)
{
    ntr = 0;
    loop_base = -1;
    terminated = 0;
    if (fn->kind == N_ACTOR)
        snprintf(where, sizeof where, "banda '%s'", fn->name);
    else if (strcmp(fn->name, "main") == 0)
        snprintf(where, sizeof where, "main");
    else
        snprintf(where, sizeof where, "kaam '%s'", fn->name);

    for (int i = 0; i < fn->list->count; i++) {
        Node *p = fn->list->items[i];
        if (p->pkind == PARAM_ENDPOINT && p->ty.kind == TY_ENDPOINT)
            p->tid = new_tracker(p, p->ty.role, p->ty.proto, p->name);
    }

    walk_stmts(fn->a->list);

    if (!terminated) {
        char at[192];
        snprintf(at, sizeof at, "the end of %s", where);
        for (int i = 0; i < ntr; i++)
            check_finished(&tr[i], fn->a->ival > 0 ? (int)fn->a->ival : fn->line, at);
    }
}

int protocol_check(Node *program)
{
    errors = 0;
    for (int i = 0; i < program->list->count; i++) {
        Node *it = program->list->items[i];
        if (it->kind == N_FUNC || it->kind == N_ACTOR)
            check_callable(it);
    }
    free(tr);
    tr = NULL;
    ntr = captr = 0;
    return errors;
}
