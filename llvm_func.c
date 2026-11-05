/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  llvm_func.c - kaam definitions, bulao calls, waapis and the C entry point.
 *
 *    kaam f(a, thali t)   ->  define internal i64 @kaam.f(i64 %a, ptr %t, i64 %t.len)
 *                             (a thali parameter is passed by reference as
 *                              its element pointer plus its length)
 *    bulao f(x, arr)      ->  %r = call i64 @kaam.f(i64 %x, ptr %arr, i64 %arr.len)
 *    waapis e             ->  free the thali owned by this call, ret i64 %e
 *
 *  The `kaam.` prefix keeps Rabta names apart from C names, so a kaam may be
 *  called e.g. `printf` or `exit` without clashing with libc.
 *
 *  Rabta's main becomes @kaam.main; the real C entry point is
 *
 *    define i32 @main(i32 %argc, ptr %argv) {
 *      call void @rabta.init()            ; actor runtime
 *      %rv = call i64 @kaam.main()
 *      call void @rabta.join_all()        ; wait for every banda
 *      call void @rabta.shutdown()        ; free channels
 *      ret i32 (%rv & 255)                ; process exit status
 *    }
 */
#include "llvm_gen_priv.h"
#include "semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void kaam_symbol(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "kaam.%s", name);
}

static LLVMValueRef kaam_fn(Gen *g, Node *fn)
{
    char sym[256];
    kaam_symbol(sym, sizeof sym, fn->name);
    return LLVMGetNamedFunction(g->mod, sym);
}

/* Pass 1: declare every kaam, so calls may appear before the definition. */
void gen_declare_functions(Gen *g)
{
    NodeList *items = g->program->list;
    for (int i = 0; i < items->count; i++) {
        Node *fn = items->items[i];
        if (fn->kind != N_FUNC)
            continue;

        LLVMTypeRef *ps = malloc(sizeof(LLVMTypeRef) * (2 * (size_t)fn->list->count + 1));
        unsigned n = 0;
        for (int k = 0; k < fn->list->count; k++) {
            Node *p = fn->list->items[k];
            if (p->pkind == PARAM_ARRAY) {
                ps[n++] = g->t_ptr;     /* elements */
                ps[n++] = g->t_i64;     /* length   */
            } else {
                ps[n++] = g->t_i64;
            }
        }
        char sym[256];
        kaam_symbol(sym, sizeof sym, fn->name);
        LLVMValueRef f = LLVMAddFunction(g->mod, sym, LLVMFunctionType(g->t_i64, ps, n, 0));
        LLVMSetLinkage(f, LLVMInternalLinkage);
        free(ps);
    }
}

/* Pass 2: generate every kaam body. */
void gen_define_functions(Gen *g)
{
    NodeList *items = g->program->list;
    for (int i = 0; i < items->count; i++) {
        Node *fn = items->items[i];
        if (fn->kind != N_FUNC)
            continue;

        LLVMValueRef f = kaam_fn(g, fn);
        gen_begin_body(g, fn, f);

        /* copy the parameters into their frame slots */
        unsigned arg = 0;
        for (int k = 0; k < fn->list->count; k++) {
            Node *p = fn->list->items[k];
            if (p->pkind == PARAM_ARRAY) {
                LLVMValueRef data = LLVMGetParam(f, arg++);
                LLVMValueRef len = LLVMGetParam(f, arg++);
                char name[160];
                snprintf(name, sizeof name, "%s.len", p->name);
                LLVMSetValueName2(data, p->name, strlen(p->name));
                LLVMSetValueName2(len, name, strlen(name));
                LLVMBuildStore(g->b, data, slot_arr(g, p->slot));
                LLVMBuildStore(g->b, len, slot_len(g, p->slot));
            } else {
                LLVMValueRef v = LLVMGetParam(f, arg++);
                LLVMSetValueName2(v, p->name, strlen(p->name));
                LLVMBuildStore(g->b, v, slot_i(g, p->slot));
            }
        }

        gen_stmts(g, fn->a->list);

        /* The semantic pass guarantees every path ends in waapis, so this
           block is unreachable; it is kept as a safety net. */
        if (!gen_is_terminated(g)) {
            char msg[300];
            snprintf(msg, sizeof msg, "kaam '%s' ended without waapis", fn->name);
            gen_runtime_fail(g, fn->a->ival, msg);
        }
        gen_end_body(g);
    }
}

/* bulao f(args): arguments are evaluated left to right. */
LLVMValueRef gen_call_kaam(Gen *g, Node *call)
{
    Node *fn = call->ref;
    int np = fn->list->count;
    LLVMValueRef *args = malloc(sizeof(LLVMValueRef) * (2 * (size_t)np + 1));
    unsigned n = 0;

    for (int k = 0; k < np; k++) {
        Node *p = fn->list->items[k];
        Node *a = call->list->items[k];
        if (p->pkind == PARAM_ARRAY) {      /* by reference */
            args[n++] = LLVMBuildLoad2(g->b, g->t_ptr, slot_arr(g, a->slot), a->name);
            args[n++] = LLVMBuildLoad2(g->b, g->t_i64, slot_len(g, a->slot), "len");
        } else {
            args[n++] = gen_expr(g, a);
        }
    }
    LLVMValueRef r = gen_call(g, kaam_fn(g, fn), args, n, "call");
    free(args);
    return r;
}

/* waapis e */
void gen_return(Gen *g, Node *s)
{
    LLVMValueRef v = gen_expr(g, s->a);
    gen_free_owned(g);
    LLVMBuildRet(g->b, v);
}

/* The C entry point that runs Rabta's main and then waits for all actors. */
void gen_c_main(Gen *g)
{
    LLVMTypeRef ps[] = { g->t_i32, g->t_ptr };
    LLVMValueRef fn = LLVMAddFunction(g->mod, "main", LLVMFunctionType(g->t_i32, ps, 2, 0));
    LLVMSetValueName2(LLVMGetParam(fn, 0), "argc", 4);
    LLVMSetValueName2(LLVMGetParam(fn, 1), "argv", 4);
    g->fn = fn;
    LLVMPositionBuilderAtEnd(g->b, gen_block(g, "entry"));

    gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.init"), NULL, 0, "");
    LLVMValueRef rv = gen_call(g, kaam_fn(g, find_function(g->program, "main")), NULL, 0, "rv");
    gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.join_all"), NULL, 0, "");
    gen_call(g, LLVMGetNamedFunction(g->mod, "rabta.shutdown"), NULL, 0, "");

    LLVMValueRef out = gen_load_stdout(g);
    gen_call(g, gen_libc(g, "fflush"), &out, 1, "");
    LLVMValueRef low = LLVMBuildAnd(g->b, rv, gen_i64(g, 0xff), "status");
    LLVMBuildRet(g->b, LLVMBuildTrunc(g->b, low, g->t_i32, "exit.code"));
}
