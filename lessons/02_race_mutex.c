/* --unsafe intentionally has undefined behavior. A plausible result from
 * concurrent non-atomic accesses is not evidence of correctness. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { THREAD_COUNT = 4, ITERATIONS = 100000 };

static long counter;
static pthread_mutex_t counter_mutex;
static int unsafe_mode;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *increment_counter(void *unused)
{
    (void)unused;
    for (int i = 0; i < ITERATIONS; i++) {
        if (unsafe_mode) {
            counter++;
        } else {
            /* The lock makes the read-modify-write one critical section. */
            check_pthread(pthread_mutex_lock(&counter_mutex), "pthread_mutex_lock");
            counter++;
            check_pthread(pthread_mutex_unlock(&counter_mutex), "pthread_mutex_unlock");
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[THREAD_COUNT];
    unsafe_mode = argc > 1 && strcmp(argv[1], "--unsafe") == 0;
    check_pthread(pthread_mutex_init(&counter_mutex, NULL), "pthread_mutex_init");

    for (int i = 0; i < THREAD_COUNT; i++) {
        check_pthread(pthread_create(&threads[i], NULL, increment_counter, NULL),
                      "pthread_create");
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    }

    long expected = (long)THREAD_COUNT * ITERATIONS;
    printf("mode=%s got=%ld want=%ld%s\n", unsafe_mode ? "UNSAFE" : "mutex",
           counter, expected, !unsafe_mode && counter == expected ? " PASS" : "");
    check_pthread(pthread_mutex_destroy(&counter_mutex), "pthread_mutex_destroy");
    return !unsafe_mode && counter != expected ? 1 : 0;
}
