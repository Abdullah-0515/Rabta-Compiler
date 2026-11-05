/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  semantic.h - Semantic pass, part 1: symbol tables and type checking.
 *
 *  semantic_check() resolves every name, checks types, argument counts and
 *  return rules, and annotates the AST for the protocol checker and the
 *  interpreter:
 *    - every variable reference gets a frame slot   (Node.slot)
 *    - every function/actor gets its frame size     (Node.nslots)
 *    - calls / spawns / nali / endpoints get Node.ref to their declaration
 *    - expressions get their type                   (Node.ty)
 */
#ifndef RABTA_SEMANTIC_H
#define RABTA_SEMANTIC_H

#include "ast.h"

#define RABTA_MAX_ARRAY 10000000   /* largest allowed thali size */

extern const char *rabta_filename;

/* Report a compile-time error at a source line ("file:line: error: ..."). */
void sema_error(int line, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Number of errors reported through sema_error so far. */
int sema_error_total(void);

/* Look up top-level declarations by name (NULL if absent). */
Node *find_protocol(Node *program, const char *name);
Node *find_function(Node *program, const char *name);
Node *find_actor(Node *program, const char *name);

/* Run type checking over the whole program. Returns the number of errors. */
int semantic_check(Node *program);

#endif /* RABTA_SEMANTIC_H */
