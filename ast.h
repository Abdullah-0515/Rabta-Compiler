/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  ast.h - Abstract Syntax Tree definitions.
 *
 *  The parser (rabta.y) builds this tree. The semantic pass (semantic.c,
 *  protocol.c) annotates it (types, variable slots, resolved declarations),
 *  and the interpreter (interpreter.c) walks it. The same annotated tree is
 *  meant to be the input of the Phase 2 LLVM IR generator.
 *
 *  Child layout per node kind (fields not listed are unused):
 *
 *    N_PROGRAM    list = top-level items (N_PROTOCOL / N_ACTOR / N_FUNC)
 *    N_PROTOCOL   name = protocol name, list = steps (N_PSTEP)
 *    N_PSTEP      name = role name ("client"/"server"), ty = message type
 *    N_ACTOR      name, list = params (N_PARAM), a = body (N_BLOCK)
 *    N_FUNC       name, list = params (N_PARAM), a = body (N_BLOCK)
 *    N_PARAM      name, pkind = PARAM_I64 | PARAM_ARRAY | PARAM_ENDPOINT,
 *                 name2 = protocol, name3 = role   (endpoint params only)
 *    N_BLOCK      list = statements, ival = line of the closing khatam
 *
 *    N_VARDECL    rakho name = a            (a = expression or N_RECV)
 *    N_ARRDECL    thali name[ival] (= a)?   (a = N_ARRLIT, N_RECV or NULL)
 *    N_ASSIGN     name = a                  (a = expression or N_RECV)
 *    N_IDXASSIGN  name[a] = b               (b = expression or N_RECV)
 *    N_IF         agar a b (warna c)?       (c = N_BLOCK, N_IF or NULL)
 *    N_WHILE      jab_tak a chal b khatam
 *    N_FOR        ghoom [name : a -> b] chal c khatam
 *    N_RETURN     waapis a
 *    N_PRINT      dikhao(list)              (list items: expressions or N_STR)
 *    N_CALLSTMT   bulao ...                 (a = N_CALL)
 *    N_CHANDECL   nali name : name2
 *    N_SPAWN      paida name(list)
 *    N_SEND       bolo a, b                 (a = endpoint: N_ENDPOINT or N_VAR)
 *    N_RECVSTMT   suno a                    (a = N_RECV, value discarded)
 *
 *    N_INT        ival
 *    N_STR        str
 *    N_VAR        name
 *    N_INDEX      name[a]
 *    N_BINOP      a op b
 *    N_UNOP       op a
 *    N_CALL       bulao name(list)
 *    N_ARRLIT     [list]
 *    N_ENDPOINT   name.name2                (channel.role)
 *    N_RECV       suno a                    (a = endpoint)
 */
#ifndef RABTA_AST_H
#define RABTA_AST_H

#include <stdint.h>
#include <stdio.h>

typedef struct Node Node;

typedef struct NodeList {
    Node **items;
    int    count;
    int    cap;
} NodeList;

typedef enum {
    /* top level */
    N_PROGRAM, N_PROTOCOL, N_PSTEP, N_ACTOR, N_FUNC, N_PARAM, N_BLOCK,
    /* statements */
    N_VARDECL, N_ARRDECL, N_ASSIGN, N_IDXASSIGN, N_IF, N_WHILE, N_FOR,
    N_RETURN, N_PRINT, N_CALLSTMT, N_CHANDECL, N_SPAWN, N_SEND, N_RECVSTMT,
    /* expressions */
    N_INT, N_STR, N_VAR, N_INDEX, N_BINOP, N_UNOP, N_CALL, N_ARRLIT,
    N_ENDPOINT, N_RECV
} NodeKind;

typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR,
    OP_NEG, OP_NOT
} OpKind;

typedef enum {
    TY_NONE,      /* no type / "anything" (e.g. a discarded suno)       */
    TY_I64,       /* 64-bit signed integer                              */
    TY_ARRAY,     /* thali: bounded array of i64, size in Type.size      */
    TY_STRING,    /* string literal (only allowed inside dikhao)         */
    TY_CHANNEL,   /* a channel created by nali                          */
    TY_ENDPOINT,  /* one end of a channel: Type.proto + Type.role        */
    TY_ERROR      /* already reported; suppresses cascaded errors        */
} TypeKind;

