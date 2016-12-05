/* Simple non-adaptive, non backoff RTM lock. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include "include/spinlock.h"
#include "include/rtm.h"

static double diff_in_second(struct timespec t1,
			struct timespec t2) __attribute__((always_inline));
static inline double diff_in_second(struct timespec t1, struct timespec t2)
{
        struct timespec diff;
        if (t2.tv_nsec - t1.tv_nsec < 0) {
                diff.tv_sec  = t2.tv_sec - t1.tv_sec - 1;
                diff.tv_nsec = t2.tv_nsec - t1.tv_nsec + 1000000000;
        } else {
                diff.tv_sec  = t2.tv_sec - t1.tv_sec;
                diff.tv_nsec = t2.tv_nsec - t1.tv_nsec;
        }
        return (diff.tv_sec*1000000000.0 + diff.tv_nsec);
}

static comm_data test_data;

#if defined SPIN_LOCK || defined MUTEX_LOCK
static inline void lock_init() {
	int err;
#ifdef SPIN_LOCK
	if ((err = pthread_spin_init(&spinlock, PTHREAD_PROCESS_SHARED)) != 0)
		EXIT(errno, "spin init error");
#endif /*SPIN_LOCK*/

#ifdef MUTEX_LOCK
        if ((err = pthread_mutex_init(&mutexlock, NULL)) != 0)
                EXIT(errno, "io lock init error");
#endif /*MUTEX_LOCK*/
}
#endif /*SPIN_LOCK || MUTEX_LOCK*/

#if defined RTM_LOCK || defined HLE_LOCK
#define pause() asm volatile("pause" ::: "memory")

#define RETRY_CON 3
#define RETRY_CAP 1
#define RETRY_OTHER 3

static int lockvar = 0;
void __attribute__((noinline, weak)) trace_abort(unsigned status) {}

static inline int lock_init(int *lock)
{
	*lock = 1;
}

static inline int lock_is_free(int *lock)
{
	return *lock == 1;
}
#endif /*RTM_LOCK || HLE_LOCK*/

#ifdef HLE_LOCK
void lock(int *lock)
{
        while ((int) __hle_acquire_sub_fetch4((unsigned *)lock, 1) < 0) {
                do
                        pause();
                while (*lock != 1);
        }
}

void unlock(int *lock)
{
        __hle_release_store_n4((unsigned *)lock, 1);
}
#endif /*HLE_LOCK*/

#ifdef RTM_LOCK
void lock(int *lock)
{
        int i;
        unsigned status;
        unsigned retry = RETRY_OTHER;
  
        for (i = 0; i < retry; i++) {
                if ((status = _xbegin()) == _XBEGIN_STARTED) {
                        if (lock_is_free(lock)) {
                                printf("use rtm successfully\n");
                                return;
                        }
                        _xabort(0xff);
                }
                trace_abort(status);
                if ((status & _XABORT_EXPLICIT) && _XABORT_CODE(status) == 0xff) {
                        while (!lock_is_free(lock))
                                pause();
                } else if (!(status & _XABORT_RETRY) && !(status & _XABORT_CAPACITY))
                        break;
                if (status & _XABORT_CONFLICT) {
                        retry = RETRY_CON;
                        while (!lock_is_free(lock))
                                pause();
                        /* Could do various kinds of backoff here. */
                } else if (status & _XABORT_CAPACITY) {
                        retry = RETRY_CAP;
                } else {
                        retry = RETRY_OTHER;
                }
        }
        /* Could do adaptation here */
        while (__sync_sub_and_fetch(lock, 1) < 0) {
                do
                        pause();
                while (!lock_is_free(lock));
                /* Could do respeculation here */
        }
}

void unlock(int *lock)
{
        if (lock_is_free(lock))
                _xend();
        else
                *lock = 1;
}
#endif /*RTM_LOCK*/

/* get max priority of specified policy*/
int max_priority(int policy) {
        return sched_get_priority_max(policy);
}

/* bind cpu to eliminate the scheduler time and cache time*/
int cpu_bind(int cpu_id) {
	if (cpu_id < 0)
		return -1;
	int err;
	cpu_set_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu_id, &mask);
	if ((err = sched_setaffinity(0, sizeof mask, &mask)) != 0)
		EXIT(0, "set affinity failed");

	return 0;
}

static double thread_rw_time[NUM_READER + NUM_WRITER];
static int j;

