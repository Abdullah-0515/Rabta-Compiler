/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  rabta.y - Bison (LALR(1)) grammar.
 *
 *  Every grammar action builds an AST node (ast.h). When the whole program
 *  has been reduced, the action of the start rule drives the rest of the
 *  pipeline:
 *
 *      program  ->  semantic_check()   type checking + symbol table
 *               ->  protocol_check()   conversation (session type) checking
 *               ->  interpret()        tree-walking execution with actors
 *
 *  The program is executed only if lexing, parsing and both semantic passes
 *  report zero errors. The grammar has 0 shift/reduce and 0 reduce/reduce
 *  conflicts (bison reports none; see `make` output).
 */

%code requires {
#include "ast.h"
}

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "semantic.h"
#include "protocol.h"
#include "interpreter.h"

int  yylex(void);
void yyerror(const char *msg);

extern FILE *yyin;
extern int   lex_error_count;

const char *rabta_filename = "<stdin>";
static int  syntax_error_count = 0;
static int  opt_dump_ast = 0;     /* --ast   : print the annotated AST       */
static int  opt_check_only = 0;   /* --check : stop after the semantic pass  */
static int  rabta_status = 0;     /* process exit status                     */
static Node *ast_root = NULL;

static int rabta_execute(Node *program);
%}

%locations
%define parse.error detailed

%union {
    int64_t   ival;
    char     *sval;
    Node     *node;
    NodeList *list;
}

/* keywords */
%token SHURU "shuru" KHATAM "khatam" AGAR "agar" WARNA "warna"
%token JAB_TAK "jab_tak" CHAL "chal" GHOOM "ghoom" RAKHO "rakho"
%token THALI "thali" KAAM "kaam" BULAO "bulao" WAAPIS "waapis"
%token DIKHAO "dikhao" BAATCHEET "baatcheet" BANDA "banda" NALI "nali"
%token PAIDA "paida" BOLO "bolo" SUNO "suno" SIRA "sira" BOLEGA "bolega"
%token I64 "i64"

/* operators */
%token ARROW "->" EQ "==" NE "!=" LE "<=" GE ">=" AND "&&" OR "||"

/* literals and names */
%token <ival> INT_LIT    "integer"
%token <sval> IDENT      "identifier"
%token <sval> STRING_LIT "string"

%type <node> program item protocol_decl protocol_step actor_decl func_decl
%type <node> param block stmt else_part endpoint recv expr primary call print_arg
%type <list> items protocol_steps params_opt params stmts args_opt args print_args

/* free partial trees when a syntax error aborts the parse */
%destructor { ast_free($$); } <node>
%destructor { list_free($$); } <list>
%destructor { free($$); } <sval>

/* precedence: lowest first */
%left OR
%left AND
%nonassoc EQ NE
%nonassoc '<' '>' LE GE
%left '+' '-'
%left '*' '/' '%'
%precedence UMINUS '!'

%start program

%%

/* ======================= top level ======================= */

program
    : items                       { ast_root = ast_program($1);
                                    $$ = ast_root;
                                    rabta_status = rabta_execute(ast_root); }
    ;

items
    : %empty                      { $$ = list_new(); }
    | items item                  { $$ = list_append($1, $2); }
    ;

item
    : protocol_decl
    | actor_decl
    | func_decl
    ;

/* baatcheet Hisaab shuru  client bolega i64  server bolega i64  khatam */
protocol_decl
    : BAATCHEET IDENT SHURU protocol_steps KHATAM
                                  { $$ = ast_protocol(@1.first_line, $2, $4); }
    ;

protocol_steps
    : protocol_step               { $$ = list_append(NULL, $1); }
    | protocol_steps protocol_step { $$ = list_append($1, $2); }
    ;

protocol_step
    : IDENT BOLEGA I64            { $$ = ast_pstep(@1.first_line, $1, type_make(TY_I64)); }
    | IDENT BOLEGA THALI '[' INT_LIT ']'
                                  { $$ = ast_pstep(@1.first_line, $1, type_array($5)); }
    ;

/* banda server(sira s : Hisaab.server) shuru ... khatam */
actor_decl
    : BANDA IDENT '(' params_opt ')' block
                                  { $$ = ast_actor(@1.first_line, $2, $4, $6); }
    ;

/* kaam add(a, b) shuru ... khatam */
func_decl
    : KAAM IDENT '(' params_opt ')' block
                                  { $$ = ast_func(@1.first_line, $2, $4, $6); }
    ;

params_opt
    : %empty                      { $$ = list_new(); }
    | params                      { $$ = $1; }
    ;

