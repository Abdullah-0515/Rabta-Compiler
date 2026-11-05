/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  interpreter.h - Tree-walking interpreter.
 *
 *  interpret() executes a program that passed the semantic pass. It runs
 *  main() on the calling thread, every banda on its own thread (runtime.c),
 *  waits for all actors to finish and returns main's waapis value, which
 *  becomes the process exit status.
 */
#ifndef RABTA_INTERPRETER_H
#define RABTA_INTERPRETER_H

#include "ast.h"

int interpret(Node *program);

#endif /* RABTA_INTERPRETER_H */
