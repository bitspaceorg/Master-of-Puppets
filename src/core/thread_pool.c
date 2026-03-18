/*
 * Master of Puppets — Generic Thread Pool
 * thread_pool.c — Fork-join thread pool implementation
 *
 * Uses a bounded circular task queue protected by a mutex + condition
 * variables.  Workers spin on the queue until shutdown is signaled.
 * mop_threadpool_wait() uses a generation counter to detect when all
 * tasks submitted before the call have completed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if !defined(_POSIX_C_SOURCE) && !defined(MOP_PLATFORM_MACOS)
#define _POSIX_C_SOURCE 200112L
#endif

#include "thread_pool.h"

#include <pthread.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Task queue — bounded circular buffer
 * ------------------------------------------------------------------------- */

#define MOP_TASK_QUEUE_CAPACITY 4096

typedef struct MopTask {
  MopTaskFn fn;
  void *arg;
} MopTask;

struct MopThreadPool {
  pthread_t *threads;
  int num_threads;

  /* Task queue (circular buffer) */
  MopTask queue[MOP_TASK_QUEUE_CAPACITY];
  uint32_t head; /* next slot to dequeue */
  uint32_t tail; /* next slot to enqueue */
  uint32_t count;

  /* Synchronization */
  pthread_mutex_t mutex;
  pthread_cond_t not_empty; /* signaled when a task is added */
  pthread_cond_t not_full;  /* signaled when a task is consumed */
  pthread_cond_t idle;      /* signaled when active_workers reaches 0 */

  /* State */
  bool shutdown;
  int active_workers; /* workers currently executing a task */

  /* Generation counter for wait():
   * submitted = total tasks ever submitted
   * completed = total tasks ever completed
   * wait() captures submitted, then blocks until completed >= that value. */
  uint64_t submitted;
  uint64_t completed;
};

/* -------------------------------------------------------------------------
 * Worker thread function
 * ------------------------------------------------------------------------- */

static void *worker_func(void *arg) {
  MopThreadPool *pool = (MopThreadPool *)arg;

  for (;;) {
    pthread_mutex_lock(&pool->mutex);

    /* Wait for a task or shutdown */
    while (pool->count == 0 && !pool->shutdown) {
      pthread_cond_wait(&pool->not_empty, &pool->mutex);
    }

    if (pool->shutdown && pool->count == 0) {
      pthread_mutex_unlock(&pool->mutex);
      break;
    }

    /* Dequeue task */
    MopTask task = pool->queue[pool->head];
    pool->head = (pool->head + 1) % MOP_TASK_QUEUE_CAPACITY;
    pool->count--;
    pool->active_workers++;

    /* Signal producers blocked on full queue */
    pthread_cond_signal(&pool->not_full);
    pthread_mutex_unlock(&pool->mutex);

    /* Execute task */
    task.fn(task.arg);

    /* Mark completion */
    pthread_mutex_lock(&pool->mutex);
    pool->active_workers--;
    pool->completed++;

    /* Signal wait() if all tasks are done */
    if (pool->active_workers == 0 && pool->count == 0) {
      pthread_cond_broadcast(&pool->idle);
    }
    pthread_mutex_unlock(&pool->mutex);
  }

  return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

MopThreadPool *mop_threadpool_create(int num_threads) {
  if (num_threads < 1)
    num_threads = 1;

  MopThreadPool *pool = calloc(1, sizeof(MopThreadPool));
  if (!pool)
    return NULL;

  pool->num_threads = num_threads;
  pool->shutdown = false;
  pool->head = 0;
  pool->tail = 0;
  pool->count = 0;
  pool->active_workers = 0;
  pool->submitted = 0;
  pool->completed = 0;

  if (pthread_mutex_init(&pool->mutex, NULL) != 0)
    goto fail_pool;
  if (pthread_cond_init(&pool->not_empty, NULL) != 0)
    goto fail_mutex;
  if (pthread_cond_init(&pool->not_full, NULL) != 0)
    goto fail_not_empty;
  if (pthread_cond_init(&pool->idle, NULL) != 0)
    goto fail_not_full;

  pool->threads = malloc((size_t)num_threads * sizeof(pthread_t));
  if (!pool->threads)
    goto fail_idle;

  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&pool->threads[i], NULL, worker_func, pool) != 0) {
      /* Join already-created threads */
      pthread_mutex_lock(&pool->mutex);
      pool->shutdown = true;
      pthread_cond_broadcast(&pool->not_empty);
      pthread_mutex_unlock(&pool->mutex);
      for (int j = 0; j < i; j++)
        pthread_join(pool->threads[j], NULL);
      free(pool->threads);
      goto fail_idle;
    }
  }

  return pool;

fail_idle:
  pthread_cond_destroy(&pool->idle);
fail_not_full:
  pthread_cond_destroy(&pool->not_full);
fail_not_empty:
  pthread_cond_destroy(&pool->not_empty);
fail_mutex:
  pthread_mutex_destroy(&pool->mutex);
fail_pool:
  free(pool);
  return NULL;
}

void mop_threadpool_destroy(MopThreadPool *pool) {
  if (!pool)
    return;

  /* Wait for pending work, then signal shutdown */
  pthread_mutex_lock(&pool->mutex);
  while (pool->count > 0 || pool->active_workers > 0)
    pthread_cond_wait(&pool->idle, &pool->mutex);
  pool->shutdown = true;
  pthread_cond_broadcast(&pool->not_empty);
  pthread_mutex_unlock(&pool->mutex);

  /* Join all workers */
  for (int i = 0; i < pool->num_threads; i++)
    pthread_join(pool->threads[i], NULL);

  free(pool->threads);
  pthread_cond_destroy(&pool->idle);
  pthread_cond_destroy(&pool->not_full);
  pthread_cond_destroy(&pool->not_empty);
  pthread_mutex_destroy(&pool->mutex);
  free(pool);
}

bool mop_threadpool_submit(MopThreadPool *pool, MopTaskFn fn, void *arg) {
  if (!pool || !fn)
    return false;

  pthread_mutex_lock(&pool->mutex);

  if (pool->shutdown) {
    pthread_mutex_unlock(&pool->mutex);
    return false;
  }

  /* Wait for space in the queue */
  while (pool->count == MOP_TASK_QUEUE_CAPACITY && !pool->shutdown) {
    pthread_cond_wait(&pool->not_full, &pool->mutex);
  }

  if (pool->shutdown) {
    pthread_mutex_unlock(&pool->mutex);
    return false;
  }

  /* Enqueue */
  pool->queue[pool->tail] = (MopTask){.fn = fn, .arg = arg};
  pool->tail = (pool->tail + 1) % MOP_TASK_QUEUE_CAPACITY;
  pool->count++;
  pool->submitted++;

  pthread_cond_signal(&pool->not_empty);
  pthread_mutex_unlock(&pool->mutex);
  return true;
}

void mop_threadpool_wait(MopThreadPool *pool) {
  if (!pool)
    return;

  pthread_mutex_lock(&pool->mutex);

  /* Capture target: all tasks submitted so far must complete */
  uint64_t target = pool->submitted;

  while (pool->completed < target ||
         (pool->count > 0 || pool->active_workers > 0)) {
    pthread_cond_wait(&pool->idle, &pool->mutex);
  }

  pthread_mutex_unlock(&pool->mutex);
}

int mop_threadpool_num_threads(const MopThreadPool *pool) {
  return pool ? pool->num_threads : 0;
}
