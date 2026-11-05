/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  semantic.c - Semantic pass, part 1: symbol table and type checking.
 *
 *  Checks performed:
 *    - declarations: duplicate protocols/actors/functions, duplicate locals
 *      in one scope, undeclared variables, unknown protocols/actors/functions
 *    - types: i64 vs thali vs channel vs endpoint in every expression,
 *      assignment, condition, index, call argument and message
 *    - calls: argument counts and argument kinds for bulao and paida
 *    - return rules: every kaam must end in waapis on every path, waapis
 *      needs an i64 value, waapis is not allowed inside a banda,
 *      main() must exist and take no parameters
 *    - protocol declarations: roles must be client/server, array sizes > 0
 */
#include "semantic.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/*  Error reporting                                                         */
/* ------------------------------------------------------------------------ */

static int error_total = 0;

void sema_error(int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", rabta_filename, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    error_total++;
}

int sema_error_total(void)
{
    return error_total;
}

/* ------------------------------------------------------------------------ */
/*  Top-level lookup                                                        */
/* ------------------------------------------------------------------------ */

static Node *find_item(Node *program, NodeKind kind, const char *name)
{
    for (int i = 0; i < program->list->count; i++) {
        Node *it = program->list->items[i];
        if (it->kind == kind && strcmp(it->name, name) == 0)
            return it;
    }
    return NULL;
}

Node *find_protocol(Node *program, const char *name) { return find_item(program, N_PROTOCOL, name); }
Node *find_function(Node *program, const char *name) { return find_item(program, N_FUNC, name); }
Node *find_actor(Node *program, const char *name)    { return find_item(program, N_ACTOR, name); }

/* ------------------------------------------------------------------------ */
/*  Local symbol table (scoped)                                             */
/* ------------------------------------------------------------------------ */

typedef enum { SYM_I64, SYM_ARRAY, SYM_CHANNEL, SYM_ENDPOINT } SymKind;

typedef struct Sym {
    const char *name;
    SymKind     kind;
    Type        ty;
    int         slot;
    int         depth;
    Node       *decl;
} Sym;

static Sym  *syms = NULL;
static int   nsyms = 0, capsyms = 0;
static int   depth = 0;
static int   next_slot = 0;

static Node *program_root = NULL;

/* names already reported as undeclared in the current kaam/banda */
static const char *undeclared[64];
static int         nundeclared = 0;
static Node *cur_fn = NULL;          /* N_FUNC or N_ACTOR being checked */

static void scope_push(void) { depth++; }

static void scope_pop(void)
{
    while (nsyms > 0 && syms[nsyms - 1].depth == depth)
        nsyms--;
    depth--;
}

static Sym *lookup(const char *name)
{
    for (int i = nsyms - 1; i >= 0; i--)
        if (strcmp(syms[i].name, name) == 0)
            return &syms[i];
    return NULL;
}

/* Declare a local name; returns its slot. */
static int declare(Node *decl, const char *name, SymKind kind, Type ty)
{
    for (int i = nsyms - 1; i >= 0 && syms[i].depth == depth; i--) {
        if (strcmp(syms[i].name, name) == 0) {
            sema_error(decl->line, "'%s' is already declared in this scope (line %d)",
                       name, syms[i].decl->line);
            break;
        }
    }
    if (nsyms == capsyms) {
        capsyms = capsyms ? capsyms * 2 : 32;
        syms = realloc(syms, (size_t)capsyms * sizeof(Sym));
        if (!syms) {
            fprintf(stderr, "rabta: out of memory\n");
            exit(3);
        }
    }
    Sym *s = &syms[nsyms++];
    s->name = name;
    s->kind = kind;
    s->ty = ty;
    s->slot = next_slot++;
    s->depth = depth;
    s->decl = decl;
    return s->slot;
}

/* ------------------------------------------------------------------------ */
/*  Helpers                                                                 */
/* ------------------------------------------------------------------------ */

static const char *where_name(void)
{
    static char buf[160];
    if (!cur_fn)
        return "?";
    if (cur_fn->kind == N_ACTOR)
        snprintf(buf, sizeof buf, "banda '%s'", cur_fn->name);
    else
        snprintf(buf, sizeof buf, "kaam '%s'", cur_fn->name);
    return buf;
}

