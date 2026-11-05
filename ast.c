/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  ast.c - AST construction, type helpers, pretty printer (--ast) and cleanup.
 */
#include "ast.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n)
{
    void *p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "rabta: out of memory\n");
        exit(3);
    }
    return p;
}

/* ------------------------------------------------------------------------ */
/*  Lists                                                                   */
/* ------------------------------------------------------------------------ */

NodeList *list_new(void)
{
    return xmalloc(sizeof(NodeList));
}

NodeList *list_append(NodeList *l, Node *n)
{
    if (!l)
        l = list_new();
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->items = realloc(l->items, (size_t)l->cap * sizeof(Node *));
        if (!l->items) {
            fprintf(stderr, "rabta: out of memory\n");
            exit(3);
        }
    }
    l->items[l->count++] = n;
    return l;
}

void list_free(NodeList *l)
{
    if (!l)
        return;
    for (int i = 0; i < l->count; i++)
        ast_free(l->items[i]);
    free(l->items);
    free(l);
}

/* ------------------------------------------------------------------------ */
/*  Constructors                                                            */
/* ------------------------------------------------------------------------ */

Node *node_new(NodeKind kind, int line)
{
    Node *n = xmalloc(sizeof(Node));
    n->kind = kind;
    n->line = line;
    n->slot = -1;
    n->tid = -1;
    n->role = ROLE_NONE;
    n->ty = type_make(TY_NONE);
    return n;
}

Node *ast_program(NodeList *items)
{
    Node *n = node_new(N_PROGRAM, 1);
    n->list = items ? items : list_new();
    return n;
}

Node *ast_protocol(int line, char *name, NodeList *steps)
{
    Node *n = node_new(N_PROTOCOL, line);
    n->name = name;
    n->list = steps;
    return n;
}

Node *ast_pstep(int line, char *role, Type msg)
{
    Node *n = node_new(N_PSTEP, line);
    n->name = role;
    n->ty = msg;
    return n;
}

Node *ast_actor(int line, char *name, NodeList *params, Node *body)
{
    Node *n = node_new(N_ACTOR, line);
    n->name = name;
    n->list = params ? params : list_new();
    n->a = body;
    return n;
}

Node *ast_func(int line, char *name, NodeList *params, Node *body)
{
    Node *n = node_new(N_FUNC, line);
    n->name = name;
    n->list = params ? params : list_new();
    n->a = body;
    return n;
}

Node *ast_param(int line, ParamKind kind, char *name, char *proto, char *role)
{
    Node *n = node_new(N_PARAM, line);
    n->pkind = kind;
    n->name = name;
    n->name2 = proto;
    n->name3 = role;
    return n;
}

Node *ast_block(int line, NodeList *stmts)
{
    Node *n = node_new(N_BLOCK, line);
    n->list = stmts ? stmts : list_new();
    return n;
}

Node *ast_vardecl(int line, char *name, Node *init)
{
    Node *n = node_new(N_VARDECL, line);
    n->name = name;
    n->a = init;
    return n;
}

Node *ast_arrdecl(int line, char *name, int64_t size, Node *init)
{
    Node *n = node_new(N_ARRDECL, line);
    n->name = name;
    n->ival = size;
    n->a = init;
    return n;
}

Node *ast_assign(int line, char *name, Node *value)
{
    Node *n = node_new(N_ASSIGN, line);
    n->name = name;
    n->a = value;
    return n;
}

Node *ast_idxassign(int line, char *name, Node *index, Node *value)
{
    Node *n = node_new(N_IDXASSIGN, line);
    n->name = name;
    n->a = index;
    n->b = value;
    return n;
}

Node *ast_if(int line, Node *cond, Node *then_b, Node *else_b)
{
    Node *n = node_new(N_IF, line);
    n->a = cond;
    n->b = then_b;
    n->c = else_b;
    return n;
}

