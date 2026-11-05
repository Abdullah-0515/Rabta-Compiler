/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  llvm_flow.c - Expressions and control flow.
 *
 *  Arithmetic
 *    + - *      add / sub / mul (no nsw flag: wraps around, as in Phase 1)
 *    / %        sdiv / srem guarded against division by zero (runtime
 *               error) and INT64_MIN / -1 (which would trap on x86-64:
 *               the result is defined as INT64_MIN, and 0 for %)
 *    - (unary)  sub 0, x
 *  Comparisons    icmp eq/ne/slt/sle/sgt/sge, widened to i64 0/1 with zext
 *  Logic          && and || short-circuit (extra basic blocks + phi);
 *                 ! is icmp eq x, 0
 *
 *  Control flow (each construct gets its own basic blocks)
 *    agar c b warna e   ->  br i1 %c, label %if.then, label %if.else
 *                           (both branches jump to %if.end)
 *    jab_tak c chal b   ->  %while.cond / %while.body / %while.end
 *    ghoom [i : a -> b] ->  %for.cond / %for.body / %for.inc / %for.end;
 *                           a and b are evaluated once, b is exclusive
 */
#include "llvm_gen_priv.h"

/* ------------------------------------------------------------------------ */
/*  Expressions                                                             */
/* ------------------------------------------------------------------------ */

/* Rabta truth value (i64 != 0) as an LLVM i1. */
LLVMValueRef gen_cond(Gen *g, Node *e)
{
    LLVMValueRef v = gen_expr(g, e);
    return LLVMBuildICmp(g->b, LLVMIntNE, v, gen_i64(g, 0), "tobool");
}

static LLVMValueRef bool_to_i64(Gen *g, LLVMValueRef b)
{
    return LLVMBuildZExt(g->b, b, g->t_i64, "bool");
}

/*
 *   %l = ...                         a && b
 *   br i1 %l, label %and.rhs, label %and.end
 * and.rhs:
 *   %r = ...
 *   br label %and.end
 * and.end:
 *   %v = phi i1 [ false, %lhs ], [ %r, %rhs.end ]
 */
static LLVMValueRef gen_logic(Gen *g, Node *e)
{
    int is_and = e->op == OP_AND;
    LLVMValueRef l = gen_cond(g, e->a);
    LLVMBasicBlockRef lhs_end = LLVMGetInsertBlock(g->b);
    LLVMBasicBlockRef rhs = gen_block(g, is_and ? "and.rhs" : "or.rhs");
    LLVMBasicBlockRef end = gen_block(g, is_and ? "and.end" : "or.end");

    if (is_and)
        LLVMBuildCondBr(g->b, l, rhs, end);
    else
        LLVMBuildCondBr(g->b, l, end, rhs);

    LLVMPositionBuilderAtEnd(g->b, rhs);
    LLVMValueRef r = gen_cond(g, e->b);
    LLVMBasicBlockRef rhs_end = LLVMGetInsertBlock(g->b);
    LLVMBuildBr(g->b, end);

    LLVMPositionBuilderAtEnd(g->b, end);
    LLVMValueRef phi = LLVMBuildPhi(g->b, g->t_i1, is_and ? "and" : "or");
    LLVMValueRef in_v[] = { LLVMConstInt(g->t_i1, is_and ? 0 : 1, 0), r };
    LLVMBasicBlockRef in_b[] = { lhs_end, rhs_end };
    LLVMAddIncoming(phi, in_v, in_b, 2);
    return bool_to_i64(g, phi);
}

/* sdiv / srem with the two runtime checks described above. */
static LLVMValueRef gen_divmod(Gen *g, Node *e, LLVMValueRef l, LLVMValueRef r)
{
    int is_div = e->op == OP_DIV;
    LLVMValueRef zero = LLVMBuildICmp(g->b, LLVMIntEQ, r, gen_i64(g, 0), "divisor.zero");
    LLVMBasicBlockRef fail = gen_block(g, "div.zero");
    LLVMBasicBlockRef ok = gen_block(g, "div.ok");
    LLVMBuildCondBr(g->b, zero, fail, ok);

    LLVMPositionBuilderAtEnd(g->b, fail);
    gen_runtime_fail(g, e->line, is_div ? "division by zero" : "modulo by zero");

    LLVMPositionBuilderAtEnd(g->b, ok);
    /* x / -1 is computed as 0 - x so INT64_MIN / -1 does not trap */
    LLVMValueRef m1 = LLVMBuildICmp(g->b, LLVMIntEQ, r, gen_i64(g, -1), "divisor.m1");
    LLVMValueRef safe = LLVMBuildSelect(g->b, m1, gen_i64(g, 1), r, "divisor");
    if (is_div) {
        LLVMValueRef q = LLVMBuildSDiv(g->b, l, safe, "quot");
        LLVMValueRef neg = LLVMBuildSub(g->b, gen_i64(g, 0), l, "neg");
        return LLVMBuildSelect(g->b, m1, neg, q, "div");
    }
    LLVMValueRef rem = LLVMBuildSRem(g->b, l, safe, "rem");
    return LLVMBuildSelect(g->b, m1, gen_i64(g, 0), rem, "mod");
}

