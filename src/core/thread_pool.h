/*
 * Master of Puppets — Generic Thread Pool
 * thread_pool.h — Fork-join thread pool with task queue
 *
 * Persistent worker threads that process submitted tasks.  Supports
 * both fire-and-forget and fork-join (submit + wait) patterns.
 * Thread-safe: multiple threads may submit tasks concurrently.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MOP_THREAD_POOL_H
#define MOP_THREAD_POOL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct MopThreadPool MopThreadPool;
typedef void (*MopTaskFn)(void *arg);

/* Create a thread pool with the given number of worker threads.
 * Returns NULL on failure.  num_threads must be >= 1. */
MopThreadPool *mop_threadpool_create(int num_threads);

/* Destroy the pool, joining all workers.  Waits for pending tasks. */
void mop_threadpool_destroy(MopThreadPool *pool);

/* Submit a task to the pool.  fn(arg) will be called on a worker thread.
 * Returns true on success, false if the pool is shutting down or OOM. */
bool mop_threadpool_submit(MopThreadPool *pool, MopTaskFn fn, void *arg);

/* Block until all previously submitted tasks have completed.
 * After this returns, it is safe to read results written by tasks. */
void mop_threadpool_wait(MopThreadPool *pool);

/* Return the number of worker threads in the pool. */
int mop_threadpool_num_threads(const MopThreadPool *pool);

#endif /* MOP_THREAD_POOL_H */