Node *ast_while(int line, Node *cond, Node *body)
{
    Node *n = node_new(N_WHILE, line);
    n->a = cond;
    n->b = body;
    return n;
}

Node *ast_for(int line, char *var, Node *start, Node *end, Node *body)
{
    Node *n = node_new(N_FOR, line);
    n->name = var;
    n->a = start;
    n->b = end;
    n->c = body;
    return n;
}

Node *ast_return(int line, Node *value)
{
    Node *n = node_new(N_RETURN, line);
    n->a = value;
    return n;
}

Node *ast_print(int line, NodeList *args)
{
    Node *n = node_new(N_PRINT, line);
    n->list = args ? args : list_new();
    return n;
}

Node *ast_callstmt(int line, Node *call)
{
    Node *n = node_new(N_CALLSTMT, line);
    n->a = call;
    return n;
}

Node *ast_chandecl(int line, char *name, char *proto)
{
    Node *n = node_new(N_CHANDECL, line);
    n->name = name;
    n->name2 = proto;
    return n;
}

Node *ast_spawn(int line, char *actor, NodeList *args)
{
    Node *n = node_new(N_SPAWN, line);
    n->name = actor;
    n->list = args ? args : list_new();
    return n;
}

Node *ast_send(int line, Node *endpoint, Node *value)
{
    Node *n = node_new(N_SEND, line);
    n->a = endpoint;
    n->b = value;
    return n;
}

Node *ast_recvstmt(int line, Node *recv)
{
    Node *n = node_new(N_RECVSTMT, line);
    n->a = recv;
    return n;
}

Node *ast_int(int line, int64_t v)
{
    Node *n = node_new(N_INT, line);
    n->ival = v;
    return n;
}

Node *ast_str(int line, char *s)
{
    Node *n = node_new(N_STR, line);
    n->str = s;
    return n;
}

Node *ast_var(int line, char *name)
{
    Node *n = node_new(N_VAR, line);
    n->name = name;
    return n;
}

Node *ast_index(int line, char *name, Node *index)
{
    Node *n = node_new(N_INDEX, line);
    n->name = name;
    n->a = index;
    return n;
}

Node *ast_binop(int line, int op, Node *l, Node *r)
{
    Node *n = node_new(N_BINOP, line);
    n->op = op;
    n->a = l;
    n->b = r;
    return n;
}

Node *ast_unop(int line, int op, Node *e)
{
    Node *n = node_new(N_UNOP, line);
    n->op = op;
    n->a = e;
    return n;
}

Node *ast_call(int line, char *name, NodeList *args)
{
    Node *n = node_new(N_CALL, line);
    n->name = name;
    n->list = args ? args : list_new();
    return n;
}

Node *ast_arrlit(int line, NodeList *elems)
{
    Node *n = node_new(N_ARRLIT, line);
    n->list = elems ? elems : list_new();
    return n;
}

Node *ast_endpoint(int line, char *channel, char *role)
{
    Node *n = node_new(N_ENDPOINT, line);
    n->name = channel;
    n->name2 = role;
    return n;
}

Node *ast_recv(int line, Node *endpoint)
{
    Node *n = node_new(N_RECV, line);
    n->a = endpoint;
    return n;
}

/* ------------------------------------------------------------------------ */
/*  Types                                                                   */
/* ------------------------------------------------------------------------ */

Type type_make(TypeKind k)
{
    Type t;
    t.kind = k;
    t.size = 0;
    t.proto = NULL;
    t.role = ROLE_NONE;
    return t;
}

Type type_array(int64_t size)
{
    Type t = type_make(TY_ARRAY);
    t.size = size;
    return t;
}

int type_equal(Type a, Type b)
{
    if (a.kind != b.kind)
        return 0;
    switch (a.kind) {
    case TY_ARRAY:
        return a.size == b.size;
    case TY_CHANNEL:
        return a.proto == b.proto;
    case TY_ENDPOINT:
        return a.proto == b.proto && a.role == b.role;
    default:
        return 1;
    }
}

