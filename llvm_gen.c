/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  llvm_gen.c - Code generator core.
 *
 *    - creates the LLVMContext, LLVMModule and LLVMBuilder
 *    - maps Rabta types to LLVM types (i64 -> i64, thali -> ptr + i64)
 *    - declares the libc functions the generated code calls
 *    - frame slots: one alloca per variable in the entry block
 *    - rakho, assignment, thali declaration / indexing (bounds checked)
 *    - dikhao (one output line, written atomically)
 *    - runtime error helpers (@rabta.fail, @rabta.index_fail)
 *    - dispatch of statements and expressions to llvm_flow.c,
 *      llvm_func.c and llvm_actor.c
 *    - llvm_generate(): build, verify and print the whole module
 */
#include "llvm_gen.h"
#include "llvm_gen_priv.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/*  Small helpers                                                           */
/* ------------------------------------------------------------------------ */

static void *gen_xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size);
    if (!p) {
        fprintf(stderr, "rabtac: out of memory\n");
        exit(3);
    }
    return p;
}

LLVMValueRef gen_i64(Gen *g, int64_t v)
{
    return LLVMConstInt(g->t_i64, (unsigned long long)v, 1);
}

LLVMValueRef gen_i32(Gen *g, int v)
{
    return LLVMConstInt(g->t_i32, (unsigned long long)(int64_t)v, 1);
}

/* A private, constant, NUL-terminated string; its value is a ptr. */
LLVMValueRef gen_cstr(Gen *g, const char *s)
{
    LLVMValueRef init = LLVMConstStringInContext(g->ctx, s, (unsigned)strlen(s), 0);
    LLVMValueRef gv = LLVMAddGlobal(g->mod, LLVMTypeOf(init), ".str");
    LLVMSetInitializer(gv, init);
    LLVMSetGlobalConstant(gv, 1);
    LLVMSetLinkage(gv, LLVMPrivateLinkage);
    LLVMSetUnnamedAddress(gv, LLVMGlobalUnnamedAddr);
    LLVMSetAlignment(gv, 1);
    return gv;
}

LLVMValueRef gen_call(Gen *g, LLVMValueRef fn, LLVMValueRef *args, unsigned n, const char *name)
{
    LLVMTypeRef ft = LLVMGlobalGetValueType(fn);
    if (LLVMGetReturnType(ft) == g->t_void)
        name = "";
    return LLVMBuildCall2(g->b, ft, fn, args, n, name);
}

LLVMBasicBlockRef gen_block(Gen *g, const char *name)
{
    return LLVMAppendBasicBlockInContext(g->ctx, g->fn, name);
}

int gen_is_terminated(Gen *g)
{
    return LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(g->b)) != NULL;
}

static void add_fn_attr(Gen *g, LLVMValueRef fn, const char *attr)
{
    unsigned kind = LLVMGetEnumAttributeKindForName(attr, strlen(attr));
    LLVMAddAttributeAtIndex(fn, LLVMAttributeFunctionIndex,
                               LLVMCreateEnumAttribute(g->ctx, kind, 0));
}

/* ------------------------------------------------------------------------ */
/*  libc / POSIX declarations                                               */
/* ------------------------------------------------------------------------ */

/*
 * Signature codes: v = void, i = i32, l = i64, p = ptr.
 * The first character is the return type, the rest are the parameters.
 */
