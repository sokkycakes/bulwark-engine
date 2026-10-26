/* Bulwark engine - a pool of worker threads for slow jobs (hashing,
 * decoding, queries). ISC licence, see LICENSE. */
#ifndef BW_WORKER_H
#define BW_WORKER_H

#include "common.h"

/* work runs on a worker thread; done, if any, runs later on the main
 * thread from Worker_Frame */
typedef void (*worker_fn)(void *ctx);
void Worker_Add(worker_fn work, worker_fn done, void *ctx);
void Worker_Frame(void);
void Worker_Init(void);

#endif