static int role_of(const char *s)
{
    if (strcmp(s, "client") == 0) return ROLE_CLIENT;
    if (strcmp(s, "server") == 0) return ROLE_SERVER;
    return ROLE_NONE;
}

/* Require an i64; `what` describes the position, e.g. "condition of agar". */
static void require_i64(Type t, int line, const char *what)
{
    char b[64];
    if (t.kind == TY_I64 || t.kind == TY_ERROR)
        return;
    sema_error(line, "type mismatch: %s must be i64, found %s", what, type_str(t, b, sizeof b));
}

static Type check_expr(Node *e);

/* Report an undeclared variable once per kaam/banda. */
static void report_undeclared(int line, const char *name)
{
    for (int i = 0; i < nundeclared; i++)
        if (strcmp(undeclared[i], name) == 0)
            return;
    if (nundeclared < 64)
        undeclared[nundeclared++] = name;
    sema_error(line, "undeclared variable '%s' (declare it first with rakho)", name);
}

/* ------------------------------------------------------------------------ */
/*  Endpoints                                                               */
/* ------------------------------------------------------------------------ */

/* Resolve `ch.role` (N_ENDPOINT) or `s` (N_VAR naming a sira parameter). */
static Type check_endpoint(Node *ep)
{
    if (ep->kind == N_ENDPOINT) {
        Sym *s = lookup(ep->name);
        if (!s) {
            sema_error(ep->line, "undeclared channel '%s'", ep->name);
            return ep->ty = type_make(TY_ERROR);
        }
        if (s->kind != SYM_CHANNEL) {
            sema_error(ep->line, "'%s' is not a channel, so '%s.%s' is not an endpoint",
                       ep->name, ep->name, ep->name2);
            return ep->ty = type_make(TY_ERROR);
        }
        if (s->ty.kind == TY_ERROR)
            return ep->ty = type_make(TY_ERROR);
        int role = role_of(ep->name2);
        if (role == ROLE_NONE) {
            sema_error(ep->line, "'%s' is not a role; use %s.client or %s.server",
                       ep->name2, ep->name, ep->name);
            return ep->ty = type_make(TY_ERROR);
        }
        ep->slot = s->slot;
        ep->ref = s->decl;
        ep->role = role;
        ep->ty = type_make(TY_ENDPOINT);
        ep->ty.proto = s->ty.proto;
        ep->ty.role = role;
        return ep->ty;
    }

    if (ep->kind == N_VAR) {
        Sym *s = lookup(ep->name);
        if (!s) {
            sema_error(ep->line, "undeclared endpoint '%s'", ep->name);
            return ep->ty = type_make(TY_ERROR);
        }
        if (s->kind == SYM_CHANNEL) {
            sema_error(ep->line, "'%s' is a channel; use %s.client or %s.server",
                       ep->name, ep->name, ep->name);
            return ep->ty = type_make(TY_ERROR);
        }
        if (s->kind != SYM_ENDPOINT) {
            sema_error(ep->line, "'%s' is not an endpoint (it is a %s variable)",
                       ep->name, s->kind == SYM_ARRAY ? "thali" : "i64");
            return ep->ty = type_make(TY_ERROR);
        }
        ep->slot = s->slot;
        ep->ref = s->decl;
        ep->role = s->ty.role;
        ep->ty = s->ty;
        return ep->ty;
    }

    sema_error(ep->line, "expected an endpoint (channel.client, channel.server or a sira parameter)");
    return ep->ty = type_make(TY_ERROR);
}

/* suno ep, received into a destination of type `dest` (TY_NONE = discard). */
static void check_recv(Node *recv, Type dest)
{
    check_endpoint(recv->a);
    recv->ty = dest;
}

/* ------------------------------------------------------------------------ */
/*  Expressions                                                             */
/* ------------------------------------------------------------------------ */

static void check_call_args(Node *call, Node *fn)
{
    NodeList *params = fn->list, *args = call->list;
    if (params->count != args->count) {
        sema_error(call->line, "kaam '%s' expects %d argument(s), found %d",
                   fn->name, params->count, args->count);
        for (int i = 0; i < args->count; i++)
            check_expr(args->items[i]);
        return;
    }
    for (int i = 0; i < args->count; i++) {
        Node *p = params->items[i], *a = args->items[i];
        Type t = check_expr(a);
        char what[128], b[64];
        if (p->pkind == PARAM_ARRAY) {
            if (t.kind == TY_ERROR)
                continue;
            if (a->kind != N_VAR || t.kind != TY_ARRAY)
                sema_error(a->line, "type mismatch: argument %d of '%s' must be a thali variable, found %s",
                           i + 1, fn->name, type_str(t, b, sizeof b));
        } else {
            snprintf(what, sizeof what, "argument %d of '%s'", i + 1, fn->name);
            require_i64(t, a->line, what);
        }
    }
}