static const struct {
    const char *name;
    const char *sig;
    int         vararg;
} libc_table[] = {
    { "calloc",                    "pll",   0 },
    { "malloc",                    "pl",    0 },
    { "realloc",                   "ppl",   0 },
    { "free",                      "vp",    0 },
    { "memcpy",                    "pppl",  0 },
    { "exit",                      "vi",    0 },
    { "fprintf",                   "ipp",   1 },
    { "dprintf",                   "iip",   1 },
    { "fputc",                     "iip",   0 },
    { "fputs",                     "ipp",   0 },
    { "fwrite",                    "lpllp", 0 },
    { "fflush",                    "ip",    0 },
    { "fclose",                    "ip",    0 },
    { "open_memstream",            "ppp",   0 },
    { "flockfile",                 "vp",    0 },
    { "funlockfile",               "vp",    0 },
    { "pthread_create",            "ipppp", 0 },
    { "pthread_join",              "ilp",   0 },
    { "pthread_attr_init",         "ip",    0 },
    { "pthread_attr_setstacksize", "ipl",   0 },
    { "pthread_attr_destroy",      "ip",    0 },
    { "pthread_mutex_init",        "ipp",   0 },
    { "pthread_mutex_lock",        "ip",    0 },
    { "pthread_mutex_unlock",      "ip",    0 },
    { "pthread_mutex_destroy",     "ip",    0 },
    { "pthread_cond_init",         "ipp",   0 },
    { "pthread_cond_destroy",      "ip",    0 },
    { "pthread_cond_wait",         "ipp",   0 },
    { "pthread_cond_signal",       "ip",    0 },
};

static LLVMTypeRef sig_type(Gen *g, char c)
{
    switch (c) {
    case 'v': return g->t_void;
    case 'i': return g->t_i32;
    case 'l': return g->t_i64;
    default:  return g->t_ptr;
    }
}

/* Return the declaration of a libc function, adding it on first use. */
LLVMValueRef gen_libc(Gen *g, const char *name)
{
    LLVMValueRef fn = LLVMGetNamedFunction(g->mod, name);
    if (fn)
        return fn;

    for (size_t i = 0; i < sizeof libc_table / sizeof libc_table[0]; i++) {
        if (strcmp(libc_table[i].name, name) != 0)
            continue;
        const char *sig = libc_table[i].sig;
        LLVMTypeRef params[8];
        unsigned n = 0;
        for (const char *c = sig + 1; *c; c++)
            params[n++] = sig_type(g, *c);
        LLVMTypeRef ft = LLVMFunctionType(sig_type(g, sig[0]), params, n, libc_table[i].vararg);
        fn = LLVMAddFunction(g->mod, name, ft);
        if (strcmp(name, "exit") == 0)
            add_fn_attr(g, fn, "noreturn");
        return fn;
    }
    fprintf(stderr, "rabtac: internal error: unknown libc function %s\n", name);
    exit(3);
}

/* `@stdout` is glibc's `FILE *stdout`. */
LLVMValueRef gen_load_stdout(Gen *g)
{
    LLVMValueRef gv = LLVMGetNamedGlobal(g->mod, "stdout");
    if (!gv) {
        gv = LLVMAddGlobal(g->mod, g->t_ptr, "stdout");
        LLVMSetLinkage(gv, LLVMExternalLinkage);
    }
    return LLVMBuildLoad2(g->b, g->t_ptr, gv, "stdout");
}

/*
 * Start a new internal runtime function and position the builder in its
 * entry block. Runtime functions are generated before any user code.
 */
LLVMValueRef gen_new_runtime_fn(Gen *g, const char *name, LLVMTypeRef ret,
                                LLVMTypeRef *params, unsigned n)
{
    LLVMValueRef fn = LLVMAddFunction(g->mod, name, LLVMFunctionType(ret, params, n, 0));
    LLVMSetLinkage(fn, LLVMInternalLinkage);
    add_fn_attr(g, fn, "nounwind");
    g->fn = fn;
    LLVMPositionBuilderAtEnd(g->b, gen_block(g, "entry"));
    return fn;
}

/* ------------------------------------------------------------------------ */
/*  Runtime errors and output (generated in IR)                             */
/* ------------------------------------------------------------------------ */

/*
 * Before reporting a runtime error the failing thread takes the stdout lock
 * (so no other actor is in the middle of a line), flushes stdout, writes
 * the message to stderr and exits with status 2, which ends every actor.
 */
static void gen_fail_prologue(Gen *g)
{
    LLVMValueRef out = gen_load_stdout(g);
    gen_call(g, gen_libc(g, "flockfile"), &out, 1, "");
    gen_call(g, gen_libc(g, "fflush"), &out, 1, "");
}

