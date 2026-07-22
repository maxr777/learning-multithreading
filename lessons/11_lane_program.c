/* Lesson 11: one persistent lane group executes several wide/narrow rounds.
 * This intentionally duplicates the barrier machinery so no framework hides it. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ROUND_COUNT = 3, MAX_LANES = 64, VALUE_COUNT = 37 };

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int arrived;
    int participant_count;
    unsigned generation;
} Barrier;

typedef struct Shared Shared;

typedef struct {
    Shared *shared;
    int lane_index;
    long partial_sum;
} Lane;

struct Shared {
    int lane_count;
    int trace;
    int active_count;
    int values[VALUE_COUNT];
    Lane *lanes;
    Barrier barrier;
    int valid;
};

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void barrier_wait(Barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock");
    unsigned generation = barrier->generation;
    barrier->arrived++;
    if (barrier->arrived == barrier->participant_count) {
        barrier->arrived = 0;
        barrier->generation++;
        check_pthread(pthread_cond_broadcast(&barrier->changed),
                      "pthread_cond_broadcast");
    } else {
        while (generation == barrier->generation) {
            check_pthread(pthread_cond_wait(&barrier->changed, &barrier->mutex),
                          "pthread_cond_wait");
        }
    }
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "pthread_mutex_unlock");
}

static void lane_range(int count, int lane_index, int lane_count,
                       int *begin, int *end)
{
    int quotient = count / lane_count;
    int remainder = count % lane_count;
    int gets_extra = lane_index < remainder;
    int extras_before = lane_index < remainder ? lane_index : remainder;
    *begin = lane_index * quotient + extras_before;
    *end = *begin + quotient + gets_extra;
}

static void *lane_main(void *argument)
{
    Lane *lane = argument;
    Shared *shared = lane->shared;

    for (int round = 1; round <= ROUND_COUNT; round++) {
        /* Narrow: lane 0 prepares immutable input for this round. */
        if (lane->lane_index == 0) {
            shared->active_count = VALUE_COUNT - (round - 1);
            for (int i = 0; i < shared->active_count; i++) {
                shared->values[i] = round * 100 + i;
            }
        }
        /* This publishes both active_count and the prepared values. */
        barrier_wait(&shared->barrier);

        /* Wide: every lane executes the same code over its own balanced range. */
        int begin;
        int end;
        lane_range(shared->active_count, lane->lane_index, shared->lane_count,
                   &begin, &end);
        long partial = 0;
        for (int i = begin; i < end; i++) {
            partial += shared->values[i];
        }
        lane->partial_sum = partial;
        barrier_wait(&shared->barrier); /* Publish every partial to lane 0. */

        /* Narrow: lane 0 reduces and validates this round. */
        if (lane->lane_index == 0) {
            long total = 0;
            for (int i = 0; i < shared->lane_count; i++) {
                total += shared->lanes[i].partial_sum;
            }
            long expected = (long)shared->active_count * (round * 100) +
                            (long)shared->active_count *
                                (shared->active_count - 1) / 2;
            if (total != expected) {
                shared->valid = 0;
            }
            printf("round=%d lanes=%d values=%d total=%ld expected=%ld %s\n",
                   round, shared->lane_count, shared->active_count, total,
                   expected, total == expected ? "PASS" : "FAIL");
            if (shared->trace) {
                for (int i = 0; i < shared->lane_count; i++) {
                    int lane_begin;
                    int lane_end;
                    lane_range(shared->active_count, i, shared->lane_count,
                               &lane_begin, &lane_end);
                    printf("  lane=%d range=[%d,%d) partial=%ld\n", i,
                           lane_begin, lane_end, shared->lanes[i].partial_sum);
                }
            }
        }
        /* Prevent next-round partials from overwriting this round's partials. */
        barrier_wait(&shared->barrier);
    }
    return NULL;
}

static int parse_lanes(const char *text)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value < 1 || value > MAX_LANES) {
        fprintf(stderr, "invalid lane count: %s (expected 1..%d)\n", text,
                MAX_LANES);
        exit(2);
    }
    return (int)value;
}

int main(int argc, char **argv)
{
    int lane_count = 4;
    int trace = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serial") == 0) {
            lane_count = 1;
        } else if (strcmp(argv[i], "--lanes") == 0 && i + 1 < argc) {
            lane_count = parse_lanes(argv[++i]);
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
        } else {
            fprintf(stderr, "usage: %s [--serial | --lanes N] [--trace]\n",
                    argv[0]);
            return 2;
        }
    }

    Shared shared = { .lane_count = lane_count, .trace = trace, .valid = 1 };
    Lane *lanes = calloc((size_t)lane_count, sizeof *lanes);
    size_t worker_count = lane_count > 1 ? (size_t)lane_count - 1 : 1;
    pthread_t *threads = calloc(worker_count, sizeof *threads);
    if (lanes == NULL || threads == NULL) {
        fputs("allocation failed\n", stderr);
        free(threads);
        free(lanes);
        return 1;
    }

    shared.lanes = lanes;
    shared.barrier.participant_count = lane_count;
    check_pthread(pthread_mutex_init(&shared.barrier.mutex, NULL),
                  "pthread_mutex_init");
    check_pthread(pthread_cond_init(&shared.barrier.changed, NULL),
                  "pthread_cond_init");
    for (int i = 0; i < lane_count; i++) {
        lanes[i] = (Lane){ .shared = &shared, .lane_index = i };
    }

    for (int i = 1; i < lane_count; i++) {
        check_pthread(pthread_create(&threads[i - 1], NULL, lane_main, &lanes[i]),
                      "pthread_create");
    }
    lane_main(&lanes[0]);
    for (int i = 1; i < lane_count; i++) {
        check_pthread(pthread_join(threads[i - 1], NULL), "pthread_join");
    }

    check_pthread(pthread_cond_destroy(&shared.barrier.changed),
                  "pthread_cond_destroy");
    check_pthread(pthread_mutex_destroy(&shared.barrier.mutex),
                  "pthread_mutex_destroy");
    free(threads);
    free(lanes);
    return shared.valid ? 0 : 1;
}