params
    : param                       { $$ = list_append(NULL, $1); }
    | params ',' param            { $$ = list_append($1, $3); }
    ;

param
    : IDENT                       { $$ = ast_param(@1.first_line, PARAM_I64, $1, NULL, NULL); }
    | IDENT ':' I64               { $$ = ast_param(@1.first_line, PARAM_I64, $1, NULL, NULL); }
    | THALI IDENT                 { $$ = ast_param(@1.first_line, PARAM_ARRAY, $2, NULL, NULL); }
    | SIRA IDENT ':' IDENT '.' IDENT
                                  { $$ = ast_param(@1.first_line, PARAM_ENDPOINT, $2, $4, $6); }
    ;

block
    : SHURU stmts KHATAM          { $$ = ast_block(@1.first_line, $2);
                                    $$->ival = @3.first_line; }
    ;

/* ======================= statements ======================= */

stmts
    : %empty                      { $$ = list_new(); }
    | stmts stmt                  { $$ = list_append($1, $2); }
    ;

stmt
    /* variables */
    : RAKHO IDENT '=' expr        { $$ = ast_vardecl(@1.first_line, $2, $4); }
    | RAKHO IDENT '=' recv        { $$ = ast_vardecl(@1.first_line, $2, $4); }
    | THALI IDENT '[' INT_LIT ']' { $$ = ast_arrdecl(@1.first_line, $2, $4, NULL); }
    | THALI IDENT '[' INT_LIT ']' '=' '[' args_opt ']'
                                  { $$ = ast_arrdecl(@1.first_line, $2, $4,
                                                     ast_arrlit(@7.first_line, $8)); }
    | THALI IDENT '[' INT_LIT ']' '=' recv
                                  { $$ = ast_arrdecl(@1.first_line, $2, $4, $7); }
    | IDENT '=' expr              { $$ = ast_assign(@1.first_line, $1, $3); }
    | IDENT '=' recv              { $$ = ast_assign(@1.first_line, $1, $3); }
    | IDENT '[' expr ']' '=' expr { $$ = ast_idxassign(@1.first_line, $1, $3, $6); }
    | IDENT '[' expr ']' '=' recv { $$ = ast_idxassign(@1.first_line, $1, $3, $6); }

    /* control flow */
    | AGAR expr block else_part   { $$ = ast_if(@1.first_line, $2, $3, $4); }
    | JAB_TAK expr CHAL stmts KHATAM
                                  { Node *body = ast_block(@3.first_line, $4);
                                    body->ival = @5.first_line;
                                    $$ = ast_while(@1.first_line, $2, body); }
    | GHOOM '[' IDENT ':' expr ARROW expr ']' CHAL stmts KHATAM
                                  { Node *body = ast_block(@9.first_line, $10);
                                    body->ival = @11.first_line;
                                    $$ = ast_for(@1.first_line, $3, $5, $7, body); }
    | WAAPIS expr                 { $$ = ast_return(@1.first_line, $2); }

    /* output and calls */
    | DIKHAO '(' print_args ')'   { $$ = ast_print(@1.first_line, $3); }
    | call                        { $$ = ast_callstmt(@1.first_line, $1); }

    /* concurrency */
    | NALI IDENT ':' IDENT        { $$ = ast_chandecl(@1.first_line, $2, $4); }
    | PAIDA IDENT '(' args_opt ')'
                                  { $$ = ast_spawn(@1.first_line, $2, $4); }
    | BOLO endpoint ',' expr      { $$ = ast_send(@1.first_line, $2, $4); }
    | recv                        { $$ = ast_recvstmt(@1.first_line, $1); }
    ;

else_part
    : %empty                      { $$ = NULL; }
    | WARNA block                 { $$ = $2; }
    | WARNA AGAR expr block else_part
                                  { $$ = ast_if(@2.first_line, $3, $4, $5); }
    ;

/* suno is only allowed as a whole initialiser/assigned value or as a
   statement on its own, so the order of protocol actions is always the
   textual order of statements. */
recv
    : SUNO endpoint               { $$ = ast_recv(@1.first_line, $2); }
    ;

endpoint
    : IDENT                       { $$ = ast_var(@1.first_line, $1); }
    | IDENT '.' IDENT             { $$ = ast_endpoint(@1.first_line, $1, $3); }
    ;

print_args
    : print_arg                   { $$ = list_append(NULL, $1); }
    | print_args ',' print_arg    { $$ = list_append($1, $3); }
    ;

print_arg
    : expr                        { $$ = $1; }
    | STRING_LIT                  { $$ = ast_str(@1.first_line, $1); }
    ;