static void gen_fail_epilogue(Gen *g)
{
    LLVMValueRef two = gen_i32(g, 2);
    gen_call(g, gen_libc(g, "exit"), &two, 1, "");
    LLVMBuildUnreachable(g->b);
}

static void gen_core_runtime(Gen *g)
{
    /* void @rabta.fail(i32 line, ptr msg) */
    {
        LLVMTypeRef ps[] = { g->t_i32, g->t_ptr };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.fail", g->t_void, ps, 2);
        add_fn_attr(g, fn, "noreturn");
        add_fn_attr(g, fn, "cold");
        gen_fail_prologue(g);
        LLVMValueRef args[] = {
            gen_i32(g, 2), gen_cstr(g, "%s:%d: runtime error: %s\n"),
            g->g_source, LLVMGetParam(fn, 0), LLVMGetParam(fn, 1)
        };
        gen_call(g, gen_libc(g, "dprintf"), args, 5, "");
        gen_fail_epilogue(g);
    }

    /* void @rabta.index_fail(i32 line, i64 index, ptr name, i64 len) */
    {
        LLVMTypeRef ps[] = { g->t_i32, g->t_i64, g->t_ptr, g->t_i64 };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.index_fail", g->t_void, ps, 4);
        add_fn_attr(g, fn, "noreturn");
        add_fn_attr(g, fn, "cold");
        gen_fail_prologue(g);
        LLVMValueRef args[] = {
            gen_i32(g, 2),
            gen_cstr(g, "%s:%d: runtime error: index %ld is out of bounds for thali '%s' of size %ld\n"),
            g->g_source, LLVMGetParam(fn, 0), LLVMGetParam(fn, 1),
            LLVMGetParam(fn, 2), LLVMGetParam(fn, 3)
        };
        gen_call(g, gen_libc(g, "dprintf"), args, 7, "");
        gen_fail_epilogue(g);
    }

    /* void @rabta.print_array(ptr file, ptr data, i64 len)  ->  [a, b, c] */
    {
        LLVMTypeRef ps[] = { g->t_ptr, g->t_ptr, g->t_i64 };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.print_array", g->t_void, ps, 3);
        LLVMValueRef f = LLVMGetParam(fn, 0), data = LLVMGetParam(fn, 1), len = LLVMGetParam(fn, 2);
        LLVMBasicBlockRef entry = LLVMGetInsertBlock(g->b);
        LLVMBasicBlockRef cond = gen_block(g, "cond");
        LLVMBasicBlockRef body = gen_block(g, "body");
        LLVMBasicBlockRef done = gen_block(g, "done");
        LLVMValueRef fmt_first = gen_cstr(g, "%ld");
        LLVMValueRef fmt_next = gen_cstr(g, ", %ld");

        LLVMValueRef a1[] = { gen_i32(g, '['), f };
        gen_call(g, gen_libc(g, "fputc"), a1, 2, "");
        LLVMBuildBr(g->b, cond);

        LLVMPositionBuilderAtEnd(g->b, cond);
        LLVMValueRef k = LLVMBuildPhi(g->b, g->t_i64, "k");
        LLVMValueRef more = LLVMBuildICmp(g->b, LLVMIntSLT, k, len, "more");
        LLVMBuildCondBr(g->b, more, body, done);

        LLVMPositionBuilderAtEnd(g->b, body);
        LLVMValueRef ep = LLVMBuildGEP2(g->b, g->t_i64, data, &k, 1, "elem.ptr");
        LLVMValueRef ev = LLVMBuildLoad2(g->b, g->t_i64, ep, "elem");
        LLVMValueRef first = LLVMBuildICmp(g->b, LLVMIntEQ, k, gen_i64(g, 0), "first");
        LLVMValueRef fmt = LLVMBuildSelect(g->b, first, fmt_first, fmt_next, "fmt");
        LLVMValueRef a2[] = { f, fmt, ev };
        gen_call(g, gen_libc(g, "fprintf"), a2, 3, "");
        LLVMValueRef k1 = LLVMBuildAdd(g->b, k, gen_i64(g, 1), "k.next");
        LLVMBuildBr(g->b, cond);

        LLVMValueRef in_v[] = { gen_i64(g, 0), k1 };
        LLVMBasicBlockRef in_b[] = { entry, body };
        LLVMAddIncoming(k, in_v, in_b, 2);

        LLVMPositionBuilderAtEnd(g->b, done);
        LLVMValueRef a3[] = { gen_i32(g, ']'), f };
        gen_call(g, gen_libc(g, "fputc"), a3, 2, "");
        LLVMBuildRetVoid(g->b);
    }

    /*
     * void @rabta.emit(ptr buf, i64 len): write one finished dikhao line to
     * stdout while holding the stdout lock, so lines printed by different
     * actors never interleave; then free the buffer.
     */
    {
        LLVMTypeRef ps[] = { g->t_ptr, g->t_i64 };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.emit", g->t_void, ps, 2);
        LLVMValueRef buf = LLVMGetParam(fn, 0), len = LLVMGetParam(fn, 1);
        LLVMValueRef out = gen_load_stdout(g);
        gen_call(g, gen_libc(g, "flockfile"), &out, 1, "");
        LLVMValueRef wa[] = { buf, gen_i64(g, 1), len, out };
        gen_call(g, gen_libc(g, "fwrite"), wa, 4, "");
        gen_call(g, gen_libc(g, "fflush"), &out, 1, "");
        gen_call(g, gen_libc(g, "funlockfile"), &out, 1, "");
        gen_call(g, gen_libc(g, "free"), &buf, 1, "");
        LLVMBuildRetVoid(g->b);
    }
}

