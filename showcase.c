/* A Mandelbrot renderer organized as a fixed lane group:
 *
 *   - every lane runs the same entry point;
 *   - an atomic counter dynamically distributes rows;
 *   - each lane accumulates into stack-local state;
 *   - a reusable barrier separates wide and narrow phases;
 *   - lane 0 alone reduces, reports, and writes the image.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t generation_changed;
    int arrived;
    int participant_count;
    unsigned generation;
} Barrier;

typedef struct Shared Shared;

typedef struct {
    Shared *shared;
    int lane_index;
    int row_count;
    uint64_t iteration_count;
} Lane;

struct Shared {
    int width;
    int height;
    int lane_count;
    int maximum_iterations;
    int trace_lanes;
    const char *output_path;
    unsigned char *pixels;
    atomic_int next_row;
    Barrier barrier;
    Lane *lanes;
    int output_failed;
};

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(error));
        exit(EXIT_FAILURE);
    }
}

static void barrier_init(Barrier *barrier, int participant_count)
{
    *barrier = (Barrier){ .participant_count = participant_count };
    check_pthread(pthread_mutex_init(&barrier->mutex, NULL),
                  "pthread_mutex_init");
    check_pthread(pthread_cond_init(&barrier->generation_changed, NULL),
                  "pthread_cond_init");
}

static void barrier_destroy(Barrier *barrier)
{
    check_pthread(pthread_cond_destroy(&barrier->generation_changed),
                  "pthread_cond_destroy");
    check_pthread(pthread_mutex_destroy(&barrier->mutex),
                  "pthread_mutex_destroy");
}

static void barrier_wait(Barrier *barrier)
{
    check_pthread(pthread_mutex_lock(&barrier->mutex), "pthread_mutex_lock");
    unsigned my_generation = barrier->generation;

    barrier->arrived++;
    if (barrier->arrived == barrier->participant_count) {
        barrier->arrived = 0;
        barrier->generation++;
        check_pthread(pthread_cond_broadcast(&barrier->generation_changed),
                      "pthread_cond_broadcast");
    } else {
        while (my_generation == barrier->generation) {
            check_pthread(pthread_cond_wait(&barrier->generation_changed,
                                            &barrier->mutex),
                          "pthread_cond_wait");
        }
    }

    check_pthread(pthread_mutex_unlock(&barrier->mutex),
                  "pthread_mutex_unlock");
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

static uint64_t hash_bytes(const unsigned char *bytes, size_t byte_count)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < byte_count; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int write_ppm(const Shared *shared)
{
    FILE *file = fopen(shared->output_path, "wb");
    if (file == NULL) {
        perror(shared->output_path);
        return 0;
    }

    size_t pixel_count = (size_t)shared->width * (size_t)shared->height;
    int header_failed = fprintf(file, "P6\n%d %d\n255\n", shared->width,
                                shared->height) < 0;
    int pixels_failed = fwrite(shared->pixels, 3, pixel_count, file) != pixel_count;
    if (header_failed || pixels_failed) {
        perror("write PPM");
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        perror("close PPM");
        return 0;
    }
    return 1;
}

static int render_row(Shared *shared, int y)
{
    int row_iterations = 0;

    for (int x = 0; x < shared->width; x++) {
        double real = ((double)x / (double)shared->width) * 3.2 - 2.2;
        double imaginary = ((double)y / (double)shared->height) * 2.2 - 1.1;
        double z_real = 0.0;
        double z_imaginary = 0.0;
        int iterations = 0;

        while (iterations < shared->maximum_iterations &&
               z_real * z_real + z_imaginary * z_imaginary <= 4.0) {
            double next_real = z_real * z_real - z_imaginary * z_imaginary + real;
            z_imaginary = 2.0 * z_real * z_imaginary + imaginary;
            z_real = next_real;
            iterations++;
        }

        size_t pixel_index = (size_t)y * (size_t)shared->width + (size_t)x;
        unsigned char *pixel = &shared->pixels[pixel_index * 3];
        pixel[0] = (unsigned char)(iterations == shared->maximum_iterations
                                      ? 0
                                      : (iterations * 9) % 256);
        pixel[1] = (unsigned char)(iterations == shared->maximum_iterations
                                      ? 0
                                      : (iterations * 5) % 256);
        pixel[2] = (unsigned char)(iterations == shared->maximum_iterations
                                      ? 0
                                      : (iterations * 13) % 256);
        row_iterations += iterations;
    }

    return row_iterations;
}

static void *lane_main(void *argument)
{
    Lane *lane = argument;
    Shared *shared = lane->shared;
    uint64_t local_iterations = 0;
    int local_rows = 0;

    /* Wide phase: lanes dynamically claim rows and write disjoint pixels. */
    for (;;) {
        int row = atomic_fetch_add_explicit(&shared->next_row, 1,
                                            memory_order_relaxed);
        if (row >= shared->height) {
            break;
        }
        local_iterations += (uint64_t)render_row(shared, row);
        local_rows++;
    }

    /* Publish once, rather than repeatedly writing adjacent Lane structures. */
    lane->iteration_count = local_iterations;
    lane->row_count = local_rows;
    barrier_wait(&shared->barrier);

    /* Narrow phase: every wide-phase write is complete before lane 0 enters. */
    if (lane->lane_index == 0) {
        uint64_t total_iterations = 0;
        for (int i = 0; i < shared->lane_count; i++) {
            total_iterations += shared->lanes[i].iteration_count;
        }

        shared->output_failed = !write_ppm(shared);
        size_t byte_count = (size_t)shared->width * (size_t)shared->height * 3;
        printf("render %dx%d lanes=%d iterations=%llu checksum=%llu output=%s\n",
               shared->width, shared->height, shared->lane_count,
               (unsigned long long)total_iterations,
               (unsigned long long)hash_bytes(shared->pixels, byte_count),
               shared->output_path);
        if (shared->trace_lanes) {
            for (int i = 0; i < shared->lane_count; i++) {
                printf("  lane=%d rows=%d iterations=%llu\n", i,
                       shared->lanes[i].row_count,
                       (unsigned long long)shared->lanes[i].iteration_count);
            }
            puts("Lane counts are observations; scheduling may differ next run.");
        }
    }

    /* Delimit the narrow phase before a possible next use of this lane group.
     * In this one-shot program, joins—not this barrier—protect teardown. */
    barrier_wait(&shared->barrier);
    return NULL;
}