/* ======================= expressions ======================= */

expr
    : expr OR expr                { $$ = ast_binop(@2.first_line, OP_OR,  $1, $3); }
    | expr AND expr               { $$ = ast_binop(@2.first_line, OP_AND, $1, $3); }
    | expr EQ expr                { $$ = ast_binop(@2.first_line, OP_EQ,  $1, $3); }
    | expr NE expr                { $$ = ast_binop(@2.first_line, OP_NE,  $1, $3); }
    | expr '<' expr               { $$ = ast_binop(@2.first_line, OP_LT,  $1, $3); }
    | expr LE expr                { $$ = ast_binop(@2.first_line, OP_LE,  $1, $3); }
    | expr '>' expr               { $$ = ast_binop(@2.first_line, OP_GT,  $1, $3); }
    | expr GE expr                { $$ = ast_binop(@2.first_line, OP_GE,  $1, $3); }
    | expr '+' expr               { $$ = ast_binop(@2.first_line, OP_ADD, $1, $3); }
    | expr '-' expr               { $$ = ast_binop(@2.first_line, OP_SUB, $1, $3); }
    | expr '*' expr               { $$ = ast_binop(@2.first_line, OP_MUL, $1, $3); }
    | expr '/' expr               { $$ = ast_binop(@2.first_line, OP_DIV, $1, $3); }
    | expr '%' expr               { $$ = ast_binop(@2.first_line, OP_MOD, $1, $3); }
    | '-' expr %prec UMINUS       { $$ = ast_unop(@1.first_line, OP_NEG, $2); }
    | '!' expr                    { $$ = ast_unop(@1.first_line, OP_NOT, $2); }
    | primary                     { $$ = $1; }
    ;

primary
    : INT_LIT                     { $$ = ast_int(@1.first_line, $1); }
    | IDENT                       { $$ = ast_var(@1.first_line, $1); }
    | IDENT '[' expr ']'          { $$ = ast_index(@1.first_line, $1, $3); }
    | IDENT '.' IDENT             { $$ = ast_endpoint(@1.first_line, $1, $3); }
    | '(' expr ')'                { $$ = $2; }
    | call                        { $$ = $1; }
    ;

call
    : BULAO IDENT '(' args_opt ')' { $$ = ast_call(@1.first_line, $2, $4); }
    ;

args_opt
    : %empty                      { $$ = list_new(); }
    | args                        { $$ = $1; }
    ;

args
    : expr                        { $$ = list_append(NULL, $1); }
    | args ',' expr               { $$ = list_append($1, $3); }
    ;

%%

void yyerror(const char *msg)
{
    fprintf(stderr, "%s:%d: %s\n",
            rabta_filename, yylloc.first_line, msg);
    syntax_error_count++;
}

/*
 * Called from the action of the start rule once the full AST exists.
 * Runs the semantic pass and, if it is clean, the interpreter.
 */
static int rabta_execute(Node *program)
{
    if (lex_error_count > 0) {
        fprintf(stderr, "%d lexical error(s); program not executed.\n", lex_error_count);
        return 1;
    }

    int errors = semantic_check(program);
    errors += protocol_check(program);

    if (opt_dump_ast)
        ast_dump(stdout, program, 0);

    if (errors > 0) {
        fprintf(stderr, "%d error(s) found; program not executed.\n", errors);
        return 1;
    }
    if (opt_check_only) {
        printf("%s: OK (types and protocols check)\n", rabta_filename);
        return 0;
    }
    return interpret(program);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Rabta interpreter (Phase 1)\n"
            "usage: %s [--check] [--ast] program.rabta\n"
            "  --check   run lexer, parser and semantic pass only\n"
            "  --ast     print the annotated AST before running\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0)
            opt_check_only = 1;
        else if (strcmp(argv[i], "--ast") == 0)
            opt_dump_ast = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "rabta: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 64;
        } else if (path) {
            fprintf(stderr, "rabta: only one program file may be given\n");
            return 64;
        } else
            path = argv[i];
    }

    if (!path) {
        usage(argv[0]);
        return 64;
    }

    yyin = fopen(path, "r");
    if (!yyin) {
        perror(path);
        return 66;
    }
    rabta_filename = path;

    int parse_result = yyparse();
    fclose(yyin);

    if (parse_result != 0 || syntax_error_count > 0) {
        fprintf(stderr, "%d syntax error(s); program not executed.\n",
                syntax_error_count ? syntax_error_count : 1);
        return 1;
    }

    /* the AST was freed by Bison's %destructor for the start symbol */
    return rabta_status;
}