/* Call @rabta.fail(line, msg) and end the current block. */
void gen_runtime_fail(Gen *g, int line, const char *msg)
{
    LLVMValueRef args[] = { gen_i32(g, line), gen_cstr(g, msg) };
    gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.fail"), args, 2, "");
    LLVMBuildUnreachable(g->b);
}

/* ------------------------------------------------------------------------ */
/*  Frame slots                                                             */
/* ------------------------------------------------------------------------ */

LLVMValueRef gen_entry_alloca(Gen *g, LLVMTypeRef t, const char *name)
{
    LLVMPositionBuilderBefore(g->ab, g->entry_br);
    return LLVMBuildAlloca(g->ab, t, name);
}

static const char *slot_name(Gen *g, int slot)
{
    return g->slot_names[slot] ? g->slot_names[slot] : "tmp";
}

LLVMValueRef slot_i(Gen *g, int slot)
{
    GenSlot *s = &g->slots[slot];
    if (!s->i)
        s->i = gen_entry_alloca(g, g->t_i64, slot_name(g, slot));
    return s->i;
}

LLVMValueRef slot_arr(Gen *g, int slot)
{
    GenSlot *s = &g->slots[slot];
    if (!s->arr) {
        char name[128];
        snprintf(name, sizeof name, "%s.data", slot_name(g, slot));
        s->arr = gen_entry_alloca(g, g->t_ptr, name);
        /* start as NULL so free() before the first declaration is safe */
        LLVMBuildStore(g->ab, LLVMConstPointerNull(g->t_ptr), s->arr);
    }
    return s->arr;
}

LLVMValueRef slot_len(Gen *g, int slot)
{
    GenSlot *s = &g->slots[slot];
    if (!s->len) {
        char name[128];
        snprintf(name, sizeof name, "%s.len", slot_name(g, slot));
        s->len = gen_entry_alloca(g, g->t_i64, name);
        LLVMBuildStore(g->ab, gen_i64(g, 0), s->len);
    }
    return s->len;
}

LLVMValueRef slot_chan(Gen *g, int slot)
{
    GenSlot *s = &g->slots[slot];
    if (!s->chan) {
        char name[128];
        snprintf(name, sizeof name, "%s.chan", slot_name(g, slot));
        s->chan = gen_entry_alloca(g, g->t_ptr, name);
    }
    return s->chan;
}

