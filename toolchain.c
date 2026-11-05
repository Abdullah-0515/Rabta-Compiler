/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  toolchain.c - Runs clang / llc on the generated IR (see toolchain.h).
 */
#include "toolchain.h"

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *tool(const char *env, const char *fallback)
{
    const char *t = getenv(env);
    return t && *t ? t : fallback;
}

int toolchain_run(char *const argv[], int verbose)
{
    pid_t pid;
    int status;

    if (verbose) {
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, i ? " %s" : "%s", argv[i]);
        fputc('\n', stderr);
    }

    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "rabtac: cannot run '%s': %s\n", argv[0], strerror(rc));
        return -1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("rabtac: waitpid");
            return -1;
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static int run_tool(char *const argv[], int verbose)
{
    int rc = toolchain_run(argv, verbose);
    if (rc == -1)
        fprintf(stderr, "rabtac: is it installed? (sudo apt install llvm clang)\n");
    else if (rc != 0)
        fprintf(stderr, "rabtac: '%s' failed with status %d\n", argv[0], rc);
    return rc;
}

int toolchain_build(const char *ll_path, const char *exe_path, const ToolchainOptions *opt)
{
    char olevel[8];
    char *clang = (char *)tool("RABTA_CLANG", "clang");

    snprintf(olevel, sizeof olevel, "-O%d", opt->opt_level);

    if (!opt->use_llc) {
        char *argv[] = { clang, olevel, "-Wno-override-module", (char *)ll_path,
                         "-o", (char *)exe_path, "-lpthread", NULL };
        return run_tool(argv, opt->verbose) == 0 ? 0 : 1;
    }

    /* llc: IR -> object file, then link it with clang (the C driver) */
    size_t n = strlen(exe_path) + 3;
    char *obj = malloc(n);
    if (!obj) {
        fprintf(stderr, "rabtac: out of memory\n");
        return 1;
    }
    snprintf(obj, n, "%s.o", exe_path);

    char *llc_argv[] = { (char *)tool("RABTA_LLC", "llc"), olevel, "-filetype=obj",
                         "-relocation-model=pic", (char *)ll_path, "-o", obj, NULL };
    int status = run_tool(llc_argv, opt->verbose) == 0 ? 0 : 1;
    if (status == 0) {
        char *link_argv[] = { clang, obj, "-o", (char *)exe_path, "-lpthread", NULL };
        status = run_tool(link_argv, opt->verbose) == 0 ? 0 : 1;
    }
    unlink(obj);
    free(obj);
    return status;
}
