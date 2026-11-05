/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 2 (LLVM IR ahead-of-time compiler)
 * ============================================================================
 *
 *  llvm_actor.c - Actors (banda), channels (nali) and messages (bolo / suno).
 *
 *  The actor runtime of Phase 1 (runtime.c) is generated here directly in
 *  LLVM IR, on top of declared POSIX-thread functions:
 *
 *    declare i32 @pthread_create(ptr, ptr, ptr, ptr)
 *    declare i32 @pthread_join(i64, ptr)
 *    declare i32 @pthread_mutex_lock(ptr) / @pthread_mutex_unlock(ptr)
 *    declare i32 @pthread_cond_wait(ptr, ptr) / @pthread_cond_signal(ptr)
 *
 *  Data layout
 *    %rabta.msg   = type { i64, ptr }
 *                   i64 message: { value, null }
 *                   thali message: { length, heap copy of the elements }
 *    %rabta.queue = type { [16 x %rabta.msg],  ; ring buffer
 *                          i64, i64,           ; head, count
 *                          [8 x i64],          ; pthread_mutex_t lock
 *                          [8 x i64],          ; pthread_cond_t  not_empty
 *                          [8 x i64] }         ; pthread_cond_t  not_full
 *    %rabta.chan  = type { [2 x %rabta.queue],   ; indexed by RECEIVING role
 *                          ptr }               ; next channel (registry)
 *
 *    (glibc's pthread_mutex_t is 40 bytes and pthread_cond_t 48 bytes; each
 *     gets 64 bytes, 8-byte aligned.)
 *
 *  Runtime functions generated in the module
 *    @rabta.chan_new(line)            nali: calloc + mutex/cond init
 *    @rabta.send(chan, to, v, data)   bolo: waits while the queue is full
 *    @rabta.recv(chan, role, out)     suno: waits until a message arrives
 *    @rabta.spawn(fn, arg, line)      paida: pthread_create with a 16 MiB
 *                                     stack, thread id kept in a registry
 *    @rabta.join_all()                pthread_join every actor (also ones
 *                                     spawned by actors) after main ends
 *    @rabta.init()                    initialises the registry lock
 *    @rabta.shutdown()                destroys and frees every channel and
 *                                     the thread registry after join_all
 *
 *  A banda becomes `define internal ptr @banda.NAME(ptr %args)`, the start
 *  routine of its thread. paida packs the arguments (i64 values and channel
 *  pointers) into a heap struct that the actor unpacks and frees, so actors
 *  never share memory; thali messages are deep-copied on bolo.
 */
#include "llvm_gen_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_CAPACITY 16
#define ACTOR_STACK    (16LL * 1024 * 1024)

/* field numbers of %rabta.queue */
enum { Q_BUF, Q_HEAD, Q_COUNT, Q_LOCK, Q_NOT_EMPTY, Q_NOT_FULL };

/* ------------------------------------------------------------------------ */
/*  Helpers                                                                 */
/* ------------------------------------------------------------------------ */

static LLVMValueRef rt(Gen *g, const char *name)
{
    return LLVMGetNamedFunction(g->mod, name);
}

static LLVMValueRef call_pthread(Gen *g, const char *name, LLVMValueRef *args, unsigned n)
{
    return gen_call(g, gen_libc(g, name), args, n, "");
}

static LLVMValueRef new_global(Gen *g, LLVMTypeRef t, const char *name)
{
    LLVMValueRef gv = LLVMAddGlobal(g->mod, t, name);
    LLVMSetInitializer(gv, LLVMConstNull(t));
    LLVMSetLinkage(gv, LLVMInternalLinkage);
    return gv;
}

/* if (p == NULL) rabta.fail(line, "out of memory") */
static void check_alloc(Gen *g, LLVMValueRef p, LLVMValueRef line)
{
    LLVMValueRef isnull = LLVMBuildIsNull(g->b, p, "oom");
    LLVMBasicBlockRef fail = gen_block(g, "oom.fail");
    LLVMBasicBlockRef ok = gen_block(g, "oom.ok");
    LLVMBuildCondBr(g->b, isnull, fail, ok);
    LLVMPositionBuilderAtEnd(g->b, fail);
    LLVMValueRef args[] = { line, gen_cstr(g, "out of memory") };
    gen_call(g, rt(g, "rabta.fail"), args, 2, "");
    LLVMBuildUnreachable(g->b);
    LLVMPositionBuilderAtEnd(g->b, ok);
}