static Type check_expr(Node *e)
{
    char b1[64], b2[64];

    switch (e->kind) {
    case N_INT:
        return e->ty = type_make(TY_I64);

    case N_STR:
        sema_error(e->line, "string literals can only be printed with dikhao");
        return e->ty = type_make(TY_ERROR);

    case N_VAR: {
        Sym *s = lookup(e->name);
        if (!s) {
            report_undeclared(e->line, e->name);
            return e->ty = type_make(TY_ERROR);
        }
        e->slot = s->slot;
        e->ref = s->decl;
        if (s->kind == SYM_ENDPOINT)
            e->role = s->ty.role;
        return e->ty = s->ty;
    }

    case N_INDEX: {
        Sym *s = lookup(e->name);
        Type it = check_expr(e->a);
        require_i64(it, e->a->line, "array index");
        if (!s) {
            report_undeclared(e->line, e->name);
            return e->ty = type_make(TY_ERROR);
        }
        if (s->kind != SYM_ARRAY) {
            sema_error(e->line, "type mismatch: '%s' is %s, it cannot be indexed",
                       e->name, type_str(s->ty, b1, sizeof b1));
            return e->ty = type_make(TY_ERROR);
        }
        e->slot = s->slot;
        e->ref = s->decl;
        return e->ty = type_make(TY_I64);
    }

    case N_BINOP: {
        Type l = check_expr(e->a);
        Type r = check_expr(e->b);
        if (l.kind == TY_ERROR || r.kind == TY_ERROR)
            return e->ty = type_make(TY_ERROR);
        if (l.kind != TY_I64 || r.kind != TY_I64) {
            sema_error(e->line, "type mismatch: operator '%s' needs i64 operands, found %s and %s",
                       op_symbol(e->op), type_str(l, b1, sizeof b1), type_str(r, b2, sizeof b2));
            return e->ty = type_make(TY_ERROR);
        }
        return e->ty = type_make(TY_I64);
    }

    case N_UNOP: {
        Type t = check_expr(e->a);
        if (t.kind == TY_ERROR)
            return e->ty = t;
        if (t.kind != TY_I64) {
            sema_error(e->line, "type mismatch: operator '%s' needs an i64 operand, found %s",
                       op_symbol(e->op), type_str(t, b1, sizeof b1));
            return e->ty = type_make(TY_ERROR);
        }
        return e->ty = type_make(TY_I64);
    }

    case N_CALL: {
        Node *fn = find_function(program_root, e->name);
        if (!fn) {
            if (find_actor(program_root, e->name))
                sema_error(e->line, "'%s' is a banda; start it with paida, not bulao", e->name);
            else
                sema_error(e->line, "call to undeclared kaam '%s'", e->name);
            for (int i = 0; i < e->list->count; i++)
                check_expr(e->list->items[i]);
            return e->ty = type_make(TY_ERROR);
        }
        if (strcmp(fn->name, "main") == 0)
            sema_error(e->line, "main cannot be called with bulao");
        e->ref = fn;
        check_call_args(e, fn);
        return e->ty = type_make(TY_I64);
    }

    case N_ENDPOINT:
        return check_endpoint(e);

    case N_ARRLIT:
        sema_error(e->line, "array literals are only allowed in a thali declaration");
        return e->ty = type_make(TY_ERROR);

    case N_RECV:
        sema_error(e->line, "suno can only be used as a whole value: rakho x = suno ep");
        return e->ty = type_make(TY_ERROR);

    default:
        sema_error(e->line, "internal error: unexpected %s in expression", node_kind_name(e->kind));
        return e->ty = type_make(TY_ERROR);
    }
}

/* An expression used as a plain i64 value (rejects endpoints/arrays). */
static void check_i64_expr(Node *e, const char *what)
{
    Type t = check_expr(e);
    require_i64(t, e->line, what);
}

