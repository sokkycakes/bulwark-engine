/* Bulwark engine - the pthread subset the engine uses, on the Xbox
 * kernel's threads (posix_xbox.c). ISC licence, see LICENSE. */
#ifndef BW_XBOX_PTHREAD_H
#define BW_XBOX_PTHREAD_H

#include <stdint.h>

typedef void *pthread_t;
typedef struct {
	int dummy;
} pthread_attr_t;

/* a lazily created critical section, so PTHREAD_MUTEX_INITIALIZER works */
typedef struct {
	volatile long init; /* 0 none, 1 initialising, 2 ready */
	void *cs;
} pthread_mutex_t;
typedef struct {
	int dummy;
} pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER {0, 0}

/* a condition variable: a waiter count and a kernel semaphore */
typedef struct {
	volatile long init;
	void *sem;
	volatile long waiters;
} pthread_cond_t;
typedef struct {
	int dummy;
} pthread_condattr_t;
#define PTHREAD_COND_INITIALIZER {0, 0, 0}

typedef struct {
	volatile long done;
} pthread_once_t;
#define PTHREAD_ONCE_INIT {0}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_detach(pthread_t t);
int pthread_join(pthread_t t, void **ret);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_once(pthread_once_t *o, void (*fn)(void));

#endif
