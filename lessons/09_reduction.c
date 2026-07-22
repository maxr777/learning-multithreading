/* Lesson 09: half-open ranges cover [0, ITEM_COUNT). Workers write separate
 * partials, and joining makes those partials visible before reduction. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deliberately not divisible by THREAD_COUNT: the remainder path is exercised. */
enum { ITEM_COUNT = 100003, THREAD_COUNT = 4 };
typedef struct {
    int lane_index;
    int begin;
    int end;
    long long partial_sum;
} WorkerArgument;

static int values[ITEM_COUNT];

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *sum_range(void *argument)
{
    WorkerArgument *worker = argument;
    worker->begin = ITEM_COUNT * worker->lane_index / THREAD_COUNT;
    worker->end = ITEM_COUNT * (worker->lane_index + 1) / THREAD_COUNT;
    for (int i = worker->begin; i < worker->end; i++) {
        worker->partial_sum += values[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    WorkerArgument arguments[THREAD_COUNT] = { 0 };
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < ITEM_COUNT; i++) {
        values[i] = i;
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        arguments[i].lane_index = i;
        check_pthread(pthread_create(&threads[i], NULL, sum_range, &arguments[i]),
                      "pthread_create");
    }

    long long sum = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
        sum += arguments[i].partial_sum;
    }
    long long expected = (long long)ITEM_COUNT * (ITEM_COUNT - 1) / 2;
    int quiet = argc > 1 && strcmp(argv[1], "--quiet") == 0;
    int trace = argc > 1 && strcmp(argv[1], "--trace") == 0;
    if (!quiet) {
        printf("reduction=%lld %s\n", sum, sum == expected ? "PASS" : "FAIL");
    }
    if (trace) {
        for (int i = 0; i < THREAD_COUNT; i++) {
            printf("  lane=%d range=[%d,%d) partial=%lld\n", i,
                   arguments[i].begin, arguments[i].end,
                   arguments[i].partial_sum);
        }
    }
    return sum == expected ? 0 : 1;
}
