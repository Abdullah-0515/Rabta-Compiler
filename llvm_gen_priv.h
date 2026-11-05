/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  llvm_gen_priv.h - State and helpers shared by the code generator files.
 *
 *    llvm_gen.c    module/context/builder setup, type mapping, libc
 *                  declarations, variables and thali, dikhao, runtime
 *                  error helpers, statement and expression dispatch
 *    llvm_flow.c   arithmetic, comparisons, logic, agar/warna, jab_tak, ghoom
 *    llvm_func.c   kaam definitions, bulao calls, waapis, the C main()
 *    llvm_actor.c  banda threads, nali channels, bolo/suno, IR runtime
 *
 *  Value mapping
 *    Rabta i64            -> LLVM i64
 *    conditions           -> LLVM i1 (value != 0), widened back with zext
 *    thali                -> heap block of i64 (ptr) + its length (i64)
 *    nali / sira endpoint -> ptr to a %rabta.chan; the role of an endpoint
 *                            is known statically from its type
 *
 *  Every variable of a kaam/banda has a frame slot number (semantic.c).
 *  Each slot is backed by allocas in the function's entry block, created
 *  on first use; LLVM's mem2reg pass (clang -O1 and above) turns them into
 *  SSA registers.
 */
#ifndef RABTA_LLVM_GEN_PRIV_H
#define RABTA_LLVM_GEN_PRIV_H

#include <llvm-c/Core.h>

#include "ast.h"

/* allocas backing one frame slot (NULL until first used) */
typedef struct GenSlot {
    LLVMValueRef i;      /* i64 variable                         */
    LLVMValueRef arr;    /* thali: ptr to the elements           */
    LLVMValueRef len;    /* thali: number of elements (i64)      */
    LLVMValueRef chan;   /* nali or sira: ptr to the channel     */
} GenSlot;

typedef struct Gen {
    LLVMContextRef ctx;
    LLVMModuleRef  mod;
    LLVMBuilderRef b;         /* builder for the code being generated  */
    LLVMBuilderRef ab;        /* builder for entry-block allocas       */

    /* type mapping */
    LLVMTypeRef t_void, t_i1, t_i8, t_i32, t_i64, t_ptr;
    LLVMTypeRef t_msg;        /* %rabta.msg   = { i64, ptr }           */
    LLVMTypeRef t_queue;      /* %rabta.queue                          */
    LLVMTypeRef t_chan;       /* %rabta.chan  = { [2 x %rabta.queue] } */

    Node        *program;
    const char  *source_name;
    LLVMValueRef g_source;    /* the source file name as a C string    */

    /* the kaam / banda currently being generated */
    Node        *cur;
    LLVMValueRef fn;
    LLVMValueRef entry_br;    /* `br label %body` ending the entry block */
    GenSlot     *slots;
    const char **slot_names;  /* variable name of each slot (for the IR) */
    int          nslots;
    int         *owned;       /* slots of thali declared in this body  */
    int          nowned;
} Gen;

/* ---- llvm_gen.c ---- */
LLVMValueRef gen_libc(Gen *g, const char *name);
LLVMValueRef gen_call(Gen *g, LLVMValueRef fn, LLVMValueRef *args, unsigned n, const char *name);
LLVMValueRef gen_cstr(Gen *g, const char *s);
LLVMValueRef gen_i64(Gen *g, int64_t v);
LLVMValueRef gen_i32(Gen *g, int v);
LLVMValueRef gen_load_stdout(Gen *g);
LLVMValueRef gen_new_runtime_fn(Gen *g, const char *name, LLVMTypeRef ret,
                                LLVMTypeRef *params, unsigned n);
LLVMValueRef slot_i(Gen *g, int slot);
LLVMValueRef slot_arr(Gen *g, int slot);
LLVMValueRef slot_len(Gen *g, int slot);
LLVMValueRef slot_chan(Gen *g, int slot);
LLVMValueRef gen_entry_alloca(Gen *g, LLVMTypeRef t, const char *name);
LLVMBasicBlockRef gen_block(Gen *g, const char *name);
int          gen_is_terminated(Gen *g);
void         gen_runtime_fail(Gen *g, int line, const char *msg);
void         gen_begin_body(Gen *g, Node *fn_node, LLVMValueRef fn);
void         gen_end_body(Gen *g);
void         gen_free_owned(Gen *g);
LLVMValueRef gen_expr(Gen *g, Node *e);
void         gen_stmt(Gen *g, Node *s);
void         gen_stmts(Gen *g, NodeList *l);

/* ---- llvm_flow.c ---- */
LLVMValueRef gen_cond(Gen *g, Node *e);               /* i1 */
LLVMValueRef gen_binop(Gen *g, Node *e);
LLVMValueRef gen_unop(Gen *g, Node *e);
void         gen_if(Gen *g, Node *s);
void         gen_while(Gen *g, Node *s);
void         gen_for(Gen *g, Node *s);

/* ---- llvm_func.c ---- */
void         gen_declare_functions(Gen *g);
void         gen_define_functions(Gen *g);
LLVMValueRef gen_call_kaam(Gen *g, Node *call);
void         gen_return(Gen *g, Node *s);
void         gen_c_main(Gen *g);

/* ---- llvm_actor.c ---- */
void         gen_actor_runtime(Gen *g);
void         gen_declare_actors(Gen *g);
void         gen_define_actors(Gen *g);
void         gen_chandecl(Gen *g, Node *s);
void         gen_spawn(Gen *g, Node *s);
void         gen_send(Gen *g, Node *s);
LLVMValueRef gen_recv_i64(Gen *g, Node *recv);
void         gen_recv_into(Gen *g, Node *recv, LLVMValueRef data, LLVMValueRef len);
void         gen_recv_discard(Gen *g, Node *recv);

#endif /* RABTA_LLVM_GEN_PRIV_H */