static int parse_number(const char *text, int minimum, int maximum,
                        const char *name)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value < minimum || value > maximum) {
        fprintf(stderr, "invalid %s: %s (expected %d..%d)\n", name, text,
                minimum, maximum);
        exit(2);
    }
    return (int)value;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--serial | --threads N] "
            "[--width N --height N --iterations N --output FILE] "
            "[--trace-lanes]\n",
            program);
}

int main(int argc, char **argv)
{
    long online_cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    int lane_count = 1;
    if (online_cpu_count > 0) {
        lane_count = online_cpu_count > 64 ? 64 : (int)online_cpu_count;
    }
    int width = 800;
    int height = 450;
    int maximum_iterations = 300;
    int trace_lanes = 0;
    const char *output_path = "showcase.ppm";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serial") == 0) {
            lane_count = 1;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            lane_count = parse_number(argv[++i], 1, 256, "threads");
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = parse_number(argv[++i], 16, 10000, "width");
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = parse_number(argv[++i], 16, 10000, "height");
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            maximum_iterations = parse_number(argv[++i], 1, 100000,
                                               "iterations");
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--trace-lanes") == 0) {
            trace_lanes = 1;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if ((size_t)width > SIZE_MAX / (size_t)height / 3) {
        fputs("image is too large\n", stderr);
        return 2;
    }

    Shared shared = {
        .width = width,
        .height = height,
        .lane_count = lane_count,
        .maximum_iterations = maximum_iterations,
        .trace_lanes = trace_lanes,
        .output_path = output_path
    };
    atomic_init(&shared.next_row, 0);
    barrier_init(&shared.barrier, lane_count);

    size_t byte_count = (size_t)width * (size_t)height * 3;
    shared.pixels = malloc(byte_count);
    shared.lanes = calloc((size_t)lane_count, sizeof *shared.lanes);
    size_t worker_count = lane_count > 1 ? (size_t)lane_count - 1 : 1;
    pthread_t *threads = calloc(worker_count, sizeof *threads);
    if (shared.pixels == NULL || shared.lanes == NULL || threads == NULL) {
        fputs("allocation failed\n", stderr);
        free(threads);
        free(shared.lanes);
        free(shared.pixels);
        barrier_destroy(&shared.barrier);
        return 1;
    }

    for (int i = 0; i < lane_count; i++) {
        shared.lanes[i] = (Lane){ .shared = &shared, .lane_index = i };
    }

    double start = monotonic_seconds();
    /* The main thread becomes lane 0 and participates in the same work loop. */
    for (int i = 1; i < lane_count; i++) {
        check_pthread(pthread_create(&threads[i - 1], NULL, lane_main,
                                     &shared.lanes[i]),
                      "pthread_create");
    }
    lane_main(&shared.lanes[0]);
    for (int i = 1; i < lane_count; i++) {
        check_pthread(pthread_join(threads[i - 1], NULL), "pthread_join");
    }
    double elapsed = monotonic_seconds() - start;
    printf("end-to-end wall time %.3fs\n", elapsed);

    int exit_status = shared.output_failed ? 1 : 0;
    free(threads);
    free(shared.lanes);
    free(shared.pixels);
    barrier_destroy(&shared.barrier);
    return exit_status;
}
