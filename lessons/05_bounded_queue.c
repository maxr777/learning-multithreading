/* Lesson 05: condition variables are hints to re-check predicates. Wait in loops:
 * wakeups may be spurious, or another consumer may get there first.
 * "closed" means that no producer will push another item. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QUEUE_CAPACITY = 8,
    ITEM_COUNT = 1000,
    PRODUCER_COUNT = 2,
    CONSUMER_COUNT = 3
};

typedef struct {
    int items[QUEUE_CAPACITY];
    int head;
    int count;
    int closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} Queue;

typedef struct {
    int first_value;
    int one_past_last_value;
} ProducerArgument;

static Queue queue;
static long consumed_sum;
static int consumed_count;
static int times_consumed[ITEM_COUNT + 1];

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void queue_push(Queue *queue_to_push, int value)
{
    check_pthread(pthread_mutex_lock(&queue_to_push->mutex),
                  "pthread_mutex_lock");

    /* Predicate: there is room for one more item. */
    while (queue_to_push->count == QUEUE_CAPACITY) {
        check_pthread(pthread_cond_wait(&queue_to_push->not_full,
                                        &queue_to_push->mutex),
                      "pthread_cond_wait");
    }

    int tail = (queue_to_push->head + queue_to_push->count) % QUEUE_CAPACITY;
    queue_to_push->items[tail] = value;
    queue_to_push->count++;

    /* One new item can enable one consumer. */
    check_pthread(pthread_cond_signal(&queue_to_push->not_empty),
                  "pthread_cond_signal");
    check_pthread(pthread_mutex_unlock(&queue_to_push->mutex),
                  "pthread_mutex_unlock");
}

static int queue_pop(Queue *queue_to_pop, int *value)
{
    check_pthread(pthread_mutex_lock(&queue_to_pop->mutex),
                  "pthread_mutex_lock");

    /* Predicate: work exists, or shutdown says that no work can ever arrive. */
    while (queue_to_pop->count == 0 && !queue_to_pop->closed) {
        check_pthread(pthread_cond_wait(&queue_to_pop->not_empty,
                                        &queue_to_pop->mutex),
                      "pthread_cond_wait");
    }

    if (queue_to_pop->count == 0) {
        /* Since closed is true here, an empty queue will remain empty. */
        check_pthread(pthread_mutex_unlock(&queue_to_pop->mutex),
                      "pthread_mutex_unlock");
        return 0;
    }

    *value = queue_to_pop->items[queue_to_pop->head];
    queue_to_pop->head = (queue_to_pop->head + 1) % QUEUE_CAPACITY;
    queue_to_pop->count--;

    /* Validation state uses the same lock, so duplicate/missing work is visible. */
    consumed_count++;
    if (*value >= 1 && *value <= ITEM_COUNT) {
        times_consumed[*value]++;
    }

    /* Removing one item can enable one producer. */
    check_pthread(pthread_cond_signal(&queue_to_pop->not_full),
                  "pthread_cond_signal");
    check_pthread(pthread_mutex_unlock(&queue_to_pop->mutex),
                  "pthread_mutex_unlock");
    return 1;
}

static void *consume_items(void *unused)
{
    (void)unused;
    long local_sum = 0;
    int value;

    while (queue_pop(&queue, &value)) {
        local_sum += value;
    }

    /* This could instead be returned per worker and reduced after join. */
    check_pthread(pthread_mutex_lock(&queue.mutex), "pthread_mutex_lock");
    consumed_sum += local_sum;
    check_pthread(pthread_mutex_unlock(&queue.mutex), "pthread_mutex_unlock");
    return NULL;
}

static void *produce_items(void *argument)
{
    ProducerArgument *producer = argument;
    for (int value = producer->first_value;
         value < producer->one_past_last_value; value++) {
        queue_push(&queue, value);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t consumers[CONSUMER_COUNT];
    pthread_t producers[PRODUCER_COUNT];
    ProducerArgument producer_arguments[PRODUCER_COUNT];

    check_pthread(pthread_mutex_init(&queue.mutex, NULL), "pthread_mutex_init");
    check_pthread(pthread_cond_init(&queue.not_empty, NULL), "pthread_cond_init");
    check_pthread(pthread_cond_init(&queue.not_full, NULL), "pthread_cond_init");

    for (int i = 0; i < CONSUMER_COUNT; i++) {
        check_pthread(pthread_create(&consumers[i], NULL, consume_items, NULL),
                      "pthread_create");
    }

    for (int i = 0; i < PRODUCER_COUNT; i++) {
        int first = 1 + ITEM_COUNT * i / PRODUCER_COUNT;
        int end = 1 + ITEM_COUNT * (i + 1) / PRODUCER_COUNT;
        producer_arguments[i] = (ProducerArgument){
            .first_value = first,
            .one_past_last_value = end
        };
        check_pthread(pthread_create(&producers[i], NULL, produce_items,
                                     &producer_arguments[i]),
                      "pthread_create producer");
    }
    for (int i = 0; i < PRODUCER_COUNT; i++) {
        check_pthread(pthread_join(producers[i], NULL), "pthread_join producer");
    }

    /* Main owns closure: it closes only after every producer has returned. */
    check_pthread(pthread_mutex_lock(&queue.mutex), "pthread_mutex_lock");
    queue.closed = 1;
    /* Every consumer must wake so each can observe closed and eventually exit. */
    check_pthread(pthread_cond_broadcast(&queue.not_empty),
                  "pthread_cond_broadcast");
    check_pthread(pthread_mutex_unlock(&queue.mutex), "pthread_mutex_unlock");

    for (int i = 0; i < CONSUMER_COUNT; i++) {
        check_pthread(pthread_join(consumers[i], NULL), "pthread_join");
    }

    long expected_sum = (long)ITEM_COUNT * (ITEM_COUNT + 1) / 2;
    int valid = consumed_sum == expected_sum && consumed_count == ITEM_COUNT;
    for (int value = 1; value <= ITEM_COUNT; value++) {
        if (times_consumed[value] != 1) {
            valid = 0;
        }
    }
    int quiet = argc > 1 && strcmp(argv[1], "--quiet") == 0;
    if (!quiet) {
        printf("queue count=%d sum=%ld %s\n", consumed_count, consumed_sum,
               valid ? "PASS" : "FAIL");
    }

    check_pthread(pthread_cond_destroy(&queue.not_full), "pthread_cond_destroy");
    check_pthread(pthread_cond_destroy(&queue.not_empty), "pthread_cond_destroy");
    check_pthread(pthread_mutex_destroy(&queue.mutex), "pthread_mutex_destroy");
    return valid ? 0 : 1;
}
