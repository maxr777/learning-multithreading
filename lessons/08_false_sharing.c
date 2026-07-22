/* Lesson 08: adjacent counters may share a line, causing ownership ping-pong.
 * Padding is illustrative: 64 bytes is common, not guaranteed. Timings vary by
 * CPU, compiler, affinity, and noise. This is an experiment, not a promise. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    THREAD_COUNT = 4,
    ITERATIONS = 5000000,
    TRIAL_COUNT = 5,
    ASSUMED_CACHE_LINE_SIZE = 64
};

typedef struct {
    volatile uint64_t value;
} PackedCounter;

typedef struct {
    volatile uint64_t value;
    char padding[ASSUMED_CACHE_LINE_SIZE - sizeof(uint64_t)];
} PaddedCounter;

typedef struct {
    int lane_index;
    int use_padding;
} WorkerArgument;

static PackedCounter packed_counters[THREAD_COUNT];
static PaddedCounter padded_counters[THREAD_COUNT];

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static double monotonic_seconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static void *increment_own_counter(void *argument)
{
    WorkerArgument *worker = argument;

    for (int i = 0; i < ITERATIONS; i++) {
        if (worker->use_padding) {
            padded_counters[worker->lane_index].value++;
        } else {
            packed_counters[worker->lane_index].value++;
        }
    }
    return NULL;
}

static double run_trial(int use_padding)
{
    pthread_t threads[THREAD_COUNT];
    WorkerArgument arguments[THREAD_COUNT];
    double start = monotonic_seconds();

    for (int i = 0; i < THREAD_COUNT; i++) {
        arguments[i] = (WorkerArgument){
            .lane_index = i,
            .use_padding = use_padding
        };
        check_pthread(pthread_create(&threads[i], NULL, increment_own_counter,
                                     &arguments[i]),
                      "pthread_create");
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    }

    return monotonic_seconds() - start;
}

static double median(double samples[TRIAL_COUNT])
{
    for (int i = 1; i < TRIAL_COUNT; i++) {
        double value = samples[i];
        int position = i;
        while (position > 0 && samples[position - 1] > value) {
            samples[position] = samples[position - 1];
            position--;
        }
        samples[position] = value;
    }
    return samples[TRIAL_COUNT / 2];
}

static double minimum(const double samples[TRIAL_COUNT])
{
    double result = samples[0];
    for (int i = 1; i < TRIAL_COUNT; i++) {
        if (samples[i] < result) {
            result = samples[i];
        }
    }
    return result;
}

static double maximum(const double samples[TRIAL_COUNT])
{
    double result = samples[0];
    for (int i = 1; i < TRIAL_COUNT; i++) {
        if (samples[i] > result) {
            result = samples[i];
        }
    }
    return result;
}

int main(void)
{
    double packed_samples[TRIAL_COUNT];
    double padded_samples[TRIAL_COUNT];

    /* Warm both paths before measuring, then alternate which layout runs first. */
    (void)run_trial(0);
    (void)run_trial(1);
    for (int trial = 0; trial < TRIAL_COUNT; trial++) {
        if (trial % 2 == 0) {
            packed_samples[trial] = run_trial(0);
            padded_samples[trial] = run_trial(1);
        } else {
            padded_samples[trial] = run_trial(1);
            packed_samples[trial] = run_trial(0);
        }
    }

    printf("packed: sizeof=%zu align=%zu\n", sizeof(PackedCounter),
           (size_t)_Alignof(PackedCounter));
    printf("padded: sizeof=%zu align=%zu\n", sizeof(PaddedCounter),
           (size_t)_Alignof(PaddedCounter));
    for (int i = 0; i < THREAD_COUNT; i++) {
        printf("  lane=%d packed=%p padded=%p\n", i,
               (void *)&packed_counters[i], (void *)&padded_counters[i]);
    }
    printf("raw samples (packed/padded seconds):");
    for (int i = 0; i < TRIAL_COUNT; i++) {
        printf(" %.4f/%.4f", packed_samples[i], padded_samples[i]);
    }
    putchar('\n');

    double packed_minimum = minimum(packed_samples);
    double packed_maximum = maximum(packed_samples);
    double padded_minimum = minimum(padded_samples);
    double padded_maximum = maximum(padded_samples);
    double packed_seconds = median(packed_samples);
    double padded_seconds = median(padded_samples);
    uint64_t expected = (uint64_t)(TRIAL_COUNT + 1) * ITERATIONS;
    int valid = 1;
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (packed_counters[i].value != expected ||
            padded_counters[i].value != expected) {
            valid = 0;
        }
    }

    printf("median of %d: packed %.3fs [%.3f,%.3f] "
           "padded %.3fs [%.3f,%.3f] ratio %.2f %s\n",
           TRIAL_COUNT, packed_seconds, packed_minimum, packed_maximum,
           padded_seconds, padded_minimum, padded_maximum,
           packed_seconds / padded_seconds, valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}
