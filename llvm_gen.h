/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  llvm_gen.h - LLVM IR code generator (public interface).
 *
 *  llvm_generate() takes a program that passed semantic_check() and
 *  protocol_check() and writes one LLVM IR module (a textual .ll file) for
 *  it, built with the LLVM C API (LLVMContext, LLVMModule, LLVMBuilder).
 *
 *  The module is self-contained: besides the code of every kaam and banda it
 *  contains the small actor/channel runtime (thread registry and bounded
 *  message queues), written directly in IR on top of declared libc and
 *  POSIX-thread functions such as
 *
 *      declare i32 @pthread_create(ptr, ptr, ptr, ptr)
 *      declare i32 @pthread_mutex_lock(ptr)
 *      declare i32 @pthread_cond_wait(ptr, ptr)
 *
 *  so the .ll file only needs `clang file.ll -lpthread` to become a native
 *  executable (see toolchain.h).
 */
#ifndef RABTA_LLVM_GEN_H
#define RABTA_LLVM_GEN_H

#include "ast.h"

/*
 * Generate LLVM IR for `program` and write it to `ll_path`.
 * `source_name` is embedded in the module and used in runtime error
 * messages ("file:line: runtime error: ...").
 * Returns 0 on success; on failure prints a message to stderr and returns
 * non-zero (an LLVM verifier failure is an internal compiler error).
 */
int llvm_generate(Node *program, const char *source_name, const char *ll_path);

#endif /* RABTA_LLVM_GEN_H */