/* ------------------------------------------------------------------------ */
/*  Statements                                                              */
/* ------------------------------------------------------------------------ */

static void check_stmts(NodeList *stmts);

static void check_block(Node *block)
{
    scope_push();
    check_stmts(block->list);
    scope_pop();
}

static void check_spawn(Node *s)
{
    char b1[64], b2[64];
    Node *actor = find_actor(program_root, s->name);
    if (!actor) {
        if (find_function(program_root, s->name))
            sema_error(s->line, "'%s' is a kaam; call it with bulao, not paida", s->name);
        else
            sema_error(s->line, "paida of undeclared banda '%s'", s->name);
        return;
    }
    s->ref = actor;
    NodeList *params = actor->list, *args = s->list;
    if (params->count != args->count) {
        sema_error(s->line, "banda '%s' expects %d argument(s), found %d",
                   actor->name, params->count, args->count);
        return;
    }
    for (int i = 0; i < args->count; i++) {
        Node *p = params->items[i], *a = args->items[i];
        if (p->pkind == PARAM_ENDPOINT) {
            if (a->kind != N_ENDPOINT && a->kind != N_VAR) {
                check_expr(a);
                sema_error(a->line, "type mismatch: argument %d of paida %s must be an endpoint (%s)",
                           i + 1, actor->name, type_str(p->ty, b1, sizeof b1));
                continue;
            }
            Type t = check_endpoint(a);
            if (t.kind == TY_ERROR || p->ty.kind == TY_ERROR)
                continue;
            if (!type_equal(t, p->ty))
                sema_error(a->line, "type mismatch: argument %d of paida %s: expected %s, found %s",
                           i + 1, actor->name, type_str(p->ty, b1, sizeof b1),
                           type_str(t, b2, sizeof b2));
        } else {
            char what[128];
            snprintf(what, sizeof what, "argument %d of paida %s", i + 1, actor->name);
            check_i64_expr(a, what);
        }
    }
}

