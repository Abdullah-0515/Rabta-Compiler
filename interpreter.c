/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  interpreter.c - Tree-walking interpreter.
 *
 *  Execution model
 *    - Each call of a kaam and each banda gets a Frame: an array of Slots,
 *      one per variable, using the slot numbers assigned by semantic.c.
 *      There are no global variables, so actors share nothing.
 *    - i64 values live directly in the slot. A thali is a heap RArray owned
 *      by the frame that declared it; thali parameters of a kaam are passed
 *      by reference (the callee's slot points at the caller's array but does
 *      not own it).
 *    - nali stores an RChannel* in the slot; a sira parameter stores an
 *      REndpoint (channel + role).
 *    - Expressions are evaluated left to right; && and || short-circuit.
 *    - Arithmetic wraps around on overflow (two's complement), division or
 *      modulo by zero and out-of-bounds thali accesses are runtime errors.
 */
#include "interpreter.h"
#include "runtime.h"
#include "semantic.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef struct RArray {
    int64_t  len;
    int64_t *data;
} RArray;

typedef struct Slot {
    int64_t    i;       /* i64 value                                  */
    RArray    *arr;     /* thali                                      */
    int        owns;    /* this frame frees `arr` when it ends        */
    RChannel  *chan;    /* nali                                       */
    REndpoint  ep;      /* sira parameter                             */
} Slot;

typedef struct Frame {
    Slot    *slots;
    int      nslots;
    int64_t  retval;
} Frame;

typedef enum { FLOW_NEXT, FLOW_RETURN } Flow;

/* Arguments handed from paida to a new actor thread. */
typedef struct ActorStart {
    Node *actor;
    Slot *args;      /* one per parameter, in parameter order */
} ActorStart;

/* ------------------------------------------------------------------------ */
/*  Errors and memory                                                       */
/* ------------------------------------------------------------------------ */

static void runtime_error(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));

static void runtime_error(int line, const char *fmt, ...)
{
    va_list ap;
    rt_output_lock();
    fflush(stdout);
    fprintf(stderr, "%s:%d: runtime error: ", rabta_filename, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    exit(2);   /* terminates all actor threads as well */
}

static void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size);
    if (!p) {
        fprintf(stderr, "rabta: out of memory\n");
        exit(3);
    }
    return p;
}

static RArray *array_new(int64_t len)
{
    RArray *a = xcalloc(1, sizeof *a);
    a->len = len;
    a->data = xcalloc((size_t)len, sizeof(int64_t));
    return a;
}

static void array_free(RArray *a)
{
    if (a) {
        free(a->data);
        free(a);
    }
}

static Slot *frame_alloc(int nslots)
{
    return xcalloc((size_t)nslots, sizeof(Slot));
}

static void frame_release(Frame *f)
{
    for (int i = 0; i < f->nslots; i++)
        if (f->slots[i].owns)
            array_free(f->slots[i].arr);
    free(f->slots);
}

/* ------------------------------------------------------------------------ */
/*  Expressions                                                             */
/* ------------------------------------------------------------------------ */

static int64_t eval(Node *e, Frame *f);
static Flow exec_block(Node *block, Frame *f);

static int64_t call_function(Node *fn, NodeList *args, Frame *caller, int line);

static int64_t check_index(Node *at, RArray *a, int64_t idx, const char *name)
{
    if (idx < 0 || idx >= a->len)
        runtime_error(at->line, "index %" PRId64 " is out of bounds for thali '%s' of size %" PRId64,
                      idx, name, a->len);
    return idx;
}

static int64_t eval_binop(Node *e, Frame *f)
{
    /* short-circuit logic first */
    if (e->op == OP_AND)
        return eval(e->a, f) ? (eval(e->b, f) != 0) : 0;
    if (e->op == OP_OR)
        return eval(e->a, f) ? 1 : (eval(e->b, f) != 0);

    int64_t l = eval(e->a, f);
    int64_t r = eval(e->b, f);
    uint64_t ul = (uint64_t)l, ur = (uint64_t)r;

    switch (e->op) {
    case OP_ADD: return (int64_t)(ul + ur);
    case OP_SUB: return (int64_t)(ul - ur);
    case OP_MUL: return (int64_t)(ul * ur);
    case OP_DIV:
        if (r == 0)
            runtime_error(e->line, "division by zero");
        if (l == INT64_MIN && r == -1)
            return INT64_MIN;
        return l / r;
    case OP_MOD:
        if (r == 0)
            runtime_error(e->line, "modulo by zero");
        if (l == INT64_MIN && r == -1)
            return 0;
        return l % r;
    case OP_EQ: return l == r;
    case OP_NE: return l != r;
    case OP_LT: return l < r;
    case OP_LE: return l <= r;
    case OP_GT: return l > r;
    case OP_GE: return l >= r;
    default:
        runtime_error(e->line, "internal error: bad operator");
    }
}

