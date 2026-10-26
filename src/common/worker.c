/* Bulwark engine - the worker threads. A job goes on a queue that the
 * workers take from in order; when one finishes, its completion waits on a
 * second queue until the main thread's next frame. worker_status and
 * worker_test look at the pool. ISC licence, see LICENSE. */
#include "worker.h"
#include "cmd.h"
#include "cvar.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct job_s {
	worker_fn work, done;
	void *ctx;
	struct job_s *next;
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static job_t *pending, **pending_tail = &pending, *finished, **finished_tail = &finished;
static int numpending, numthreads, busy;
static bool started;

static void *worker_main(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!pending)
			pthread_cond_wait(&wake, &lock);
		job_t *j = pending;
		pending = j->next;
		if (!pending)
			pending_tail = &pending;
		numpending--;
		busy++;
		pthread_mutex_unlock(&lock);

		j->work(j->ctx);

		pthread_mutex_lock(&lock);
		busy--;
		j->next = NULL;
		*finished_tail = j;
		finished_tail = &j->next;
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static void start(void)
{
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	int want = (int)BW_MAX(1, BW_MIN(cores - 1, 8));
	started = true;
	for (int i = 0; i < want; i++) {
		pthread_t t;
		if (pthread_create(&t, NULL, worker_main, NULL) == 0) {
			pthread_detach(t);
			numthreads++;
		}
	}
}

void Worker_Add(worker_fn work, worker_fn done, void *ctx)
{
	if (!started)
		start();
	if (!numthreads) { /* no threads: do it now */
		work(ctx);
		if (done)
			done(ctx);
		return;
	}
	job_t *j = Mem_Alloc(sizeof(*j));
	j->work = work;
	j->done = done;
	j->ctx = ctx;
	pthread_mutex_lock(&lock);
	*pending_tail = j;
	pending_tail = &j->next;
	numpending++;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void Worker_Frame(void)
{
	job_t *list;
	if (!finished)
		return; /* a stale read only delays a completion by a frame */
	pthread_mutex_lock(&lock);
	list = finished;
	finished = NULL;
	finished_tail = &finished;
	pthread_mutex_unlock(&lock);
	while (list) {
		job_t *next = list->next;
		if (list->done)
			list->done(list->ctx);
		Mem_Free(list);
		list = next;
	}
}

static void WorkerStatus_f(int argc, const char **argv)
{
	if (!started)
		start();
	pthread_mutex_lock(&lock);
	Con_Printf("%d workers live\n", numthreads);
	Con_Printf("%d pending tasks\n", numpending + busy);
	pthread_mutex_unlock(&lock);
}

static void ping_work(void *ctx) { (void)ctx; }

static void ping_done(void *ctx)
{
	Con_Printf("Ping: %g\n", Sys_Seconds() - *(double *)ctx);
	Mem_Free(ctx);
}

/* a job that does nothing: the time to go round a worker and back */
static void WorkerTest_f(int argc, const char **argv)
{
	double *t = Mem_Alloc(sizeof(*t));
	*t = Sys_Seconds();
	Worker_Add(ping_work, ping_done, t);
}

void Worker_Init(void)
{
	Cmd_AddD("worker_status", WorkerStatus_f, "How many worker threads there are and how much work waits");
	Cmd_AddD("worker_test", WorkerTest_f, "Send an empty job round a worker thread and print how long it took");
}
