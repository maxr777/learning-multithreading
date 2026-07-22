/* Lesson 07: a release store publishes earlier ordinary writes to an acquire
 * load that observes it. The atomic flag is synchronization; the payload is not
 * atomic because happens-before prevents concurrent conflicting access. */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int sequence;
    int values[4];
    long checksum;
} Payload;

static Payload payload;
static atomic_bool ready;
static int payload_valid;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *produce(void *unused)
{
    (void)unused;
    payload.sequence = 42;
    payload.values[0] = 3;
    payload.values[1] = 5;
    payload.values[2] = 8;
    payload.values[3] = 13;
    payload.checksum = payload.sequence;
    for (int i = 0; i < 4; i++) {
        payload.checksum += payload.values[i];
    }

    /* Every payload write above is sequenced before this release store. */
    atomic_store_explicit(&ready, true, memory_order_release);
    return NULL;
}

static void *consume(void *unused)
{
    (void)unused;
    while (!atomic_load_explicit(&ready, memory_order_acquire)) {
        /* Yield is only politeness while spinning; it is not synchronization. */
        sched_yield();
    }

    /* The acquire that observed ready=true imports all published payload writes. */
    long expected = payload.sequence;
    for (int i = 0; i < 4; i++) {
        expected += payload.values[i];
    }
    payload_valid = payload.sequence == 42 && payload.values[0] == 3 &&
                    payload.values[1] == 5 && payload.values[2] == 8 &&
                    payload.values[3] == 13 && payload.checksum == expected;
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t producer;
    pthread_t consumer;
    atomic_init(&ready, false);

    /* Start the waiter first so the publication edge is easy to observe. */
    check_pthread(pthread_create(&consumer, NULL, consume, NULL),
                  "pthread_create consumer");
    check_pthread(pthread_create(&producer, NULL, produce, NULL),
                  "pthread_create producer");
    check_pthread(pthread_join(producer, NULL), "pthread_join producer");
    check_pthread(pthread_join(consumer, NULL), "pthread_join consumer");

    int checking = argc > 1 && strcmp(argv[1], "--check") == 0;
    printf("published sequence=%d checksum=%ld %s%s\n", payload.sequence,
           payload.checksum, payload_valid ? "PASS" : "FAIL",
           checking && payload_valid ? " PASS" : "");
    return payload_valid ? 0 : 1;
}