LLVMValueRef gen_binop(Gen *g, Node *e)
{
    if (e->op == OP_AND || e->op == OP_OR)
        return gen_logic(g, e);

    LLVMValueRef l = gen_expr(g, e->a);     /* left to right */
    LLVMValueRef r = gen_expr(g, e->b);

    switch (e->op) {
    case OP_ADD: return LLVMBuildAdd(g->b, l, r, "add");
    case OP_SUB: return LLVMBuildSub(g->b, l, r, "sub");
    case OP_MUL: return LLVMBuildMul(g->b, l, r, "mul");
    case OP_DIV:
    case OP_MOD: return gen_divmod(g, e, l, r);
    case OP_EQ:  return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntEQ, l, r, "eq"));
    case OP_NE:  return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntNE, l, r, "ne"));
    case OP_LT:  return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntSLT, l, r, "lt"));
    case OP_LE:  return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntSLE, l, r, "le"));
    case OP_GT:  return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntSGT, l, r, "gt"));
    case OP_GE:  return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntSGE, l, r, "ge"));
    default:     return gen_i64(g, 0);
    }
}

LLVMValueRef gen_unop(Gen *g, Node *e)
{
    LLVMValueRef v = gen_expr(g, e->a);
    if (e->op == OP_NEG)
        return LLVMBuildSub(g->b, gen_i64(g, 0), v, "neg");
    return bool_to_i64(g, LLVMBuildICmp(g->b, LLVMIntEQ, v, gen_i64(g, 0), "not"));
}

/* ------------------------------------------------------------------------ */
/*  Statements                                                              */
/* ------------------------------------------------------------------------ */

/* Jump to `to` unless the current block already ended (e.g. waapis). */
static void br_if_open(Gen *g, LLVMBasicBlockRef to)
{
    if (!gen_is_terminated(g))
        LLVMBuildBr(g->b, to);
}

void gen_if(Gen *g, Node *s)
{
    LLVMValueRef c = gen_cond(g, s->a);
    LLVMBasicBlockRef then_bb = gen_block(g, "if.then");
    LLVMBasicBlockRef else_bb = s->c ? gen_block(g, "if.else") : NULL;
    LLVMBasicBlockRef end_bb = gen_block(g, "if.end");

    LLVMBuildCondBr(g->b, c, then_bb, else_bb ? else_bb : end_bb);

    LLVMPositionBuilderAtEnd(g->b, then_bb);
    gen_stmts(g, s->b->list);
    br_if_open(g, end_bb);

    if (else_bb) {
        LLVMPositionBuilderAtEnd(g->b, else_bb);
        if (s->c->kind == N_IF)           /* warna agar ... */
            gen_if(g, s->c);
        else
            gen_stmts(g, s->c->list);
        br_if_open(g, end_bb);
    }

    /* if both branches returned, %if.end has no predecessors and stays
       empty; the next statement (or the end of the body) fills it */
    LLVMPositionBuilderAtEnd(g->b, end_bb);
}

void gen_while(Gen *g, Node *s)
{
    LLVMBasicBlockRef cond_bb = gen_block(g, "while.cond");
    LLVMBasicBlockRef body_bb = gen_block(g, "while.body");
    LLVMBasicBlockRef end_bb = gen_block(g, "while.end");

    LLVMBuildBr(g->b, cond_bb);
    LLVMPositionBuilderAtEnd(g->b, cond_bb);
    LLVMBuildCondBr(g->b, gen_cond(g, s->a), body_bb, end_bb);

    LLVMPositionBuilderAtEnd(g->b, body_bb);
    gen_stmts(g, s->b->list);
    br_if_open(g, cond_bb);

    LLVMPositionBuilderAtEnd(g->b, end_bb);
}

/* ghoom [i : from -> to] chal ... khatam   ==   for (i = from; i < to; i++) */
void gen_for(Gen *g, Node *s)
{
    LLVMValueRef from = gen_expr(g, s->a);
    LLVMValueRef to = gen_expr(g, s->b);
    LLVMValueRef iv = slot_i(g, s->slot);
    LLVMBuildStore(g->b, from, iv);

    LLVMBasicBlockRef cond_bb = gen_block(g, "for.cond");
    LLVMBasicBlockRef body_bb = gen_block(g, "for.body");
    LLVMBasicBlockRef inc_bb = gen_block(g, "for.inc");
    LLVMBasicBlockRef end_bb = gen_block(g, "for.end");

    LLVMBuildBr(g->b, cond_bb);
    LLVMPositionBuilderAtEnd(g->b, cond_bb);
    LLVMValueRef i = LLVMBuildLoad2(g->b, g->t_i64, iv, s->name);
    LLVMValueRef more = LLVMBuildICmp(g->b, LLVMIntSLT, i, to, "for.more");
    LLVMBuildCondBr(g->b, more, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(g->b, body_bb);
    gen_stmts(g, s->c->list);
    br_if_open(g, inc_bb);

    LLVMPositionBuilderAtEnd(g->b, inc_bb);
    LLVMValueRef cur = LLVMBuildLoad2(g->b, g->t_i64, iv, s->name);
    LLVMBuildStore(g->b, LLVMBuildAdd(g->b, cur, gen_i64(g, 1), "for.next"), iv);
    LLVMBuildBr(g->b, cond_bb);

    LLVMPositionBuilderAtEnd(g->b, end_bb);
}
