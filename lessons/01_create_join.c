/* Thread arguments must outlive the thread. Joining establishes completion and
 * makes the worker's result safe to read. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int input;
    int output;
} Job;

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void *square(void *argument)
{
    Job *job = argument;
    job->output = job->input * job->input;
    return job;
}

int main(int argc, char **argv)
{
    enum { JOB_COUNT = 4 };
    pthread_t threads[JOB_COUNT];
    Job jobs[JOB_COUNT];

    for (int i = 0; i < JOB_COUNT; i++) {
        jobs[i] = (Job){ .input = i + 1, .output = 0 };
        check_pthread(pthread_create(&threads[i], NULL, square, &jobs[i]),
                      "pthread_create");
    }

    int sum = 0;
    for (int i = 0; i < JOB_COUNT; i++) {
        void *result = NULL;
        check_pthread(pthread_join(threads[i], &result), "pthread_join");
        if (result != &jobs[i]) {
            return 2;
        }
        sum += jobs[i].output;
    }

    int checking = argc > 1 && strcmp(argv[1], "--check") == 0;
    printf("squares sum=%d %s\n", sum, checking && sum == 30 ? "PASS" : "");
    return sum == 30 ? 0 : 1;
}
