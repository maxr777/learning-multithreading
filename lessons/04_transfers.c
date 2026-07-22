/* Lesson 04: multiple locks need one global acquisition order.
 * Stable unique account IDs determine it, regardless of transfer direction. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TRANSFER_COUNT = 100000 };

typedef struct {
    int id;
    long balance;
    pthread_mutex_t mutex;
} Account;

typedef struct {
    Account *from;
    Account *to;
} TransferWorker;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void transfer_one(Account *from, Account *to)
{
    if (from == to || from->id == to->id) {
        fputs("transfer requires two accounts with unique IDs\n", stderr);
        exit(EXIT_FAILURE);
    }
    Account *first = from->id < to->id ? from : to;
    Account *second = from->id < to->id ? to : from;

    check_pthread(pthread_mutex_lock(&first->mutex), "pthread_mutex_lock first");
    check_pthread(pthread_mutex_lock(&second->mutex), "pthread_mutex_lock second");

    /* Both balances form one invariant and are protected at the same time. */
    if (from->balance > 0) {
        from->balance--;
        to->balance++;
    }

    check_pthread(pthread_mutex_unlock(&second->mutex),
                  "pthread_mutex_unlock second");
    check_pthread(pthread_mutex_unlock(&first->mutex),
                  "pthread_mutex_unlock first");
}

static void *run_transfers(void *argument)
{
    TransferWorker *worker = argument;
    for (int i = 0; i < TRANSFER_COUNT; i++) {
        transfer_one(worker->from, worker->to);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    Account accounts[2] = {
        { .id = 0, .balance = 1000 },
        { .id = 1, .balance = 1000 }
    };
    TransferWorker workers[2] = {
        { .from = &accounts[0], .to = &accounts[1] },
        { .from = &accounts[1], .to = &accounts[0] }
    };
    pthread_t threads[2];

    for (int i = 0; i < 2; i++) {
        check_pthread(pthread_mutex_init(&accounts[i].mutex, NULL),
                      "pthread_mutex_init");
    }
    for (int i = 0; i < 2; i++) {
        check_pthread(pthread_create(&threads[i], NULL, run_transfers,
                                     &workers[i]),
                      "pthread_create");
    }
    for (int i = 0; i < 2; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    }

    long total = accounts[0].balance + accounts[1].balance;
    int valid = total == 2000 && accounts[0].balance >= 0 &&
                accounts[1].balance >= 0;
    int checking = argc > 1 && strcmp(argv[1], "--check") == 0;
    printf("balances=[%ld,%ld] total=%ld invariant=%s%s\n",
           accounts[0].balance, accounts[1].balance, total,
           valid ? "valid" : "BROKEN", checking && valid ? " PASS" : "");

    for (int i = 0; i < 2; i++) {
        check_pthread(pthread_mutex_destroy(&accounts[i].mutex),
                      "pthread_mutex_destroy");
    }
    return valid ? 0 : 1;
}