const char *type_str(Type t, char *buf, size_t n)
{
    switch (t.kind) {
    case TY_NONE:     snprintf(buf, n, "nothing"); break;
    case TY_I64:      snprintf(buf, n, "i64"); break;
    case TY_STRING:   snprintf(buf, n, "string"); break;
    case TY_ERROR:    snprintf(buf, n, "<error>"); break;
    case TY_ARRAY:
        if (t.size < 0)
            snprintf(buf, n, "thali");
        else
            snprintf(buf, n, "thali[%" PRId64 "]", t.size);
        break;
    case TY_CHANNEL:
        snprintf(buf, n, "nali %s", t.proto ? t.proto->name : "?");
        break;
    case TY_ENDPOINT:
        snprintf(buf, n, "sira %s.%s", t.proto ? t.proto->name : "?",
                 t.role == ROLE_CLIENT ? "client" : "server");
        break;
    }
    return buf;
}

/* ------------------------------------------------------------------------ */
/*  Names                                                                   */
/* ------------------------------------------------------------------------ */

const char *op_symbol(int op)
{
    switch (op) {
    case OP_ADD: return "+";
    case OP_SUB: return "-";
    case OP_MUL: return "*";
    case OP_DIV: return "/";
    case OP_MOD: return "%";
    case OP_EQ:  return "==";
    case OP_NE:  return "!=";
    case OP_LT:  return "<";
    case OP_LE:  return "<=";
    case OP_GT:  return ">";
    case OP_GE:  return ">=";
    case OP_AND: return "&&";
    case OP_OR:  return "||";
    case OP_NEG: return "-";
    case OP_NOT: return "!";
    }
    return "?";
}

const char *node_kind_name(NodeKind k)
{
    static const char *names[] = {
        "Program", "Protocol", "Step", "Actor", "Function", "Param", "Block",
        "VarDecl", "ArrayDecl", "Assign", "IndexAssign", "If", "While", "For",
        "Return", "Print", "CallStmt", "ChannelDecl", "Spawn", "Send", "RecvStmt",
        "Int", "String", "Var", "Index", "BinOp", "UnOp", "Call", "ArrayLit",
        "Endpoint", "Recv"
    };
    if ((int)k < 0 || (size_t)k >= sizeof(names) / sizeof(names[0]))
        return "?";
    return names[k];
}

/* ------------------------------------------------------------------------ */
/*  Pretty printer (rabta --ast file.rabta)                                 */
/* ------------------------------------------------------------------------ */

static void dump_list(FILE *out, const char *label, const NodeList *l, int indent)
{
    if (!l || l->count == 0)
        return;
    fprintf(out, "%*s%s:\n", indent, "", label);
    for (int i = 0; i < l->count; i++)
        ast_dump(out, l->items[i], indent + 2);
}

static void dump_child(FILE *out, const char *label, const Node *n, int indent)
{
    if (!n)
        return;
    fprintf(out, "%*s%s:\n", indent, "", label);
    ast_dump(out, n, indent + 2);
}

