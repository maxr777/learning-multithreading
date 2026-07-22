/* Lesson 10: a generation distinguishes one barrier use from the next.
 * Every condition wait is a loop because wakeups are hints, not permission. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { THREAD_COUNT = 4, ROUND_COUNT = 20 };
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int arrived;
    unsigned generation;
    int participant_count;
} Barrier;

static Barrier barrier;
static int completed_phase[THREAD_COUNT];
static int trace_enabled;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void barrier_wait(Barrier *barrier_to_wait_on, int lane, int round,
                         const char *phase)
{
    check_pthread(pthread_mutex_lock(&barrier_to_wait_on->mutex), "pthread_mutex_lock");
    unsigned my_generation = barrier_to_wait_on->generation;
    barrier_to_wait_on->arrived++;
    if (trace_enabled) {
        printf("lane=%d round=%d point=%s generation=%u arrived=%d/%d%s\n",
               lane, round, phase, my_generation, barrier_to_wait_on->arrived,
               barrier_to_wait_on->participant_count,
               barrier_to_wait_on->arrived == barrier_to_wait_on->participant_count
                   ? " LAST"
                   : "");
    }
    if (barrier_to_wait_on->arrived == barrier_to_wait_on->participant_count) {
        barrier_to_wait_on->arrived = 0;
        barrier_to_wait_on->generation++;
        check_pthread(pthread_cond_broadcast(&barrier_to_wait_on->changed),
                      "pthread_cond_broadcast");
    } else {
        while (my_generation == barrier_to_wait_on->generation) {
            check_pthread(pthread_cond_wait(&barrier_to_wait_on->changed,
                                            &barrier_to_wait_on->mutex),
                          "pthread_cond_wait");
        }
    }
    check_pthread(pthread_mutex_unlock(&barrier_to_wait_on->mutex), "pthread_mutex_unlock");
}

static void *run_rounds(void *argument)
{
    int lane = *(int *)argument;
    for (int round = 1; round <= ROUND_COUNT; round++) {
        completed_phase[lane] = round;
        barrier_wait(&barrier, lane, round, "writes-complete");

        for (int other = 0; other < THREAD_COUNT; other++) {
            if (completed_phase[other] != round) {
                abort();
            }
        }
        /* Keep readers in this round until every lane has finished reading. */
        barrier_wait(&barrier, lane, round, "reads-complete");
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[THREAD_COUNT];
    int lane_ids[THREAD_COUNT];
    barrier.participant_count = THREAD_COUNT;
    trace_enabled = argc > 1 && strcmp(argv[1], "--trace") == 0;
    check_pthread(pthread_mutex_init(&barrier.mutex, NULL), "pthread_mutex_init");
    check_pthread(pthread_cond_init(&barrier.changed, NULL), "pthread_cond_init");

    for (int i = 0; i < THREAD_COUNT; i++) {
        lane_ids[i] = i;
        check_pthread(pthread_create(&threads[i], NULL, run_rounds, &lane_ids[i]),
                      "pthread_create");
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    }

    printf("barrier rounds=%d%s\n", ROUND_COUNT,
           argc > 1 && strcmp(argv[1], "--check") == 0 ? " PASS" : "");
    check_pthread(pthread_cond_destroy(&barrier.changed), "pthread_cond_destroy");
    check_pthread(pthread_mutex_destroy(&barrier.mutex), "pthread_mutex_destroy");
    return 0;
}