enum { ROLE_NONE = -1, ROLE_CLIENT = 0, ROLE_SERVER = 1 };

typedef enum { PARAM_I64, PARAM_ARRAY, PARAM_ENDPOINT } ParamKind;

typedef struct Type {
    TypeKind kind;
    int64_t  size;    /* TY_ARRAY: number of elements, -1 = unknown (param) */
    Node    *proto;   /* TY_CHANNEL / TY_ENDPOINT: the N_PROTOCOL node      */
    int      role;    /* TY_ENDPOINT: ROLE_CLIENT or ROLE_SERVER            */
} Type;

struct Node {
    NodeKind  kind;
    int       line;

    char     *name;
    char     *name2;
    char     *name3;
    char     *str;
    int64_t   ival;
    int       op;
    int       pkind;

    Node     *a, *b, *c;
    NodeList *list;

    /* ---- annotations filled in by the semantic pass ---- */
    Type      ty;      /* type of an expression / param / message          */
    int       slot;    /* frame slot of the variable this node refers to   */
    int       nslots;  /* N_FUNC / N_ACTOR: number of frame slots needed    */
    Node     *ref;     /* resolved declaration (function, actor, protocol,
                          channel declaration or endpoint parameter)       */
    int       role;    /* endpoint role (ROLE_CLIENT / ROLE_SERVER)         */
    int       tid;     /* protocol checker: endpoint tracker index         */
};

/* ---- lists ---- */
NodeList *list_new(void);
NodeList *list_append(NodeList *l, Node *n);
void      list_free(NodeList *l);      /* frees the list and its nodes */

/* ---- constructors (used by the Bison actions) ---- */
Node *node_new(NodeKind kind, int line);
Node *ast_program(NodeList *items);
Node *ast_protocol(int line, char *name, NodeList *steps);
Node *ast_pstep(int line, char *role, Type msg);
Node *ast_actor(int line, char *name, NodeList *params, Node *body);
Node *ast_func(int line, char *name, NodeList *params, Node *body);
Node *ast_param(int line, ParamKind kind, char *name, char *proto, char *role);
Node *ast_block(int line, NodeList *stmts);

Node *ast_vardecl(int line, char *name, Node *init);
Node *ast_arrdecl(int line, char *name, int64_t size, Node *init);
Node *ast_assign(int line, char *name, Node *value);
Node *ast_idxassign(int line, char *name, Node *index, Node *value);
Node *ast_if(int line, Node *cond, Node *then_b, Node *else_b);
Node *ast_while(int line, Node *cond, Node *body);
Node *ast_for(int line, char *var, Node *start, Node *end, Node *body);
Node *ast_return(int line, Node *value);
Node *ast_print(int line, NodeList *args);
Node *ast_callstmt(int line, Node *call);
Node *ast_chandecl(int line, char *name, char *proto);
Node *ast_spawn(int line, char *actor, NodeList *args);
Node *ast_send(int line, Node *endpoint, Node *value);
Node *ast_recvstmt(int line, Node *recv);

Node *ast_int(int line, int64_t v);
Node *ast_str(int line, char *s);
Node *ast_var(int line, char *name);
Node *ast_index(int line, char *name, Node *index);
Node *ast_binop(int line, int op, Node *l, Node *r);
Node *ast_unop(int line, int op, Node *e);
Node *ast_call(int line, char *name, NodeList *args);
Node *ast_arrlit(int line, NodeList *elems);
Node *ast_endpoint(int line, char *channel, char *role);
Node *ast_recv(int line, Node *endpoint);

/* ---- types ---- */
Type        type_make(TypeKind k);
Type        type_array(int64_t size);
int         type_equal(Type a, Type b);
const char *type_str(Type t, char *buf, size_t n);

/* ---- utilities ---- */
const char *op_symbol(int op);
const char *node_kind_name(NodeKind k);
void        ast_dump(FILE *out, const Node *n, int indent);
void        ast_free(Node *n);

#endif /* RABTA_AST_H */