/* Record the variable name of every slot and the slots of owned thali. */
static void collect_slots(Gen *g, Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case N_PARAM: case N_VARDECL: case N_FOR: case N_CHANDECL:
        if (n->slot >= 0 && n->slot < g->nslots)
            g->slot_names[n->slot] = n->name;
        break;
    case N_ARRDECL:
        if (n->slot >= 0 && n->slot < g->nslots) {
            g->slot_names[n->slot] = n->name;
            g->owned[g->nowned++] = n->slot;
        }
        break;
    default:
        break;
    }
    if (n->kind == N_BLOCK || n->kind == N_IF || n->kind == N_WHILE || n->kind == N_FOR) {
        collect_slots(g, n->a);
        collect_slots(g, n->b);
        collect_slots(g, n->c);
        if (n->list)
            for (int i = 0; i < n->list->count; i++)
                collect_slots(g, n->list->items[i]);
    }
}

/*
 * Prepare code generation for a kaam / banda body:
 *   entry:  allocas ... ; br label %body
 *   body:   (the builder is left here)
 */
void gen_begin_body(Gen *g, Node *fn_node, LLVMValueRef fn)
{
    g->cur = fn_node;
    g->fn = fn;
    g->nslots = fn_node->nslots;
    g->slots = gen_xcalloc((size_t)g->nslots, sizeof(GenSlot));
    g->slot_names = gen_xcalloc((size_t)g->nslots, sizeof(char *));
    g->owned = gen_xcalloc((size_t)g->nslots, sizeof(int));
    g->nowned = 0;

    for (int i = 0; i < fn_node->list->count; i++)
        collect_slots(g, fn_node->list->items[i]);
    collect_slots(g, fn_node->a);

    LLVMBasicBlockRef entry = gen_block(g, "entry");
    LLVMBasicBlockRef body = gen_block(g, "body");
    LLVMPositionBuilderAtEnd(g->b, entry);
    g->entry_br = LLVMBuildBr(g->b, body);
    LLVMPositionBuilderAtEnd(g->b, body);
}

void gen_end_body(Gen *g)
{
    free(g->slots);
    free(g->slot_names);
    free(g->owned);
    g->slots = NULL;
    g->slot_names = NULL;
    g->owned = NULL;
    g->nslots = g->nowned = 0;
    g->cur = NULL;
}

/* free() every thali this body declared (called before each return). */
void gen_free_owned(Gen *g)
{
    for (int i = 0; i < g->nowned; i++) {
        LLVMValueRef p = LLVMBuildLoad2(g->b, g->t_ptr, slot_arr(g, g->owned[i]), "old");
        gen_call(g, gen_libc(g, "free"), &p, 1, "");
    }
}

/* ------------------------------------------------------------------------ */
/*  Variables and thali                                                     */
/* ------------------------------------------------------------------------ */

/* Address of name[idx] after a bounds check (0 <= idx < len). */
static LLVMValueRef gen_elem_ptr(Gen *g, int slot, const char *name, LLVMValueRef idx, int line)
{
    LLVMValueRef data = LLVMBuildLoad2(g->b, g->t_ptr, slot_arr(g, slot), "data");
    LLVMValueRef len = LLVMBuildLoad2(g->b, g->t_i64, slot_len(g, slot), "len");
    /* one unsigned compare catches both idx < 0 and idx >= len */
    LLVMValueRef bad = LLVMBuildICmp(g->b, LLVMIntUGE, idx, len, "out.of.bounds");
    LLVMBasicBlockRef fail = gen_block(g, "index.fail");
    LLVMBasicBlockRef ok = gen_block(g, "index.ok");
    LLVMBuildCondBr(g->b, bad, fail, ok);

    LLVMPositionBuilderAtEnd(g->b, fail);
    LLVMValueRef args[] = { gen_i32(g, line), idx, gen_cstr(g, name), len };
    gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.index_fail"), args, 4, "");
    LLVMBuildUnreachable(g->b);

    LLVMPositionBuilderAtEnd(g->b, ok);
    return LLVMBuildGEP2(g->b, g->t_i64, data, &idx, 1, "elem.ptr");
}