static int64_t eval(Node *e, Frame *f)
{
    switch (e->kind) {
    case N_INT:
        return e->ival;
    case N_VAR:
        return f->slots[e->slot].i;
    case N_INDEX: {
        RArray *a = f->slots[e->slot].arr;
        int64_t idx = check_index(e, a, eval(e->a, f), e->name);
        return a->data[idx];
    }
    case N_BINOP:
        return eval_binop(e, f);
    case N_UNOP: {
        int64_t v = eval(e->a, f);
        if (e->op == OP_NEG)
            return (int64_t)(0u - (uint64_t)v);
        return !v;
    }
    case N_CALL:
        return call_function(e->ref, e->list, f, e->line);
    default:
        runtime_error(e->line, "internal error: cannot evaluate %s", node_kind_name(e->kind));
    }
}

/* ------------------------------------------------------------------------ */
/*  Functions and actors                                                    */
/* ------------------------------------------------------------------------ */

static int64_t call_function(Node *fn, NodeList *args, Frame *caller, int line)
{
    Frame fr;
    fr.nslots = fn->nslots;
    fr.slots = frame_alloc(fn->nslots);
    fr.retval = 0;

    for (int i = 0; i < fn->list->count; i++) {
        Node *p = fn->list->items[i];
        Node *a = args->items[i];
        if (p->pkind == PARAM_ARRAY) {
            fr.slots[p->slot].arr = caller->slots[a->slot].arr;   /* by reference */
            fr.slots[p->slot].owns = 0;
        } else {
            fr.slots[p->slot].i = eval(a, caller);
        }
    }

    Flow fl = exec_block(fn->a, &fr);
    if (fl != FLOW_RETURN)
        runtime_error(line, "kaam '%s' ended without waapis", fn->name);
    int64_t rv = fr.retval;
    frame_release(&fr);
    return rv;
}

static REndpoint endpoint_value(Node *ep, Frame *f)
{
    REndpoint e;
    if (ep->kind == N_ENDPOINT) {
        e.chan = f->slots[ep->slot].chan;
        e.role = ep->role;
    } else {
        e = f->slots[ep->slot].ep;
    }
    return e;
}

static void *actor_main(void *arg)
{
    ActorStart *st = arg;
    Node *actor = st->actor;
    Frame fr;

    fr.nslots = actor->nslots;
    fr.slots = frame_alloc(actor->nslots);
    fr.retval = 0;
    for (int i = 0; i < actor->list->count; i++) {
        Node *p = actor->list->items[i];
        fr.slots[p->slot] = st->args[i];
    }
    free(st->args);
    free(st);

    exec_block(actor->a, &fr);
    frame_release(&fr);
    return NULL;
}

static void spawn_actor(Node *s, Frame *f)
{
    Node *actor = s->ref;
    ActorStart *st = xcalloc(1, sizeof *st);
    st->actor = actor;
    st->args = xcalloc((size_t)actor->list->count, sizeof(Slot));

    for (int i = 0; i < actor->list->count; i++) {
        Node *p = actor->list->items[i];
        Node *a = s->list->items[i];
        if (p->pkind == PARAM_ENDPOINT)
            st->args[i].ep = endpoint_value(a, f);
        else
            st->args[i].i = eval(a, f);    /* values are copied: no sharing */
    }
    rt_spawn(actor_main, st);
}

/* ------------------------------------------------------------------------ */
/*  Messages                                                                */
/* ------------------------------------------------------------------------ */

static void do_send(Node *s, Frame *f)
{
    REndpoint ep = endpoint_value(s->a, f);
    RMsg m;
    memset(&m, 0, sizeof m);

    if (s->b->ty.kind == TY_ARRAY) {
        RArray *a = f->slots[s->b->slot].arr;
        m.is_array = 1;
        m.len = a->len;
        m.data = xcalloc((size_t)a->len, sizeof(int64_t));
        memcpy(m.data, a->data, (size_t)a->len * sizeof(int64_t));   /* deep copy */
    } else {
        m.value = eval(s->b, f);
    }
    rt_send(ep, m);
}

static RMsg do_recv(Node *recv, Frame *f)
{
    return rt_recv(endpoint_value(recv->a, f));
}

/* Store a received message into an existing thali (sizes checked statically). */
static void recv_into_array(Node *at, RArray *dst, RMsg m)
{
    if (!m.is_array || m.len != dst->len)
        runtime_error(at->line, "received message does not match thali of size %" PRId64, dst->len);
    memcpy(dst->data, m.data, (size_t)m.len * sizeof(int64_t));
    free(m.data);
}

static int64_t recv_scalar(Node *at, RMsg m)
{
    if (m.is_array) {
        free(m.data);
        runtime_error(at->line, "received a thali where an i64 was expected");
    }
    return m.value;
}

/* ------------------------------------------------------------------------ */
/*  Output                                                                  */
/* ------------------------------------------------------------------------ */