/* Pointer to queue[role] of channel c (role is an i32 value). */
static LLVMValueRef queue_ptr(Gen *g, LLVMValueRef c, LLVMValueRef role)
{
    LLVMValueRef idx[] = { gen_i32(g, 0), gen_i32(g, 0), role };
    return LLVMBuildInBoundsGEP2(g->b, g->t_chan, c, idx, 3, "queue");
}

static LLVMValueRef qfield(Gen *g, LLVMValueRef q, unsigned field, const char *name)
{
    return LLVMBuildStructGEP2(g->b, g->t_queue, q, field, name);
}

/* Address of ring-buffer entry `slot` of queue q. */
static LLVMValueRef queue_entry(Gen *g, LLVMValueRef q, LLVMValueRef slot)
{
    LLVMValueRef idx[] = { gen_i32(g, 0), gen_i32(g, Q_BUF), slot };
    return LLVMBuildInBoundsGEP2(g->b, g->t_queue, q, idx, 3, "entry");
}

/*
 *   br label %wait.cond
 * wait.cond:
 *   %count = load i64, ptr %count.ptr
 *   %w = icmp eq i64 %count, <limit>
 *   br i1 %w, label %wait.body, label %<after>
 * wait.body:
 *   call i32 @pthread_cond_wait(ptr %cond, ptr %lock)
 *   br label %wait.cond
 */
static void gen_wait_while_count(Gen *g, LLVMValueRef count_ptr, int64_t limit,
                                 LLVMValueRef cond, LLVMValueRef lock)
{
    LLVMBasicBlockRef wcond = gen_block(g, "wait.cond");
    LLVMBasicBlockRef wbody = gen_block(g, "wait.body");
    LLVMBasicBlockRef after = gen_block(g, "ready");

    LLVMBuildBr(g->b, wcond);
    LLVMPositionBuilderAtEnd(g->b, wcond);
    LLVMValueRef count = LLVMBuildLoad2(g->b, g->t_i64, count_ptr, "count");
    LLVMValueRef w = LLVMBuildICmp(g->b, LLVMIntEQ, count, gen_i64(g, limit), "must.wait");
    LLVMBuildCondBr(g->b, w, wbody, after);

    LLVMPositionBuilderAtEnd(g->b, wbody);
    LLVMValueRef wa[] = { cond, lock };
    call_pthread(g, "pthread_cond_wait", wa, 2);
    LLVMBuildBr(g->b, wcond);

    LLVMPositionBuilderAtEnd(g->b, after);
}

/* ------------------------------------------------------------------------ */
/*  Runtime in IR                                                           */
/* ------------------------------------------------------------------------ */

