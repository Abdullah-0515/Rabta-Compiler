/*
 * ============================================================================
 *  Rabta Programming Language
 *  Authors: Ansa Mubasher, Ahmed Yasin, Rana Muhammad Abdullah
 *  Course : Compiler Construction - Phase 1 (Flex + Bison tree-walking interpreter)
 * ============================================================================
 *
 *  runtime.c - Actor threads and bounded channel queues (see runtime.h).
 */
#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RQueue {
    RMsg            buf[RT_QUEUE_CAPACITY];
    int             head;
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} RQueue;

struct RChannel {
    RQueue      queue[2];      /* indexed by the RECEIVING role */
    const char *protocol;
    RChannel   *next;          /* registry of all channels, for cleanup */
};

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static RChannel       *channels = NULL;

static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t      *threads = NULL;
static int             nthreads = 0, capthreads = 0, njoined = 0;

static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

static void rt_die(const char *what)
{
    fprintf(stderr, "rabta runtime: %s\n", what);
    exit(2);
}

void rt_init(void)
{
    /* All state is statically initialised; kept for symmetry with Phase 2. */
}

/* ------------------------------------------------------------------------ */
/*  Channels                                                                */
/* ------------------------------------------------------------------------ */

static void queue_init(RQueue *q)
{
    q->head = 0;
    q->count = 0;
    if (pthread_mutex_init(&q->lock, NULL) != 0 ||
        pthread_cond_init(&q->not_empty, NULL) != 0 ||
        pthread_cond_init(&q->not_full, NULL) != 0)
        rt_die("cannot initialise channel queue");
}

static void queue_destroy(RQueue *q)
{
    for (int i = 0; i < q->count; i++)
        free(q->buf[(q->head + i) % RT_QUEUE_CAPACITY].data);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

RChannel *rt_channel_new(const char *protocol_name)
{
    RChannel *c = calloc(1, sizeof *c);
    if (!c)
        rt_die("out of memory");
    queue_init(&c->queue[0]);
    queue_init(&c->queue[1]);
    c->protocol = protocol_name;

    pthread_mutex_lock(&registry_lock);
    c->next = channels;
    channels = c;
    pthread_mutex_unlock(&registry_lock);
    return c;
}

void rt_send(REndpoint ep, RMsg msg)
{
    RQueue *q = &ep.chan->queue[1 - ep.role];   /* deliver to the other role */

    pthread_mutex_lock(&q->lock);
    while (q->count == RT_QUEUE_CAPACITY)
        pthread_cond_wait(&q->not_full, &q->lock);
    q->buf[(q->head + q->count) % RT_QUEUE_CAPACITY] = msg;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

RMsg rt_recv(REndpoint ep)
{
    RQueue *q = &ep.chan->queue[ep.role];       /* messages addressed to me */
    RMsg msg;

    pthread_mutex_lock(&q->lock);
    while (q->count == 0)
        pthread_cond_wait(&q->not_empty, &q->lock);
    msg = q->buf[q->head];
    q->head = (q->head + 1) % RT_QUEUE_CAPACITY;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return msg;
}

/* ------------------------------------------------------------------------ */
/*  Actor threads                                                           */
/* ------------------------------------------------------------------------ */

void rt_spawn(void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    pthread_t tid;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, RT_THREAD_STACK);

    /* Hold the lock while creating so rt_join_all never misses a thread. */
    pthread_mutex_lock(&threads_lock);
    if (pthread_create(&tid, &attr, fn, arg) != 0) {
        pthread_mutex_unlock(&threads_lock);
        rt_die("cannot create actor thread");
    }
    if (nthreads == capthreads) {
        capthreads = capthreads ? capthreads * 2 : 16;
        threads = realloc(threads, (size_t)capthreads * sizeof(pthread_t));
        if (!threads)
            rt_die("out of memory");
    }
    threads[nthreads++] = tid;
    pthread_mutex_unlock(&threads_lock);
    pthread_attr_destroy(&attr);
}

void rt_join_all(void)
{
    for (;;) {
        pthread_t tid;
        pthread_mutex_lock(&threads_lock);
        if (njoined == nthreads) {
            pthread_mutex_unlock(&threads_lock);
            break;
        }
        tid = threads[njoined++];
        pthread_mutex_unlock(&threads_lock);
        pthread_join(tid, NULL);
    }
}

void rt_shutdown(void)
{
    pthread_mutex_lock(&registry_lock);
    while (channels) {
        RChannel *c = channels;
        channels = c->next;
        queue_destroy(&c->queue[0]);
        queue_destroy(&c->queue[1]);
        free(c);
    }
    pthread_mutex_unlock(&registry_lock);

    pthread_mutex_lock(&threads_lock);
    free(threads);
    threads = NULL;
    nthreads = capthreads = njoined = 0;
    pthread_mutex_unlock(&threads_lock);
}

/* ------------------------------------------------------------------------ */
/*  Output                                                                  */
/* ------------------------------------------------------------------------ */

void rt_output_lock(void)   { pthread_mutex_lock(&output_lock); }
void rt_output_unlock(void) { pthread_mutex_unlock(&output_lock); }
