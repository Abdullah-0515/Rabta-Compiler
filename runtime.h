/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  runtime.h - Actor and channel runtime (pthreads).
 *
 *  - Every banda started with paida runs on its own POSIX thread.
 *  - A channel (nali) holds two bounded FIFO queues, one per direction:
 *        queue[ROLE_SERVER] : messages from client to server
 *        queue[ROLE_CLIENT] : messages from server to client
 *    Each queue is protected by a mutex; senders wait on `not_full`,
 *    receivers wait on `not_empty` (condition variables).
 *  - suno blocks until a message arrives; bolo blocks only if the queue is
 *    full.
 *  - Messages are copied (arrays are deep-copied), so actors never share
 *    memory.
 *
 *  This is the same small C runtime that Phase 2 (LLVM) code will call.
 */
#ifndef RABTA_RUNTIME_H
#define RABTA_RUNTIME_H

#include <stdint.h>

#define RT_QUEUE_CAPACITY 16
#define RT_THREAD_STACK   (16u * 1024u * 1024u)   /* 16 MiB per actor */

typedef struct RMsg {
    int      is_array;
    int64_t  value;       /* i64 message                                */
    int64_t  len;         /* thali message: number of elements          */
    int64_t *data;        /* thali message: heap copy owned by the msg  */
} RMsg;

typedef struct RChannel RChannel;

typedef struct REndpoint {
    RChannel *chan;
    int       role;       /* ROLE_CLIENT / ROLE_SERVER (see ast.h) */
} REndpoint;

void      rt_init(void);
RChannel *rt_channel_new(const char *protocol_name);

/* Send from endpoint `ep` to the other side. Takes ownership of msg.data. */
void      rt_send(REndpoint ep, RMsg msg);
/* Receive on endpoint `ep`; blocks until a message arrives. */
RMsg      rt_recv(REndpoint ep);

/* Start a new actor thread running fn(arg). */
void      rt_spawn(void *(*fn)(void *), void *arg);
/* Wait until every actor thread (including ones spawned by actors) ends. */
void      rt_join_all(void);
/* Free all channels. Call after rt_join_all(). */
void      rt_shutdown(void);

/* Serialises output lines from different actors. */
void      rt_output_lock(void);
void      rt_output_unlock(void);

#endif /* RABTA_RUNTIME_H */