void *read(void *arg) {
        struct timespec start;
        struct timespec end;
        double read_time;
        int tmp;

        cpu_bind(0);
        pthread_barrier_wait(&barrier);
        clock_gettime(CLOCK_MONOTONIC, &start);

#ifdef SPIN_LOCK
	pthread_spin_lock(&spinlock);
#endif

#ifdef MUTEX_LOCK
        pthread_mutex_lock(&mutexlock);
#endif

#if defined RTM_LOCK || defined HLE_LOCK
        lock(&lockvar);
#endif

	tmp = test_data.test_1;
        tmp = test_data.test_2;
        tmp = test_data.test_0;

#ifdef SPIN_LOCK
	pthread_spin_unlock(&spinlock);
#endif

#ifdef MUTEX_LOCK
        pthread_mutex_unlock(&mutexlock);
#endif

#if defined RTM_LOCK || defined HLE_LOCK
        unlock(&lockvar);
#endif

        clock_gettime(CLOCK_MONOTONIC, &end);
	read_time = diff_in_second(start, end);
        thread_rw_time[__sync_fetch_and_add(&j, 1)] = read_time;
}

void *write(void *arg) {
        struct timespec start;
        struct timespec end;
        double write_time;

        cpu_bind(1);
        pthread_barrier_wait(&barrier);
        clock_gettime(CLOCK_MONOTONIC, &start);

#ifdef SPIN_LOCK
	pthread_spin_lock(&spinlock);
#endif

#ifdef MUTEX_LOCK
        pthread_mutex_lock(&mutexlock);
#endif

#if defined RTM_LOCK || defined HLE_LOC
        lock(&lockvar);
#endif

	test_data.test_0++;
	test_data.test_2++;
        test_data.test_1++;

#ifdef SPIN_LOCK
	pthread_spin_unlock(&spinlock);
#endif

#ifdef MUTEX_LOCK
        pthread_mutex_unlock(&mutexlock);
#endif

#if defined RTM_LOCK || defined HLE_LOCK
        unlock(&lockvar);
#endif

        clock_gettime(CLOCK_MONOTONIC, &end);
        write_time = diff_in_second(start, end);
        thread_rw_time[__sync_fetch_and_add(&j, 1)] = write_time;
}

int main(void) {
	int err;
	int i;
	pthread_attr_t attr;
	struct sched_param param;
        FILE *fp;
        char sz[128];

        j = 0;
        memset(thread_rw_time, 0, sizeof(double)*(NUM_READER + NUM_WRITER));
	test_data.test_0 = 1;
	test_data.test_1 = 1;
	test_data.test_2 = 1;
	param.sched_priority = max_priority(SCHED_RR);
        if ((err=pthread_attr_init(&attr)) != 0)
                EXIT(errno, "init attr failed");
	/* set thread policy */
        if ((err=pthread_attr_setschedpolicy(&attr, SCHED_RR)) != 0)
                EXIT(errno, "set policy failed");
        /* set thread priority */
        if ((err=pthread_attr_setschedparam(&attr, &param)) != 0)
                EXIT(errno, "set param failed");

#if defined MUTEX_LOCK || defined SPIN_LOCK
	lock_init();
#endif

#if defined RTM_LOCK || defined HLE_LOCK
        lock_init(&lockvar);
#endif

	pthread_barrier_init(&barrier, NULL, NUM_READER + NUM_WRITER);

	for(i=0; i<NUM_READER; i++)
		if ((err=pthread_create(&reader[i], NULL, &read, NULL)) != 0)
			EXIT(errno, "create read 1 pthread error");
	for(i=0; i<NUM_WRITER; i++)
		if ((err=pthread_create(&writer[i], NULL, &write, NULL)) != 0)
			EXIT(errno, "create writer thread 1 error");

	for(i=0; i<NUM_READER; i++)
		pthread_join(reader[i], NULL);
	for(i=0; i<NUM_WRITER; i++)
		pthread_join(writer[i], NULL);

        fp = fopen("./time_dememtion.txt", "w+");
        if (!fp) {
                printf("%s\n", strerror(errno));
                return errno;
        }
        for (i = 0; i < NUM_READER + NUM_WRITER; ++i) {
                memset(sz, 0, 128);
                sprintf(sz, "%d %lf\n", i, thread_rw_time[i]);
                if (fwrite(sz, sizeof(char), strlen(sz), fp) == 0) {
                        printf("%s\n", strerror(errno));
                        return errno;
                }
        }
	return 0;
}