/* thali name[N] (= [...] | = suno ep)? */
static void gen_arrdecl(Gen *g, Node *s)
{
    LLVMValueRef arr = slot_arr(g, s->slot);

    /* re-executed inside a loop: release the previous array first */
    LLVMValueRef old = LLVMBuildLoad2(g->b, g->t_ptr, arr, "old");
    gen_call(g, gen_libc(g, "free"), &old, 1, "");

    LLVMValueRef n = gen_i64(g, s->ival);
    LLVMValueRef ca[] = { n, gen_i64(g, 8) };
    LLVMValueRef data = gen_call(g, gen_libc(g, "calloc"), ca, 2, s->name);

    LLVMValueRef isnull = LLVMBuildIsNull(g->b, data, "oom");
    LLVMBasicBlockRef fail = gen_block(g, "oom.fail");
    LLVMBasicBlockRef ok = gen_block(g, "oom.ok");
    LLVMBuildCondBr(g->b, isnull, fail, ok);
    LLVMPositionBuilderAtEnd(g->b, fail);
    gen_runtime_fail(g, s->line, "out of memory");
    LLVMPositionBuilderAtEnd(g->b, ok);

    LLVMBuildStore(g->b, data, arr);
    LLVMBuildStore(g->b, n, slot_len(g, s->slot));

    if (s->a && s->a->kind == N_ARRLIT) {
        NodeList *el = s->a->list;
        for (int i = 0; i < el->count; i++) {
            LLVMValueRef v = gen_expr(g, el->items[i]);
            LLVMValueRef idx = gen_i64(g, i);
            LLVMValueRef p = LLVMBuildGEP2(g->b, g->t_i64, data, &idx, 1, "init.ptr");
            LLVMBuildStore(g->b, v, p);
        }
    } else if (s->a && s->a->kind == N_RECV) {
        gen_recv_into(g, s->a, data, n);
    }
}

static void gen_assign(Gen *g, Node *s)
{
    if (s->a->kind == N_RECV) {
        if (s->ty.kind == TY_ARRAY) {
            LLVMValueRef data = LLVMBuildLoad2(g->b, g->t_ptr, slot_arr(g, s->slot), "data");
            LLVMValueRef len = LLVMBuildLoad2(g->b, g->t_i64, slot_len(g, s->slot), "len");
            gen_recv_into(g, s->a, data, len);
        } else {
            LLVMBuildStore(g->b, gen_recv_i64(g, s->a), slot_i(g, s->slot));
        }
        return;
    }
    LLVMBuildStore(g->b, gen_expr(g, s->a), slot_i(g, s->slot));
}

static void gen_idxassign(Gen *g, Node *s)
{
    LLVMValueRef idx = gen_expr(g, s->a);
    LLVMValueRef v = s->b->kind == N_RECV ? gen_recv_i64(g, s->b) : gen_expr(g, s->b);
    LLVMBuildStore(g->b, v, gen_elem_ptr(g, s->slot, s->name, idx, s->line));
}

/* ------------------------------------------------------------------------ */
/*  dikhao                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * The line is first formatted into a private buffer (open_memstream), in
 * argument order, exactly like the Phase 1 interpreter did, and then
 * written to stdout in one piece by @rabta.emit.
 */