static void check_stmt(Node *s)
{
    char b1[64], b2[64];

    switch (s->kind) {
    case N_VARDECL:
        if (s->a->kind == N_RECV)
            check_recv(s->a, type_make(TY_I64));
        else {
            Type t = check_expr(s->a);
            if (t.kind != TY_I64 && t.kind != TY_ERROR)
                sema_error(s->line, "type mismatch: rakho %s needs an i64 value, found %s%s",
                           s->name, type_str(t, b1, sizeof b1),
                           t.kind == TY_ARRAY ? " (declare arrays with thali)" : "");
        }
        s->slot = declare(s, s->name, SYM_I64, type_make(TY_I64));
        break;

    case N_ARRDECL: {
        if (s->ival <= 0 || s->ival > RABTA_MAX_ARRAY)
            sema_error(s->line, "thali '%s' size must be between 1 and %d, found %" PRId64,
                       s->name, RABTA_MAX_ARRAY, s->ival);
        Type at = type_array(s->ival);
        if (s->a && s->a->kind == N_ARRLIT) {
            NodeList *el = s->a->list;
            if (el->count > s->ival)
                sema_error(s->line, "thali '%s' has size %" PRId64 " but %d initial values were given",
                           s->name, s->ival, el->count);
            for (int i = 0; i < el->count; i++)
                check_i64_expr(el->items[i], "thali element");
        } else if (s->a && s->a->kind == N_RECV) {
            check_recv(s->a, at);
        }
        s->ty = at;
        s->slot = declare(s, s->name, SYM_ARRAY, at);
        break;
    }

    case N_ASSIGN: {
        Sym *v = lookup(s->name);
        if (!v) {
            report_undeclared(s->line, s->name);
            if (s->a->kind == N_RECV)
                check_recv(s->a, type_make(TY_NONE));
            else
                check_expr(s->a);
            break;
        }
        s->slot = v->slot;
        s->ty = v->ty;
        if (v->kind == SYM_CHANNEL || v->kind == SYM_ENDPOINT) {
            sema_error(s->line, "cannot assign to %s '%s'",
                       v->kind == SYM_CHANNEL ? "channel" : "endpoint", s->name);
            break;
        }
        if (s->a->kind == N_RECV) {
            check_recv(s->a, v->ty);
            break;
        }
        Type t = check_expr(s->a);
        if (v->kind == SYM_ARRAY) {
            sema_error(s->line, "cannot assign a whole thali '%s'; assign elements with %s[i] = ...",
                       s->name, s->name);
        } else if (t.kind != TY_I64 && t.kind != TY_ERROR) {
            sema_error(s->line, "type mismatch: cannot assign %s to i64 variable '%s'",
                       type_str(t, b1, sizeof b1), s->name);
        }
        break;
    }

    case N_IDXASSIGN: {
        Sym *v = lookup(s->name);
        check_i64_expr(s->a, "array index");
        if (s->b->kind == N_RECV)
            check_recv(s->b, type_make(TY_I64));
        else
            check_i64_expr(s->b, "value stored in a thali element");
        if (!v) {
            report_undeclared(s->line, s->name);
            break;
        }
        if (v->kind != SYM_ARRAY) {
            sema_error(s->line, "type mismatch: '%s' is %s, it cannot be indexed",
                       s->name, type_str(v->ty, b1, sizeof b1));
            break;
        }
        s->slot = v->slot;
        break;
    }

    case N_IF:
        check_i64_expr(s->a, "condition of agar");
        check_block(s->b);
        if (s->c) {
            if (s->c->kind == N_IF)
                check_stmt(s->c);
            else
                check_block(s->c);
        }
        break;

    case N_WHILE:
        check_i64_expr(s->a, "condition of jab_tak");
        check_block(s->b);
        break;

    case N_FOR:
        check_i64_expr(s->a, "start of ghoom range");
        check_i64_expr(s->b, "end of ghoom range");
        scope_push();
        s->slot = declare(s, s->name, SYM_I64, type_make(TY_I64));
        check_block(s->c);
        scope_pop();
        break;

    case N_RETURN:
        if (cur_fn->kind == N_ACTOR) {
            sema_error(s->line, "waapis is not allowed inside banda '%s' (an actor ends at its khatam)",
                       cur_fn->name);
            check_expr(s->a);
            break;
        }
        check_i64_expr(s->a, "value of waapis");
        break;

    case N_PRINT:
        for (int i = 0; i < s->list->count; i++) {
            Node *a = s->list->items[i];
            if (a->kind == N_STR) {
                a->ty = type_make(TY_STRING);
                continue;
            }
            Type t = check_expr(a);
            if (t.kind == TY_CHANNEL || t.kind == TY_ENDPOINT)
                sema_error(a->line, "type mismatch: dikhao cannot print %s",
                           type_str(t, b1, sizeof b1));
        }
        break;

    case N_CALLSTMT:
        check_expr(s->a);
        break;

    case N_CHANDECL: {
        Node *p = find_protocol(program_root, s->name2);
        Type ct = type_make(TY_CHANNEL);
        if (!p) {
            sema_error(s->line, "unknown protocol '%s' in nali %s", s->name2, s->name);
            ct = type_make(TY_ERROR);
        }
        ct.proto = p;
        s->ref = p;
        s->ty = ct;
        s->slot = declare(s, s->name, SYM_CHANNEL, ct);
        break;
    }

    case N_SPAWN:
        check_spawn(s);
        break;

    case N_SEND: {
        check_endpoint(s->a);
        Type t = check_expr(s->b);
        if (t.kind == TY_ERROR)
            break;
        if (t.kind == TY_ARRAY) {
            if (t.size < 0)
                sema_error(s->line, "cannot send thali parameter '%s' of unknown size",
                           s->b->name);
        } else if (t.kind != TY_I64) {
            sema_error(s->line, "type mismatch: bolo can send i64 or thali values, found %s",
                       type_str(t, b2, sizeof b2));
        }
        break;
    }

    case N_RECVSTMT:
        check_recv(s->a, type_make(TY_NONE));
        break;

    default:
        sema_error(s->line, "internal error: unexpected %s statement", node_kind_name(s->kind));
        break;
    }
}

static void check_stmts(NodeList *stmts)
{
    for (int i = 0; i < stmts->count; i++)
        check_stmt(stmts->items[i]);
}

/* ------------------------------------------------------------------------ */
/*  Return analysis                                                         */
/* ------------------------------------------------------------------------ */

static int stmt_returns(const Node *s);

static int block_returns(const Node *b)
{
    for (int i = 0; i < b->list->count; i++)
        if (stmt_returns(b->list->items[i]))
            return 1;
    return 0;
}

