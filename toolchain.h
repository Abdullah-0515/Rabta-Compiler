/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  toolchain.h - Turning the generated .ll file into a native executable.
 *
 *  Two back-end pipelines are supported:
 *
 *    clang (default)   clang -O2 prog.ll -o prog -lpthread
 *    llc               llc -O2 -filetype=obj -relocation-model=pic prog.ll -o prog.o
 *                      clang prog.o -o prog -lpthread
 *
 *  The programs used can be overridden with the RABTA_CLANG and RABTA_LLC
 *  environment variables (e.g. RABTA_CLANG=clang-18).
 */
#ifndef RABTA_TOOLCHAIN_H
#define RABTA_TOOLCHAIN_H

typedef struct ToolchainOptions {
    int use_llc;     /* 1: llc + link, 0: clang directly       */
    int opt_level;   /* 0..3, passed as -O<n>                  */
    int verbose;     /* print every command before running it  */
} ToolchainOptions;

/* Compile `ll_path` into the executable `exe_path`. Returns 0 on success. */
int toolchain_build(const char *ll_path, const char *exe_path, const ToolchainOptions *opt);

/*
 * Run a program with arguments argv (argv[0] is looked up in PATH unless it
 * contains a '/'). Returns its exit status, 128 + signal number if it was
 * killed by a signal, or -1 if it could not be started.
 */
int toolchain_run(char *const argv[], int verbose);

#endif /* RABTA_TOOLCHAIN_H */
