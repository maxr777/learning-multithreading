/* Lesson 06: fetch_add gives every worker a unique ticket. Relaxed is sufficient:
 * tickets select disjoint slots, and pthread_join publishes result writes. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VALUE_COUNT = 10000, THREAD_COUNT = 4 };
static atomic_int next_index;
static int values[VALUE_COUNT];

typedef struct {
    int lane_index;
    int claim_count;
} WorkerArgument;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *fill_values(void *argument)
{
    WorkerArgument *worker = argument;
    for (;;) {
        int index = atomic_fetch_add_explicit(&next_index, 1, memory_order_relaxed);
        if (index >= VALUE_COUNT) {
            break;
        }
        values[index] = index * 2 + 1;
        worker->claim_count++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[THREAD_COUNT];
    WorkerArgument workers[THREAD_COUNT] = { 0 };
    atomic_init(&next_index, 0);
    for (int i = 0; i < THREAD_COUNT; i++) {
        workers[i].lane_index = i;
        check_pthread(pthread_create(&threads[i], NULL, fill_values, &workers[i]),
                      "pthread_create");
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    }

    long sum = 0;
    int valid = 1;
    for (int i = 0; i < VALUE_COUNT; i++) {
        if (values[i] != i * 2 + 1) {
            valid = 0;
        }
        sum += values[i];
    }
    if (sum != (long)VALUE_COUNT * VALUE_COUNT) {
        valid = 0;
    }
    int quiet = argc > 1 && strcmp(argv[1], "--quiet") == 0;
    int trace = argc > 1 && strcmp(argv[1], "--trace") == 0;
    if (!quiet) {
        printf("atomic tickets sum=%ld %s\n", sum, valid ? "PASS" : "FAIL");
    }
    if (trace) {
        for (int i = 0; i < THREAD_COUNT; i++) {
            printf("  lane=%d claims=%d\n", workers[i].lane_index,
                   workers[i].claim_count);
        }
        puts("Claim counts are observations, not a scheduling guarantee.");
    }
    return valid ? 0 : 1;
}