static void gen_channel_runtime(Gen *g)
{
    LLVMValueRef null = LLVMConstPointerNull(g->t_ptr);

    /* ptr @rabta.chan_new(i32 line) */
    {
        LLVMTypeRef ps[] = { g->t_i32 };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.chan_new", g->t_ptr, ps, 1);
        LLVMValueRef ca[] = { gen_i64(g, 1), LLVMSizeOf(g->t_chan) };
        LLVMValueRef c = gen_call(g, gen_libc(g, "calloc"), ca, 2, "chan");
        check_alloc(g, c, LLVMGetParam(fn, 0));
        for (int role = 0; role < 2; role++) {
            LLVMValueRef q = queue_ptr(g, c, gen_i32(g, role));
            LLVMValueRef m[] = { qfield(g, q, Q_LOCK, "lock"), null };
            call_pthread(g, "pthread_mutex_init", m, 2);
            LLVMValueRef e[] = { qfield(g, q, Q_NOT_EMPTY, "not_empty"), null };
            call_pthread(g, "pthread_cond_init", e, 2);
            LLVMValueRef f[] = { qfield(g, q, Q_NOT_FULL, "not_full"), null };
            call_pthread(g, "pthread_cond_init", f, 2);
        }
        /* push the channel on the registry list (freed by rabta.shutdown) */
        LLVMValueRef lock = LLVMGetNamedGlobal(g->mod, "rabta.threads_lock");
        LLVMValueRef head = LLVMGetNamedGlobal(g->mod, "rabta.channels");
        call_pthread(g, "pthread_mutex_lock", &lock, 1);
        LLVMBuildStore(g->b, LLVMBuildLoad2(g->b, g->t_ptr, head, "first"),
                       LLVMBuildStructGEP2(g->b, g->t_chan, c, 1, "next.ptr"));
        LLVMBuildStore(g->b, c, head);
        call_pthread(g, "pthread_mutex_unlock", &lock, 1);
        LLVMBuildRet(g->b, c);
    }

    /* void @rabta.send(ptr chan, i32 to_role, i64 value, ptr data) */
    {
        LLVMTypeRef ps[] = { g->t_ptr, g->t_i32, g->t_i64, g->t_ptr };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.send", g->t_void, ps, 4);
        LLVMValueRef q = queue_ptr(g, LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
        LLVMValueRef lock = qfield(g, q, Q_LOCK, "lock");
        LLVMValueRef headp = qfield(g, q, Q_HEAD, "head.ptr");
        LLVMValueRef countp = qfield(g, q, Q_COUNT, "count.ptr");

        call_pthread(g, "pthread_mutex_lock", &lock, 1);
        gen_wait_while_count(g, countp, QUEUE_CAPACITY, qfield(g, q, Q_NOT_FULL, "not_full"), lock);

        LLVMValueRef head = LLVMBuildLoad2(g->b, g->t_i64, headp, "head");
        LLVMValueRef count = LLVMBuildLoad2(g->b, g->t_i64, countp, "count");
        LLVMValueRef tail = LLVMBuildURem(g->b, LLVMBuildAdd(g->b, head, count, "pos"),
                                          gen_i64(g, QUEUE_CAPACITY), "tail");
        LLVMValueRef ent = queue_entry(g, q, tail);
        LLVMBuildStore(g->b, LLVMGetParam(fn, 2), LLVMBuildStructGEP2(g->b, g->t_msg, ent, 0, "msg.value"));
        LLVMBuildStore(g->b, LLVMGetParam(fn, 3), LLVMBuildStructGEP2(g->b, g->t_msg, ent, 1, "msg.data"));
        LLVMBuildStore(g->b, LLVMBuildAdd(g->b, count, gen_i64(g, 1), "count.inc"), countp);

        LLVMValueRef ne = qfield(g, q, Q_NOT_EMPTY, "not_empty");
        call_pthread(g, "pthread_cond_signal", &ne, 1);
        call_pthread(g, "pthread_mutex_unlock", &lock, 1);
        LLVMBuildRetVoid(g->b);
    }

    /* i64 @rabta.recv(ptr chan, i32 role, ptr out_data) */
    {
        LLVMTypeRef ps[] = { g->t_ptr, g->t_i32, g->t_ptr };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.recv", g->t_i64, ps, 3);
        LLVMValueRef q = queue_ptr(g, LLVMGetParam(fn, 0), LLVMGetParam(fn, 1));
        LLVMValueRef lock = qfield(g, q, Q_LOCK, "lock");
        LLVMValueRef headp = qfield(g, q, Q_HEAD, "head.ptr");
        LLVMValueRef countp = qfield(g, q, Q_COUNT, "count.ptr");

        call_pthread(g, "pthread_mutex_lock", &lock, 1);
        gen_wait_while_count(g, countp, 0, qfield(g, q, Q_NOT_EMPTY, "not_empty"), lock);

        LLVMValueRef head = LLVMBuildLoad2(g->b, g->t_i64, headp, "head");
        LLVMValueRef count = LLVMBuildLoad2(g->b, g->t_i64, countp, "count");
        LLVMValueRef ent = queue_entry(g, q, head);
        LLVMValueRef v = LLVMBuildLoad2(g->b, g->t_i64,
                                        LLVMBuildStructGEP2(g->b, g->t_msg, ent, 0, "msg.value"), "value");
        LLVMValueRef d = LLVMBuildLoad2(g->b, g->t_ptr,
                                        LLVMBuildStructGEP2(g->b, g->t_msg, ent, 1, "msg.data"), "data");
        LLVMValueRef next = LLVMBuildURem(g->b, LLVMBuildAdd(g->b, head, gen_i64(g, 1), "pos"),
                                          gen_i64(g, QUEUE_CAPACITY), "head.next");
        LLVMBuildStore(g->b, next, headp);
        LLVMBuildStore(g->b, LLVMBuildSub(g->b, count, gen_i64(g, 1), "count.dec"), countp);

        LLVMValueRef nf = qfield(g, q, Q_NOT_FULL, "not_full");
        call_pthread(g, "pthread_cond_signal", &nf, 1);
        call_pthread(g, "pthread_mutex_unlock", &lock, 1);
        LLVMBuildStore(g->b, d, LLVMGetParam(fn, 2));
        LLVMBuildRet(g->b, v);
    }
}

static void gen_thread_runtime(Gen *g)
{
    LLVMTypeRef t_lock = LLVMArrayType(g->t_i64, 8);
    LLVMValueRef null = LLVMConstPointerNull(g->t_ptr);
    LLVMValueRef g_lock = new_global(g, t_lock, "rabta.threads_lock");
    LLVMValueRef g_threads = new_global(g, g->t_ptr, "rabta.threads");     /* i64 array */
    LLVMValueRef g_n = new_global(g, g->t_i64, "rabta.nthreads");
    LLVMValueRef g_cap = new_global(g, g->t_i64, "rabta.capthreads");
    LLVMValueRef g_joined = new_global(g, g->t_i64, "rabta.njoined");
    LLVMValueRef g_chans = new_global(g, g->t_ptr, "rabta.channels");   /* list */

    /* void @rabta.init() */
    {
        gen_new_runtime_fn(g, "rabta.init", g->t_void, NULL, 0);
        LLVMValueRef a[] = { g_lock, null };
        call_pthread(g, "pthread_mutex_init", a, 2);
        LLVMBuildRetVoid(g->b);
    }

    /* void @rabta.spawn(ptr start_routine, ptr arg, i32 line) */
    {
        LLVMTypeRef ps[] = { g->t_ptr, g->t_ptr, g->t_i32 };
        LLVMValueRef fn = gen_new_runtime_fn(g, "rabta.spawn", g->t_void, ps, 3);
        LLVMValueRef line = LLVMGetParam(fn, 2);
        LLVMValueRef attr = LLVMBuildAlloca(g->b, LLVMArrayType(g->t_i64, 8), "attr");
        LLVMValueRef tid = LLVMBuildAlloca(g->b, g->t_i64, "tid");

        call_pthread(g, "pthread_attr_init", &attr, 1);
        LLVMValueRef ss[] = { attr, gen_i64(g, ACTOR_STACK) };
        call_pthread(g, "pthread_attr_setstacksize", ss, 2);

        /* hold the registry lock while creating, so join_all never misses
           a thread that is being started */
        call_pthread(g, "pthread_mutex_lock", &g_lock, 1);
        LLVMValueRef ca[] = { tid, attr, LLVMGetParam(fn, 0), LLVMGetParam(fn, 1) };
        LLVMValueRef rc = gen_call(g, gen_libc(g, "pthread_create"), ca, 4, "rc");
        LLVMValueRef failed = LLVMBuildICmp(g->b, LLVMIntNE, rc, gen_i32(g, 0), "failed");
        LLVMBasicBlockRef fail_bb = gen_block(g, "create.fail");
        LLVMBasicBlockRef ok_bb = gen_block(g, "create.ok");
        LLVMBuildCondBr(g->b, failed, fail_bb, ok_bb);

        LLVMPositionBuilderAtEnd(g->b, fail_bb);
        call_pthread(g, "pthread_mutex_unlock", &g_lock, 1);
        LLVMValueRef fa[] = { line, gen_cstr(g, "cannot create actor thread") };
        gen_call(g, rt(g, "rabta.fail"), fa, 2, "");
        LLVMBuildUnreachable(g->b);

        /* grow the registry when it is full (16, 32, 64, ...) */
        LLVMPositionBuilderAtEnd(g->b, ok_bb);
        LLVMValueRef n = LLVMBuildLoad2(g->b, g->t_i64, g_n, "n");
        LLVMValueRef cap = LLVMBuildLoad2(g->b, g->t_i64, g_cap, "cap");
        LLVMValueRef full = LLVMBuildICmp(g->b, LLVMIntEQ, n, cap, "full");
        LLVMBasicBlockRef grow_bb = gen_block(g, "grow");
        LLVMBasicBlockRef put_bb = gen_block(g, "put");
        LLVMBuildCondBr(g->b, full, grow_bb, put_bb);

        LLVMPositionBuilderAtEnd(g->b, grow_bb);
        LLVMValueRef was0 = LLVMBuildICmp(g->b, LLVMIntEQ, cap, gen_i64(g, 0), "empty");
        LLVMValueRef newcap = LLVMBuildSelect(g->b, was0, gen_i64(g, 16),
                                              LLVMBuildMul(g->b, cap, gen_i64(g, 2), "cap2"), "newcap");
        LLVMValueRef old = LLVMBuildLoad2(g->b, g->t_ptr, g_threads, "old");
        LLVMValueRef ra[] = { old, LLVMBuildMul(g->b, newcap, gen_i64(g, 8), "bytes") };
        LLVMValueRef grown = gen_call(g, gen_libc(g, "realloc"), ra, 2, "grown");
        check_alloc(g, grown, line);
        LLVMBuildStore(g->b, grown, g_threads);
        LLVMBuildStore(g->b, newcap, g_cap);
        LLVMBuildBr(g->b, put_bb);

        LLVMPositionBuilderAtEnd(g->b, put_bb);
        LLVMValueRef arr = LLVMBuildLoad2(g->b, g->t_ptr, g_threads, "threads");
        LLVMValueRef slot = LLVMBuildGEP2(g->b, g->t_i64, arr, &n, 1, "slot");
        LLVMBuildStore(g->b, LLVMBuildLoad2(g->b, g->t_i64, tid, "id"), slot);
        LLVMBuildStore(g->b, LLVMBuildAdd(g->b, n, gen_i64(g, 1), "n.inc"), g_n);
        call_pthread(g, "pthread_mutex_unlock", &g_lock, 1);
        call_pthread(g, "pthread_attr_destroy", &attr, 1);
        LLVMBuildRetVoid(g->b);
    }

    /* void @rabta.join_all(): join until every started thread is joined */
    {
        gen_new_runtime_fn(g, "rabta.join_all", g->t_void, NULL, 0);
        LLVMBasicBlockRef loop = gen_block(g, "loop");
        LLVMBasicBlockRef done = gen_block(g, "done");
        LLVMBasicBlockRef join = gen_block(g, "join");
        LLVMBuildBr(g->b, loop);

        LLVMPositionBuilderAtEnd(g->b, loop);
        call_pthread(g, "pthread_mutex_lock", &g_lock, 1);
        LLVMValueRef j = LLVMBuildLoad2(g->b, g->t_i64, g_joined, "joined");
        LLVMValueRef n = LLVMBuildLoad2(g->b, g->t_i64, g_n, "n");
        LLVMBuildCondBr(g->b, LLVMBuildICmp(g->b, LLVMIntEQ, j, n, "all"), done, join);

        LLVMPositionBuilderAtEnd(g->b, done);
        call_pthread(g, "pthread_mutex_unlock", &g_lock, 1);
        LLVMBuildRetVoid(g->b);

        LLVMPositionBuilderAtEnd(g->b, join);
        LLVMValueRef arr = LLVMBuildLoad2(g->b, g->t_ptr, g_threads, "threads");
        LLVMValueRef t = LLVMBuildLoad2(g->b, g->t_i64,
                                        LLVMBuildGEP2(g->b, g->t_i64, arr, &j, 1, "slot"), "tid");
        LLVMBuildStore(g->b, LLVMBuildAdd(g->b, j, gen_i64(g, 1), "j.inc"), g_joined);
        call_pthread(g, "pthread_mutex_unlock", &g_lock, 1);
        LLVMValueRef ja[] = { t, null };
        call_pthread(g, "pthread_join", ja, 2);
        LLVMBuildBr(g->b, loop);
    }

    /* void @rabta.shutdown(): every actor has been joined; free channels */
    {
        gen_new_runtime_fn(g, "rabta.shutdown", g->t_void, NULL, 0);
        LLVMBasicBlockRef entry = LLVMGetInsertBlock(g->b);
        LLVMBasicBlockRef loop = gen_block(g, "loop");
        LLVMBasicBlockRef body = gen_block(g, "free.chan");
        LLVMBasicBlockRef done = gen_block(g, "done");
        LLVMValueRef first = LLVMBuildLoad2(g->b, g->t_ptr, g_chans, "first");
        LLVMBuildBr(g->b, loop);

        LLVMPositionBuilderAtEnd(g->b, loop);
        LLVMValueRef c = LLVMBuildPhi(g->b, g->t_ptr, "chan");
        LLVMBuildCondBr(g->b, LLVMBuildIsNull(g->b, c, "end"), done, body);

        LLVMPositionBuilderAtEnd(g->b, body);
        LLVMValueRef next = LLVMBuildLoad2(g->b, g->t_ptr,
                                           LLVMBuildStructGEP2(g->b, g->t_chan, c, 1, "next.ptr"), "next");
        for (int role = 0; role < 2; role++) {
            LLVMValueRef q = queue_ptr(g, c, gen_i32(g, role));
            LLVMValueRef m = qfield(g, q, Q_LOCK, "lock");
            call_pthread(g, "pthread_mutex_destroy", &m, 1);
            LLVMValueRef e = qfield(g, q, Q_NOT_EMPTY, "not_empty");
            call_pthread(g, "pthread_cond_destroy", &e, 1);
            LLVMValueRef f = qfield(g, q, Q_NOT_FULL, "not_full");
            call_pthread(g, "pthread_cond_destroy", &f, 1);
        }
        gen_call(g, gen_libc(g, "free"), &c, 1, "");
        LLVMBuildBr(g->b, loop);

        LLVMValueRef in_v[] = { first, next };
        LLVMBasicBlockRef in_b[] = { entry, body };
        LLVMAddIncoming(c, in_v, in_b, 2);

        LLVMPositionBuilderAtEnd(g->b, done);
        LLVMBuildStore(g->b, null, g_chans);
        LLVMValueRef threads = LLVMBuildLoad2(g->b, g->t_ptr, g_threads, "threads");
        gen_call(g, gen_libc(g, "free"), &threads, 1, "");
        LLVMBuildStore(g->b, null, g_threads);
        LLVMBuildRetVoid(g->b);
    }
}

void gen_actor_runtime(Gen *g)
{
    LLVMTypeRef msg_fields[] = { g->t_i64, g->t_ptr };
    g->t_msg = LLVMStructCreateNamed(g->ctx, "rabta.msg");
    LLVMStructSetBody(g->t_msg, msg_fields, 2, 0);

    LLVMTypeRef sync = LLVMArrayType(g->t_i64, 8);     /* 64 bytes, align 8 */
    LLVMTypeRef q_fields[] = {
        LLVMArrayType(g->t_msg, QUEUE_CAPACITY), g->t_i64, g->t_i64, sync, sync, sync
    };
    g->t_queue = LLVMStructCreateNamed(g->ctx, "rabta.queue");
    LLVMStructSetBody(g->t_queue, q_fields, 6, 0);

    LLVMTypeRef c_fields[] = { LLVMArrayType(g->t_queue, 2), g->t_ptr };
    g->t_chan = LLVMStructCreateNamed(g->ctx, "rabta.chan");
    LLVMStructSetBody(g->t_chan, c_fields, 2, 0);

    gen_thread_runtime(g);      /* first: chan_new uses its registry lock */
    gen_channel_runtime(g);
}

/* ------------------------------------------------------------------------ */
/*  banda                                                                   */
/* ------------------------------------------------------------------------ */

/* { i64 | ptr, ... }: one 8-byte field per parameter, in order */
static LLVMTypeRef actor_args_type(Gen *g, Node *actor)
{
    int n = actor->list->count;
    LLVMTypeRef *f = malloc(sizeof(LLVMTypeRef) * ((size_t)n + 1));
    for (int i = 0; i < n; i++)
        f[i] = actor->list->items[i]->pkind == PARAM_ENDPOINT ? g->t_ptr : g->t_i64;
    LLVMTypeRef t = LLVMStructTypeInContext(g->ctx, f, (unsigned)n, 0);
    free(f);
    return t;
}

static LLVMValueRef actor_fn(Gen *g, Node *actor)
{
    char sym[256];
    snprintf(sym, sizeof sym, "banda.%s", actor->name);
    return LLVMGetNamedFunction(g->mod, sym);
}

void gen_declare_actors(Gen *g)
{
    NodeList *items = g->program->list;
    for (int i = 0; i < items->count; i++) {
        Node *a = items->items[i];
        if (a->kind != N_ACTOR)
            continue;
        char sym[256];
        snprintf(sym, sizeof sym, "banda.%s", a->name);
        LLVMTypeRef ps[] = { g->t_ptr };
        LLVMValueRef f = LLVMAddFunction(g->mod, sym, LLVMFunctionType(g->t_ptr, ps, 1, 0));
        LLVMSetLinkage(f, LLVMInternalLinkage);
        LLVMSetValueName2(LLVMGetParam(f, 0), "args", 4);
    }
}

void gen_define_actors(Gen *g)
{
    NodeList *items = g->program->list;
    for (int i = 0; i < items->count; i++) {
        Node *actor = items->items[i];
        if (actor->kind != N_ACTOR)
            continue;

        LLVMValueRef f = actor_fn(g, actor);
        gen_begin_body(g, actor, f);

        /* unpack the arguments packed by paida, then free the pack */
        LLVMValueRef args = LLVMGetParam(f, 0);
        LLVMTypeRef at = actor_args_type(g, actor);
        for (int k = 0; k < actor->list->count; k++) {
            Node *p = actor->list->items[k];
            LLVMValueRef fp = LLVMBuildStructGEP2(g->b, at, args, (unsigned)k, "arg.ptr");
            if (p->pkind == PARAM_ENDPOINT)
                LLVMBuildStore(g->b, LLVMBuildLoad2(g->b, g->t_ptr, fp, p->name), slot_chan(g, p->slot));
            else
                LLVMBuildStore(g->b, LLVMBuildLoad2(g->b, g->t_i64, fp, p->name), slot_i(g, p->slot));
        }
        gen_call(g, gen_libc(g, "free"), &args, 1, "");

        gen_stmts(g, actor->a->list);

        /* an actor ends at its khatam (waapis is not allowed in a banda) */
        if (!gen_is_terminated(g)) {
            gen_free_owned(g);
            LLVMBuildRet(g->b, LLVMConstPointerNull(g->t_ptr));
        }
        gen_end_body(g);
    }
}

/* paida name(args) */
void gen_spawn(Gen *g, Node *s)
{
    Node *actor = s->ref;
    int n = actor->list->count;
    LLVMTypeRef at = actor_args_type(g, actor);
    LLVMValueRef *vals = malloc(sizeof(LLVMValueRef) * ((size_t)n + 1));

    /* evaluate the arguments left to right; endpoints pass the channel */
    for (int k = 0; k < n; k++) {
        Node *p = actor->list->items[k];
        Node *a = s->list->items[k];
        vals[k] = p->pkind == PARAM_ENDPOINT
                ? LLVMBuildLoad2(g->b, g->t_ptr, slot_chan(g, a->slot), "endpoint")
                : gen_expr(g, a);
    }

    LLVMValueRef ca[] = { gen_i64(g, 1), LLVMSizeOf(at) };
    LLVMValueRef pack = gen_call(g, gen_libc(g, "calloc"), ca, 2, "args");
    check_alloc(g, pack, gen_i32(g, s->line));
    for (int k = 0; k < n; k++)
        LLVMBuildStore(g->b, vals[k], LLVMBuildStructGEP2(g->b, at, pack, (unsigned)k, "arg.ptr"));
    free(vals);

    LLVMValueRef sa[] = { actor_fn(g, actor), pack, gen_i32(g, s->line) };
    gen_call(g, rt(g, "rabta.spawn"), sa, 3, "");
}

/* ------------------------------------------------------------------------ */
/*  nali, bolo, suno                                                        */
/* ------------------------------------------------------------------------ */

void gen_chandecl(Gen *g, Node *s)
{
    LLVMValueRef line = gen_i32(g, s->line);
    LLVMValueRef c = gen_call(g, rt(g, "rabta.chan_new"), &line, 1, s->name);
    LLVMBuildStore(g->b, c, slot_chan(g, s->slot));
}

/* channel pointer of an endpoint expression (ch.client, ch.server or sira) */
static LLVMValueRef endpoint_chan(Gen *g, Node *ep)
{
    return LLVMBuildLoad2(g->b, g->t_ptr, slot_chan(g, ep->slot), "chan");
}

/* bolo ep, value   (a thali is copied, the copy travels with the message) */
void gen_send(Gen *g, Node *s)
{
    LLVMValueRef c = endpoint_chan(g, s->a);
    LLVMValueRef to = gen_i32(g, 1 - s->a->role);        /* the other side */
    LLVMValueRef value, data;

    if (s->b->ty.kind == TY_ARRAY) {
        LLVMValueRef src = LLVMBuildLoad2(g->b, g->t_ptr, slot_arr(g, s->b->slot), "data");
        value = LLVMBuildLoad2(g->b, g->t_i64, slot_len(g, s->b->slot), "len");
        LLVMValueRef bytes = LLVMBuildMul(g->b, value, gen_i64(g, 8), "bytes");
        data = gen_call(g, gen_libc(g, "malloc"), &bytes, 1, "copy");
        check_alloc(g, data, gen_i32(g, s->line));
        LLVMValueRef ma[] = { data, src, bytes };
        gen_call(g, gen_libc(g, "memcpy"), ma, 3, "");
    } else {
        value = gen_expr(g, s->b);
        data = LLVMConstPointerNull(g->t_ptr);
    }
    LLVMValueRef args[] = { c, to, value, data };
    gen_call(g, rt(g, "rabta.send"), args, 4, "");
}

/* call @rabta.recv; *out_data receives the thali copy (or null) */
static LLVMValueRef do_recv(Gen *g, Node *recv, LLVMValueRef *out_data)
{
    LLVMValueRef outp = gen_entry_alloca(g, g->t_ptr, "msg.data");
    LLVMValueRef args[] = { endpoint_chan(g, recv->a), gen_i32(g, recv->a->role), outp };
    LLVMValueRef v = gen_call(g, rt(g, "rabta.recv"), args, 3, "msg");
    *out_data = LLVMBuildLoad2(g->b, g->t_ptr, outp, "msg.data");
    return v;
}

/* rakho x = suno ep  /  x = suno ep  /  a[i] = suno ep */
LLVMValueRef gen_recv_i64(Gen *g, Node *recv)
{
    LLVMValueRef data;
    return do_recv(g, recv, &data);
}

/* thali a[N] = suno ep  /  a = suno ep: copy the elements, free the message */
void gen_recv_into(Gen *g, Node *recv, LLVMValueRef dst, LLVMValueRef len)
{
    LLVMValueRef data;
    (void)do_recv(g, recv, &data);
    LLVMValueRef ma[] = { dst, data, LLVMBuildMul(g->b, len, gen_i64(g, 8), "bytes") };
    gen_call(g, gen_libc(g, "memcpy"), ma, 3, "");
    gen_call(g, gen_libc(g, "free"), &data, 1, "");
}

/* suno ep as a statement: the message is dropped */
void gen_recv_discard(Gen *g, Node *recv)
{
    LLVMValueRef data;
    (void)do_recv(g, recv, &data);
    gen_call(g, gen_libc(g, "free"), &data, 1, "");
}