void ast_dump(FILE *out, const Node *n, int indent)
{
    char tb[64];
    if (!n)
        return;
    fprintf(out, "%*s%s", indent, "", node_kind_name(n->kind));

    switch (n->kind) {
    case N_PROTOCOL: case N_ACTOR: case N_FUNC: case N_VARDECL:
    case N_ASSIGN: case N_IDXASSIGN: case N_VAR: case N_INDEX:
    case N_CALL: case N_SPAWN: case N_FOR:
        fprintf(out, " %s", n->name);
        break;
    case N_PSTEP:
        fprintf(out, " %s bolega %s", n->name, type_str(n->ty, tb, sizeof tb));
        break;
    case N_PARAM:
        if (n->pkind == PARAM_ENDPOINT)
            fprintf(out, " sira %s : %s.%s", n->name, n->name2, n->name3);
        else if (n->pkind == PARAM_ARRAY)
            fprintf(out, " thali %s", n->name);
        else
            fprintf(out, " %s : i64", n->name);
        break;
    case N_ARRDECL:
        fprintf(out, " %s[%" PRId64 "]", n->name, n->ival);
        break;
    case N_CHANDECL:
        fprintf(out, " %s : %s", n->name, n->name2);
        break;
    case N_ENDPOINT:
        fprintf(out, " %s.%s", n->name, n->name2);
        break;
    case N_INT:
        fprintf(out, " %" PRId64, n->ival);
        break;
    case N_STR:
        fprintf(out, " \"%s\"", n->str);
        break;
    case N_BINOP: case N_UNOP:
        fprintf(out, " %s", op_symbol(n->op));
        break;
    default:
        break;
    }
    if (n->slot >= 0)
        fprintf(out, "  [slot %d]", n->slot);
    if (n->kind == N_FUNC || n->kind == N_ACTOR)
        fprintf(out, "  [%d slots]", n->nslots);
    fprintf(out, "  (line %d)\n", n->line);

    switch (n->kind) {
    case N_PROGRAM:   dump_list(out, "items", n->list, indent + 2); break;
    case N_PROTOCOL:  dump_list(out, "steps", n->list, indent + 2); break;
    case N_ACTOR:
    case N_FUNC:
        dump_list(out, "params", n->list, indent + 2);
        dump_child(out, "body", n->a, indent + 2);
        break;
    case N_BLOCK:     dump_list(out, "stmts", n->list, indent + 2); break;
    case N_VARDECL:
    case N_ARRDECL:   dump_child(out, "init", n->a, indent + 2); break;
    case N_ASSIGN:    dump_child(out, "value", n->a, indent + 2); break;
    case N_IDXASSIGN:
        dump_child(out, "index", n->a, indent + 2);
        dump_child(out, "value", n->b, indent + 2);
        break;
    case N_IF:
        dump_child(out, "cond", n->a, indent + 2);
        dump_child(out, "then", n->b, indent + 2);
        dump_child(out, "else", n->c, indent + 2);
        break;
    case N_WHILE:
        dump_child(out, "cond", n->a, indent + 2);
        dump_child(out, "body", n->b, indent + 2);
        break;
    case N_FOR:
        dump_child(out, "from", n->a, indent + 2);
        dump_child(out, "to", n->b, indent + 2);
        dump_child(out, "body", n->c, indent + 2);
        break;
    case N_RETURN:    dump_child(out, "value", n->a, indent + 2); break;
    case N_PRINT:     dump_list(out, "args", n->list, indent + 2); break;
    case N_CALLSTMT:  ast_dump(out, n->a, indent + 2); break;
    case N_SPAWN:
    case N_CALL:      dump_list(out, "args", n->list, indent + 2); break;
    case N_SEND:
        dump_child(out, "endpoint", n->a, indent + 2);
        dump_child(out, "value", n->b, indent + 2);
        break;
    case N_RECVSTMT:  ast_dump(out, n->a, indent + 2); break;
    case N_RECV:      dump_child(out, "endpoint", n->a, indent + 2); break;
    case N_INDEX:     dump_child(out, "index", n->a, indent + 2); break;
    case N_BINOP:
        ast_dump(out, n->a, indent + 2);
        ast_dump(out, n->b, indent + 2);
        break;
    case N_UNOP:      ast_dump(out, n->a, indent + 2); break;
    case N_ARRLIT:    dump_list(out, "elems", n->list, indent + 2); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------ */
/*  Cleanup                                                                 */
/* ------------------------------------------------------------------------ */

void ast_free(Node *n)
{
    if (!n)
        return;
    list_free(n->list);
    ast_free(n->a);
    ast_free(n->b);
    ast_free(n->c);
    free(n->name);
    free(n->name2);
    free(n->name3);
    free(n->str);
    free(n);
}