static void gen_print(Gen *g, Node *s)
{
    LLVMValueRef bufp = gen_entry_alloca(g, g->t_ptr, "line.buf");
    LLVMValueRef lenp = gen_entry_alloca(g, g->t_i64, "line.len");
    LLVMValueRef oa[] = { bufp, lenp };
    LLVMValueRef f = gen_call(g, gen_libc(g, "open_memstream"), oa, 2, "line");

    LLVMValueRef isnull = LLVMBuildIsNull(g->b, f, "oom");
    LLVMBasicBlockRef fail = gen_block(g, "oom.fail");
    LLVMBasicBlockRef ok = gen_block(g, "oom.ok");
    LLVMBuildCondBr(g->b, isnull, fail, ok);
    LLVMPositionBuilderAtEnd(g->b, fail);
    gen_runtime_fail(g, s->line, "out of memory");
    LLVMPositionBuilderAtEnd(g->b, ok);

    for (int i = 0; i < s->list->count; i++) {
        Node *a = s->list->items[i];
        if (i > 0) {
            LLVMValueRef sp[] = { gen_i32(g, ' '), f };
            gen_call(g, gen_libc(g, "fputc"), sp, 2, "");
        }
        if (a->kind == N_STR) {
            LLVMValueRef pa[] = { gen_cstr(g, a->str), f };
            gen_call(g, gen_libc(g, "fputs"), pa, 2, "");
        } else if (a->ty.kind == TY_ARRAY) {
            LLVMValueRef pa[] = {
                f,
                LLVMBuildLoad2(g->b, g->t_ptr, slot_arr(g, a->slot), "data"),
                LLVMBuildLoad2(g->b, g->t_i64, slot_len(g, a->slot), "len"),
            };
            gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.print_array"), pa, 3, "");
        } else {
            LLVMValueRef v = gen_expr(g, a);
            LLVMValueRef pa[] = { f, gen_cstr(g, "%ld"), v };
            gen_call(g, gen_libc(g, "fprintf"), pa, 3, "");
        }
    }
    LLVMValueRef nl[] = { gen_i32(g, '\n'), f };
    gen_call(g, gen_libc(g, "fputc"), nl, 2, "");
    gen_call(g, gen_libc(g, "fclose"), &f, 1, "");

    LLVMValueRef ea[] = {
        LLVMBuildLoad2(g->b, g->t_ptr, bufp, "buf"),
        LLVMBuildLoad2(g->b, g->t_i64, lenp, "len"),
    };
    gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.emit"), ea, 2, "");
}

/* ------------------------------------------------------------------------ */
/*  Dispatch                                                                */
/* ------------------------------------------------------------------------ */

LLVMValueRef gen_expr(Gen *g, Node *e)
{
    switch (e->kind) {
    case N_INT:
        return gen_i64(g, e->ival);
    case N_VAR:
        return LLVMBuildLoad2(g->b, g->t_i64, slot_i(g, e->slot), e->name);
    case N_INDEX: {
        LLVMValueRef idx = gen_expr(g, e->a);
        LLVMValueRef p = gen_elem_ptr(g, e->slot, e->name, idx, e->line);
        return LLVMBuildLoad2(g->b, g->t_i64, p, "elem");
    }
    case N_BINOP:
        return gen_binop(g, e);
    case N_UNOP:
        return gen_unop(g, e);
    case N_CALL:
        return gen_call_kaam(g, e);
    default:
        fprintf(stderr, "rabtac: internal error: cannot generate %s\n", node_kind_name(e->kind));
        exit(3);
    }
}

void gen_stmt(Gen *g, Node *s)
{
    /* code after a waapis is unreachable: give it a block of its own */
    if (gen_is_terminated(g))
        LLVMPositionBuilderAtEnd(g->b, gen_block(g, "dead"));

    switch (s->kind) {
    case N_VARDECL:
        LLVMBuildStore(g->b,
                       s->a->kind == N_RECV ? gen_recv_i64(g, s->a) : gen_expr(g, s->a),
                       slot_i(g, s->slot));
        break;
    case N_ARRDECL:   gen_arrdecl(g, s); break;
    case N_ASSIGN:    gen_assign(g, s); break;
    case N_IDXASSIGN: gen_idxassign(g, s); break;
    case N_IF:        gen_if(g, s); break;
    case N_WHILE:     gen_while(g, s); break;
    case N_FOR:       gen_for(g, s); break;
    case N_RETURN:    gen_return(g, s); break;
    case N_PRINT:     gen_print(g, s); break;
    case N_CALLSTMT:  (void)gen_expr(g, s->a); break;
    case N_CHANDECL:  gen_chandecl(g, s); break;
    case N_SPAWN:     gen_spawn(g, s); break;
    case N_SEND:      gen_send(g, s); break;
    case N_RECVSTMT:  gen_recv_discard(g, s->a); break;
    default:
        fprintf(stderr, "rabtac: internal error: cannot generate %s\n", node_kind_name(s->kind));
        exit(3);
    }
}