static void do_print(Node *s, Frame *f)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *out = open_memstream(&buf, &len);
    if (!out) {
        fprintf(stderr, "rabta: out of memory\n");
        exit(3);
    }

    /* Evaluate everything first (a bulao inside dikhao may print too). */
    for (int i = 0; i < s->list->count; i++) {
        Node *a = s->list->items[i];
        if (i > 0)
            fputc(' ', out);
        if (a->kind == N_STR) {
            fputs(a->str, out);
        } else if (a->ty.kind == TY_ARRAY) {
            RArray *arr = f->slots[a->slot].arr;
            fputc('[', out);
            for (int64_t k = 0; k < arr->len; k++)
                fprintf(out, k ? ", %" PRId64 : "%" PRId64, arr->data[k]);
            fputc(']', out);
        } else {
            fprintf(out, "%" PRId64, eval(a, f));
        }
    }
    fputc('\n', out);
    fclose(out);

    rt_output_lock();
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    rt_output_unlock();
    free(buf);
}

/* ------------------------------------------------------------------------ */
/*  Statements                                                              */
/* ------------------------------------------------------------------------ */

static Flow exec_stmt(Node *s, Frame *f);

static Flow exec_stmts(NodeList *l, Frame *f)
{
    for (int i = 0; i < l->count; i++)
        if (exec_stmt(l->items[i], f) == FLOW_RETURN)
            return FLOW_RETURN;
    return FLOW_NEXT;
}

static Flow exec_block(Node *block, Frame *f)
{
    return exec_stmts(block->list, f);
}

static Flow exec_stmt(Node *s, Frame *f)
{
    Slot *sl;

    switch (s->kind) {
    case N_VARDECL:
        sl = &f->slots[s->slot];
        sl->i = s->a->kind == N_RECV ? recv_scalar(s, do_recv(s->a, f)) : eval(s->a, f);
        return FLOW_NEXT;

    case N_ARRDECL:
        sl = &f->slots[s->slot];
        if (sl->owns)
            array_free(sl->arr);           /* re-executed inside a loop */
        sl->arr = array_new(s->ival);
        sl->owns = 1;
        if (s->a && s->a->kind == N_ARRLIT) {
            NodeList *el = s->a->list;
            for (int i = 0; i < el->count; i++)
                sl->arr->data[i] = eval(el->items[i], f);
        } else if (s->a && s->a->kind == N_RECV) {
            recv_into_array(s, sl->arr, do_recv(s->a, f));
        }
        return FLOW_NEXT;

    case N_ASSIGN:
        sl = &f->slots[s->slot];
        if (s->a->kind == N_RECV) {
            RMsg m = do_recv(s->a, f);
            if (s->ty.kind == TY_ARRAY)
                recv_into_array(s, sl->arr, m);
            else
                sl->i = recv_scalar(s, m);
        } else {
            sl->i = eval(s->a, f);
        }
        return FLOW_NEXT;

    case N_IDXASSIGN: {
        RArray *a = f->slots[s->slot].arr;
        int64_t idx = eval(s->a, f);
        int64_t v = s->b->kind == N_RECV ? recv_scalar(s, do_recv(s->b, f)) : eval(s->b, f);
        a->data[check_index(s, a, idx, s->name)] = v;
        return FLOW_NEXT;
    }

    case N_IF:
        if (eval(s->a, f))
            return exec_block(s->b, f);
        if (s->c)
            return s->c->kind == N_IF ? exec_stmt(s->c, f) : exec_block(s->c, f);
        return FLOW_NEXT;

    case N_WHILE:
        while (eval(s->a, f))
            if (exec_block(s->b, f) == FLOW_RETURN)
                return FLOW_RETURN;
        return FLOW_NEXT;

    case N_FOR: {
        int64_t from = eval(s->a, f);
        int64_t to = eval(s->b, f);          /* end bound is exclusive */
        sl = &f->slots[s->slot];
        for (sl->i = from; sl->i < to; sl->i++)
            if (exec_block(s->c, f) == FLOW_RETURN)
                return FLOW_RETURN;
        return FLOW_NEXT;
    }

    case N_RETURN:
        f->retval = eval(s->a, f);
        return FLOW_RETURN;

    case N_PRINT:
        do_print(s, f);
        return FLOW_NEXT;

    case N_CALLSTMT:
        (void)eval(s->a, f);
        return FLOW_NEXT;

    case N_CHANDECL:
        f->slots[s->slot].chan = rt_channel_new(s->name2);
        return FLOW_NEXT;

    case N_SPAWN:
        spawn_actor(s, f);
        return FLOW_NEXT;

    case N_SEND:
        do_send(s, f);
        return FLOW_NEXT;

    case N_RECVSTMT: {
        RMsg m = do_recv(s->a, f);
        free(m.data);
        return FLOW_NEXT;
    }

    default:
        runtime_error(s->line, "internal error: cannot execute %s", node_kind_name(s->kind));
    }
}

/* ------------------------------------------------------------------------ */
/*  Entry point                                                             */
/* ------------------------------------------------------------------------ */

int interpret(Node *program)
{
    Node *mainfn = find_function(program, "main");
    NodeList none = { NULL, 0, 0 };

    rt_init();
    int64_t rv = call_function(mainfn, &none, NULL, mainfn->line);
    fflush(stdout);
    rt_join_all();          /* main is done: wait for every actor to finish */
    rt_shutdown();
    return (int)(rv & 0xff);
}
