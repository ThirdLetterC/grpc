#pragma once

/* The small POSIX threading subset used by the library and its examples. */
#ifndef _WIN32
#include <pthread.h>
#else
#include <uv.h>
#include <process.h>
#include <stdlib.h>

typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef INIT_ONCE pthread_once_t;
typedef HANDLE pthread_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER  CONDITION_VARIABLE_INIT
#define PTHREAD_ONCE_INIT         INIT_ONCE_STATIC_INIT

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
  (void)attr;
  InitializeSRWLock(mutex);
  return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
  (void)mutex;
  return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
  AcquireSRWLockExclusive(mutex);
  return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
  ReleaseSRWLockExclusive(mutex);
  return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
  return SleepConditionVariableSRW(cond, mutex, INFINITE, 0) ? 0 : -1;
}
static inline int pthread_cond_signal(pthread_cond_t *cond) {
  WakeConditionVariable(cond);
  return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *cond) {
  WakeAllConditionVariable(cond);
  return 0;
}
static BOOL CALLBACK grpc_once_callback(INIT_ONCE *once, void *arg, void **context) {
  (void)once;
  (void)context;
  void (**fn)(void) = arg;
  (*fn)();
  return TRUE;
}
static inline int pthread_once(pthread_once_t *once, void (*fn)(void)) {
  return InitOnceExecuteOnce(once, grpc_once_callback, &fn, NULL) ? 0 : -1;
}

typedef struct grpc_thread_start {
  void *(*fn)(void *);
  void *arg;
} grpc_thread_start;

static unsigned __stdcall grpc_thread_entry(void *arg) {
  grpc_thread_start start = *(grpc_thread_start *)arg;
  free(arg);
  start.fn(start.arg);
  return 0;
}
static inline int pthread_create(pthread_t *thread, const void *attr, void *(*fn)(void *),
                                 void *arg) {
  (void)attr;
  grpc_thread_start *start = malloc(sizeof(*start));
  if (start == NULL)
    return -1;
  *start = (grpc_thread_start){fn, arg};
  *thread = (HANDLE)_beginthreadex(NULL, 0, grpc_thread_entry, start, 0, NULL);
  if (*thread == NULL) {
    free(start);
    return -1;
  }
  return 0;
}
static inline int pthread_join(pthread_t thread, void **result) {
  /* All callers discard the thread's return value. */
  if (result != NULL)
    return -1;
  if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0)
    return -1;
  return CloseHandle(thread) ? 0 : -1;
}
static inline int pthread_detach(pthread_t thread) {
  return CloseHandle(thread) ? 0 : -1;
}
#endif