/* True if the statement ends the function on every path. */
static int stmt_returns(const Node *s)
{
    if (s->kind == N_RETURN)
        return 1;
    if (s->kind == N_IF && s->c) {
        int else_ret = s->c->kind == N_IF ? stmt_returns(s->c) : block_returns(s->c);
        return block_returns(s->b) && else_ret;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Declarations                                                            */
/* ------------------------------------------------------------------------ */

static void check_protocol(Node *p)
{
    char b[64];
    for (int i = 0; i < p->list->count; i++) {
        Node *st = p->list->items[i];
        st->role = role_of(st->name);
        if (st->role == ROLE_NONE)
            sema_error(st->line, "protocol %s: unknown role '%s' (roles are client and server)",
                       p->name, st->name);
        if (st->ty.kind == TY_ARRAY && (st->ty.size <= 0 || st->ty.size > RABTA_MAX_ARRAY))
            sema_error(st->line, "protocol %s: invalid message type %s",
                       p->name, type_str(st->ty, b, sizeof b));
    }
}

static void check_callable(Node *fn)
{
    cur_fn = fn;
    nundeclared = 0;
    nsyms = 0;
    depth = 0;
    next_slot = 0;
    scope_push();

    for (int i = 0; i < fn->list->count; i++) {
        Node *p = fn->list->items[i];
        SymKind k = SYM_I64;
        Type t = type_make(TY_I64);

        if (p->pkind == PARAM_ARRAY) {
            k = SYM_ARRAY;
            t = type_array(-1);
            if (fn->kind == N_ACTOR)
                sema_error(p->line, "banda '%s' cannot take thali parameter '%s' "
                           "(actors share nothing; send the thali as a message)",
                           fn->name, p->name);
        } else if (p->pkind == PARAM_ENDPOINT) {
            k = SYM_ENDPOINT;
            t = type_make(TY_ENDPOINT);
            t.proto = find_protocol(program_root, p->name2);
            t.role = role_of(p->name3);
            if (fn->kind != N_ACTOR)
                sema_error(p->line, "sira parameter '%s' is only allowed on a banda, not on kaam '%s'",
                           p->name, fn->name);
            if (!t.proto) {
                sema_error(p->line, "unknown protocol '%s' for sira parameter '%s'", p->name2, p->name);
                t = type_make(TY_ERROR);
            } else if (t.role == ROLE_NONE) {
                sema_error(p->line, "'%s' is not a role of protocol %s (use client or server)",
                           p->name3, p->name2);
                t = type_make(TY_ERROR);
            }
            p->ref = t.proto;
            p->role = t.role;
        }
        p->ty = t;
        p->slot = declare(p, p->name, k, t);
    }

    check_stmts(fn->a->list);
    scope_pop();
    fn->nslots = next_slot;

    if (fn->kind == N_FUNC && !block_returns(fn->a))
        sema_error(fn->line, "%s can reach its end without waapis (every path must return an i64)",
                   where_name());
}

int semantic_check(Node *program)
{
    int before = error_total;
    program_root = program;

    /* 1. duplicate top-level names, protocol declarations */
    for (int i = 0; i < program->list->count; i++) {
        Node *it = program->list->items[i];
        for (int j = 0; j < i; j++) {
            Node *prev = program->list->items[j];
            if (prev->kind == it->kind && strcmp(prev->name, it->name) == 0) {
                const char *what = it->kind == N_PROTOCOL ? "baatcheet"
                                 : it->kind == N_ACTOR ? "banda" : "kaam";
                sema_error(it->line, "%s '%s' is already declared (line %d)",
                           what, it->name, prev->line);
                break;
            }
        }
        if (it->kind == N_PROTOCOL)
            check_protocol(it);
    }

    /* 2. main */
    Node *mainfn = find_function(program, "main");
    if (!mainfn)
        sema_error(1, "program has no entry point: declare kaam main() shuru ... khatam");
    else if (mainfn->list->count != 0)
        sema_error(mainfn->line, "main() must not take parameters");

    /* 3. bodies */
    for (int i = 0; i < program->list->count; i++) {
        Node *it = program->list->items[i];
        if (it->kind == N_FUNC || it->kind == N_ACTOR)
            check_callable(it);
    }

    cur_fn = NULL;
    free(syms);
    syms = NULL;
    nsyms = capsyms = 0;
    return error_total - before;
}