void gen_stmts(Gen *g, NodeList *l)
{
    for (int i = 0; i < l->count; i++)
        gen_stmt(g, l->items[i]);
}

/* ------------------------------------------------------------------------ */
/*  Module                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Give the module the host's target triple and data layout (type sizes and
 * alignments, e.g. i64 is 8-byte aligned on x86-64), so the IR matches what
 * clang / llc will compile it for.
 */
static void set_host_target(LLVMModuleRef mod)
{
    char *triple = LLVMGetDefaultTargetTriple();
    char *err = NULL;
    LLVMTargetRef target;

    LLVMSetTarget(mod, triple);
    if (LLVMInitializeNativeTarget() == 0 &&
        LLVMGetTargetFromTriple(triple, &target, &err) == 0) {
        LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
            target, triple, "generic", "", LLVMCodeGenLevelDefault,
            LLVMRelocPIC, LLVMCodeModelDefault);
        LLVMTargetDataRef dl = LLVMCreateTargetDataLayout(tm);
        LLVMSetModuleDataLayout(mod, dl);
        LLVMDisposeTargetData(dl);
        LLVMDisposeTargetMachine(tm);
    }
    if (err)
        LLVMDisposeMessage(err);
    LLVMDisposeMessage(triple);
}

int llvm_generate(Node *program, const char *source_name, const char *ll_path)
{
    Gen g;
    char *err = NULL;
    int status = 0;

    memset(&g, 0, sizeof g);
    g.program = program;
    g.source_name = source_name;

    g.ctx = LLVMContextCreate();
    g.mod = LLVMModuleCreateWithNameInContext(source_name, g.ctx);
    LLVMSetSourceFileName(g.mod, source_name, strlen(source_name));
    set_host_target(g.mod);

    g.b = LLVMCreateBuilderInContext(g.ctx);
    g.ab = LLVMCreateBuilderInContext(g.ctx);

    /* type mapping */
    g.t_void = LLVMVoidTypeInContext(g.ctx);
    g.t_i1 = LLVMInt1TypeInContext(g.ctx);
    g.t_i8 = LLVMInt8TypeInContext(g.ctx);
    g.t_i32 = LLVMInt32TypeInContext(g.ctx);
    g.t_i64 = LLVMInt64TypeInContext(g.ctx);          /* Rabta i64 */
    g.t_ptr = LLVMPointerTypeInContext(g.ctx, 0);      /* opaque ptr */

    g.g_source = gen_cstr(&g, source_name);

    gen_core_runtime(&g);
    gen_actor_runtime(&g);

    gen_declare_functions(&g);
    gen_declare_actors(&g);
    gen_define_functions(&g);
    gen_define_actors(&g);
    gen_c_main(&g);

    if (LLVMVerifyModule(g.mod, LLVMReturnStatusAction, &err)) {
        fprintf(stderr, "rabtac: internal error: generated LLVM IR is invalid:\n%s\n", err);
        status = 1;
    }
    LLVMDisposeMessage(err);
    err = NULL;

    if (status == 0 && LLVMPrintModuleToFile(g.mod, ll_path, &err)) {
        fprintf(stderr, "rabtac: cannot write %s: %s\n", ll_path, err ? err : "unknown error");
        status = 1;
    }
    if (err)
        LLVMDisposeMessage(err);

    LLVMDisposeBuilder(g.ab);
    LLVMDisposeBuilder(g.b);
    LLVMDisposeModule(g.mod);
    LLVMContextDispose(g.ctx);
    return status;
}
