/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  rabta.y - Bison (LALR(1)) grammar and the rabtac compiler driver.
 *
 *  Every grammar action builds an AST node (ast.h). When the whole program
 *  has been reduced, the action of the start rule drives the rest of the
 *  pipeline:
 *
 *      program  ->  semantic_check()   type checking + symbol table
 *               ->  protocol_check()   conversation (session type) checking
 *               ->  llvm_generate()    LLVM IR module -> program.ll
 *               ->  toolchain_build()  clang (or llc) -> native executable
 *
 *  IR is generated only if lexing, parsing and both semantic passes report
 *  zero errors. The grammar has 0 shift/reduce and 0 reduce/reduce
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
#include "llvm_gen.h"
#include "toolchain.h"

#include <unistd.h>

int  yylex(void);
void yyerror(const char *msg);

extern FILE *yyin;
extern int   lex_error_count;

const char *rabta_filename = "<stdin>";
static int  syntax_error_count = 0;
static int  opt_dump_ast = 0;     /* --ast   : print the annotated AST       */
static int  opt_check_only = 0;   /* --check : stop after the semantic pass  */
static int  opt_emit_llvm = 0;    /* -S      : only write the .ll file       */
static int  opt_keep_ll = 0;      /* --keep-ll : keep the .ll after linking  */
static int  opt_run = 0;          /* --run   : build, then run the program   */
static const char *opt_output = NULL;  /* -o FILE                            */
static ToolchainOptions tc_opt = { 0, 2, 0 };
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

/* "dir/prog.rabta" -> "prog" + suffix (a fresh malloc'd string) */
static char *derived_name(const char *path, const char *suffix)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n > 6 && strcmp(base + n - 6, ".rabta") == 0)
        n -= 6;
    char *out = malloc(n + strlen(suffix) + 1);
    if (!out) {
        fprintf(stderr, "rabtac: out of memory\n");
        exit(3);
    }
    memcpy(out, base, n);
    strcpy(out + n, suffix);
    return out;
}

static char *concat(const char *a, const char *b)
{
    char *out = malloc(strlen(a) + strlen(b) + 1);
    if (!out) {
        fprintf(stderr, "rabtac: out of memory\n");
        exit(3);
    }
    strcpy(out, a);
    strcat(out, b);
    return out;
}

/* Generate IR, build the executable and (with --run) run it. */
static int rabta_compile(Node *program)
{
    char *exe = NULL, *ll = NULL, *tmpdir = NULL;
    int status;

    if (opt_emit_llvm) {
        ll = opt_output ? strdup(opt_output) : derived_name(rabta_filename, ".ll");
        status = llvm_generate(program, rabta_filename, ll) == 0 ? 0 : 70;
        free(ll);
        return status;
    }

    if (opt_run && !opt_output) {
        /* --run without -o: build in a private temporary directory */
        const char *t = getenv("TMPDIR");
        char *templ = concat(t && *t ? t : "/tmp", "/rabtac.XXXXXX");
        if (!mkdtemp(templ)) {
            perror("rabtac: mkdtemp");
            free(templ);
            return 70;
        }
        tmpdir = templ;
        char *name = derived_name(rabta_filename, "");
        char *dir = concat(tmpdir, "/");
        exe = concat(dir, name);
        free(dir);
        free(name);
    } else {
        exe = opt_output ? strdup(opt_output) : derived_name(rabta_filename, "");
    }
    ll = concat(exe, ".ll");

    status = llvm_generate(program, rabta_filename, ll) == 0 ? 0 : 70;
    if (status == 0)
        status = toolchain_build(ll, exe, &tc_opt) == 0 ? 0 : 70;
    if (!opt_keep_ll || tmpdir)
        unlink(ll);

    if (status == 0 && opt_run) {
        fflush(stdout);
        char *argv[] = { exe, NULL };
        if (!strchr(exe, '/')) {
            char *rel = concat("./", exe);
            argv[0] = rel;
            status = toolchain_run(argv, tc_opt.verbose);
            free(rel);
        } else {
            status = toolchain_run(argv, tc_opt.verbose);
        }
        if (status < 0)
            status = 70;
    }

    if (tmpdir) {
        unlink(exe);
        rmdir(tmpdir);
        free(tmpdir);
    }
    free(ll);
    free(exe);
    return status;
}

/*
 * Called from the action of the start rule once the full AST exists.
 * Runs the semantic pass and, if it is clean, the code generator.
 */
static int rabta_execute(Node *program)
{
    if (lex_error_count > 0) {
        fprintf(stderr, "%d lexical error(s); program not compiled.\n", lex_error_count);
        return 1;
    }

    int errors = semantic_check(program);
    errors += protocol_check(program);

    if (opt_dump_ast)
        ast_dump(stdout, program, 0);

    if (errors > 0) {
        fprintf(stderr, "%d error(s) found; program not compiled.\n", errors);
        return 1;
    }
    if (opt_check_only) {
        printf("%s: OK (types and protocols check)\n", rabta_filename);
        return 0;
    }
    return rabta_compile(program);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Rabta compiler (Phase 2: LLVM IR, ahead-of-time)\n"
            "usage: %s [options] program.rabta\n"
            "  -o FILE          name of the output (default: program name without .rabta)\n"
            "  -S, --emit-llvm  only write the LLVM IR file (default name: program.ll)\n"
            "  --keep-ll        keep FILE.ll next to the executable\n"
            "  --run            build the program, then run it (exit status = the program's)\n"
            "  --llc            build with llc + link instead of clang alone\n"
            "  -O0 .. -O3       optimisation level for clang / llc (default -O2)\n"
            "  -v               print the clang / llc commands\n"
            "  --check          run lexer, parser and semantic pass only\n"
            "  --ast            print the annotated AST\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--check") == 0)
            opt_check_only = 1;
        else if (strcmp(a, "--ast") == 0)
            opt_dump_ast = 1;
        else if (strcmp(a, "-S") == 0 || strcmp(a, "--emit-llvm") == 0)
            opt_emit_llvm = 1;
        else if (strcmp(a, "--keep-ll") == 0)
            opt_keep_ll = 1;
        else if (strcmp(a, "--run") == 0)
            opt_run = 1;
        else if (strcmp(a, "--llc") == 0)
            tc_opt.use_llc = 1;
        else if (strcmp(a, "-v") == 0)
            tc_opt.verbose = 1;
        else if (a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3' && a[3] == '\0')
            tc_opt.opt_level = a[2] - '0';
        else if (strcmp(a, "-o") == 0) {
            if (++i == argc) {
                fprintf(stderr, "rabtac: -o needs a file name\n");
                return 64;
            }
            opt_output = argv[i];
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "rabtac: unknown option '%s'\n", a);
            usage(argv[0]);
            return 64;
        } else if (path) {
            fprintf(stderr, "rabtac: only one program file may be given\n");
            return 64;
        } else
            path = a;
    }

    if (!path) {
        usage(argv[0]);
        return 64;
    }
    if (opt_emit_llvm && opt_run) {
        fprintf(stderr, "rabtac: -S and --run cannot be combined\n");
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
        fprintf(stderr, "%d syntax error(s); program not compiled.\n",
                syntax_error_count ? syntax_error_count : 1);
        return 1;
    }

    /* the AST was freed by Bison's %destructor for the start symbol */
    return rabta_status;
}
