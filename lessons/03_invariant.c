/* The mutex protects balance == deposits - withdrawals and balance >= 0.
 * Testing funds and withdrawing must therefore be one critical section. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long balance;
    long deposits;
    long withdrawals;
    pthread_mutex_t mutex;
} Bank;

typedef struct {
    Bank *bank;
    int deposits_money;
} WorkerArgument;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *update_bank(void *argument)
{
    WorkerArgument *worker = argument;
    for (int i = 0; i < 50000; i++) {
        check_pthread(pthread_mutex_lock(&worker->bank->mutex), "pthread_mutex_lock");
        if (worker->deposits_money) {
            worker->bank->balance++;
            worker->bank->deposits++;
        } else if (worker->bank->balance > 0) {
            worker->bank->balance--;
            worker->bank->withdrawals++;
        }
        check_pthread(pthread_mutex_unlock(&worker->bank->mutex), "pthread_mutex_unlock");
    }
    return NULL;
}

int main(int argc, char **argv)
{
    enum { WORKER_COUNT = 4 };
    Bank bank = { 0 };
    WorkerArgument arguments[WORKER_COUNT];
    pthread_t threads[WORKER_COUNT];
    check_pthread(pthread_mutex_init(&bank.mutex, NULL), "pthread_mutex_init");

    for (int i = 0; i < WORKER_COUNT; i++) {
        arguments[i] = (WorkerArgument){ .bank = &bank, .deposits_money = i < 2 };
        check_pthread(pthread_create(&threads[i], NULL, update_bank, &arguments[i]),
                      "pthread_create");
    }
    for (int i = 0; i < WORKER_COUNT; i++) {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    }

    int valid = bank.balance == bank.deposits - bank.withdrawals && bank.balance >= 0;
    int checking = argc > 1 && strcmp(argv[1], "--check") == 0;
    printf("balance=%ld in=%ld out=%ld invariant=%s%s\n", bank.balance,
           bank.deposits, bank.withdrawals, valid ? "yes" : "NO",
           checking && valid ? " PASS" : "");
    check_pthread(pthread_mutex_destroy(&bank.mutex), "pthread_mutex_destroy");
    return valid ? 0 : 1;
}
