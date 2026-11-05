/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  rabta.l - Flex lexer.
 *
 *  Tokenizes keywords, identifiers, integer and string literals, operators
 *  and // comments. Line numbers are tracked with yylineno and copied into
 *  the Bison location of every token so that all later errors can report
 *  the source line.
 */

%option noyywrap nounput noinput yylineno

%{
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "rabta.tab.h"

extern const char *rabta_filename;   /* defined in rabta.y */
int lex_error_count = 0;

#define YY_USER_ACTION \
    yylloc.first_line = yylloc.last_line = yylineno;

static void lex_error(const char *msg, const char *text)
{
    fprintf(stderr, "%s:%d: error: %s '%s'\n",
            rabta_filename ? rabta_filename : "<input>", yylineno, msg, text);
    lex_error_count++;
}

/* Copy a string literal without its quotes, processing escape sequences. */
static char *unescape(const char *s, size_t len)
{
    char *out = malloc(len + 1);
    size_t j = 0;
    if (!out) {
        fprintf(stderr, "rabta: out of memory\n");
        exit(3);
    }
    for (size_t i = 1; i + 1 < len; i++) {
        if (s[i] == '\\' && i + 2 < len) {
            i++;
            switch (s[i]) {
            case 'n':  out[j++] = '\n'; break;
            case 't':  out[j++] = '\t'; break;
            case '\\': out[j++] = '\\'; break;
            case '"':  out[j++] = '"';  break;
            default:   out[j++] = '\\'; out[j++] = s[i]; break;
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}
%}

DIGIT   [0-9]
IDSTART [A-Za-z_]
IDCHAR  [A-Za-z0-9_]

%%

[ \t\r\n]+              { /* whitespace */ }
"//"[^\n]*              { /* comment */ }

"shuru"                 { return SHURU; }
"khatam"                { return KHATAM; }
"agar"                  { return AGAR; }
"warna"                 { return WARNA; }
"jab_tak"               { return JAB_TAK; }
"chal"                  { return CHAL; }
"ghoom"                 { return GHOOM; }
"rakho"                 { return RAKHO; }
"thali"                 { return THALI; }
"kaam"                  { return KAAM; }
"bulao"                 { return BULAO; }
"waapis"                { return WAAPIS; }
"dikhao"                { return DIKHAO; }
"baatcheet"             { return BAATCHEET; }
"banda"                 { return BANDA; }
"nali"                  { return NALI; }
"paida"                 { return PAIDA; }
"bolo"                  { return BOLO; }
"suno"                  { return SUNO; }
"sira"                  { return SIRA; }
"bolega"                { return BOLEGA; }
"i64"                   { return I64; }

{DIGIT}+                {
                            errno = 0;
                            long long v = strtoll(yytext, NULL, 10);
                            if (errno == ERANGE) {
                                lex_error("integer literal does not fit in i64:", yytext);
                                v = 0;
                            }
                            yylval.ival = (int64_t)v;
                            return INT_LIT;
                        }
{DIGIT}+{IDSTART}{IDCHAR}* {
                            lex_error("invalid number or identifier", yytext);
                            yylval.ival = 0;
                            return INT_LIT;
                        }

{IDSTART}{IDCHAR}*      { yylval.sval = strdup(yytext); return IDENT; }

\"([^"\\\n]|\\.)*\"     { yylval.sval = unescape(yytext, yyleng); return STRING_LIT; }
\"([^"\\\n]|\\.)*       {
                            lex_error("unterminated string literal", yytext);
                            yylval.sval = strdup("");
                            return STRING_LIT;
                        }

"->"                    { return ARROW; }
"=="                    { return EQ; }
"!="                    { return NE; }
"<="                    { return LE; }
">="                    { return GE; }
"&&"                    { return AND; }
"||"                    { return OR; }

"+"|"-"|"*"|"/"|"%"     { return yytext[0]; }
"<"|">"|"="|"!"         { return yytext[0]; }
"("|")"|"["|"]"         { return yytext[0]; }
","|":"|"."             { return yytext[0]; }

.                       { lex_error("unexpected character", yytext); }

%%
