# Multithreading from first principles (C + pthreads)

This is a source-first course: read one small file, predict it, run it, change
one fact, rebuild, and explain the observation. There is no job-system library
hiding the machinery. Requirements are a C11 compiler, `make`, and a
POSIX/Linux pthread implementation. The pthread API is POSIX rather than ISO C;
`clock_gettime` and `sysconf` are also POSIX. The check and stress scripts use
POSIX `sh` syntax but require `mktemp` and the Linux/coreutils `timeout` command;
`CHECK_TIMEOUT` and `STRESS_TIMEOUT` override their default 30-second
per-process deadlines. Sanitizer availability and behavior are compiler/OS
dependent.

This is multithreading from zero, not C from zero. You should already be able to
read pointers, arrays, structs, function pointers, stack versus heap lifetime,
integer types, and a basic Makefile. If `void *`, address-of (`&`), or object
lifetime are still unfamiliar, learn those first—the thread API makes all three
impossible to avoid. No prior operating-system or concurrency knowledge is
assumed.

The Makefile passes `-pthread` while compiling and linking. On supported POSIX
toolchains this selects the compiler and C-library thread configuration as well
as linking pthread support; manually adding only `-lpthread` is not always an
equivalent compile-time configuration.

```sh
make                 # strict warnings, optimized but debuggable
make run             # safe tour (never runs the deliberate race)
make check           # safe results + byte-identical serial/threaded PPM images
make stress          # repeat timing-sensitive code 20 times
make tsan-check      # TSan build + complete safe checks, when supported
make showcase        # writes showcase.ppm; many image viewers open PPM
make help
```

`make run` skips lesson 08 because a noisy hardware benchmark does not belong in
the deterministic tour; run `./build/08_false_sharing` separately.

## Part I: quick orientation

This first pass gives you the whole map without demanding that you understand
every detail. Read it once, run `make run`, and then use Part II as the actual
workbook. Repetition here is deliberate: first recognize the vocabulary, then
return to each mechanism with source, predictions, experiments, and checks.

### The machine model

A **process** owns an address space and resources. Its **threads** share that
address space but each has a stack, registers, and instruction position. A CPU
**core** executes instructions; hardware threads may share parts of a core.
Linux's scheduler maps runnable software threads to available CPUs, preempts
them, and may migrate them. More runnable threads than CPUs means time slicing,
not more execution capacity. Never depend on a particular interleaving: even the
same binary and input may schedule differently.

**Concurrency** means multiple activities have overlapping lifetimes;
**parallelism** means they literally execute simultaneously. One core can run
concurrent threads. Many cores can run parallel work, if the scheduler permits.
Server-style activities often have heterogeneous timelines:

```
input:  [wait][parse]------[wait]
audio:  --[mix]--[mix]--[mix]---
```

A data-parallel lane group instead runs the same stages in roughly lockstep:

```
lane 0: [rows][ barrier ][reduce/output]
lane 1: [rows][ barrier ]
lane 2: [rows][ barrier ]
```

Both shapes are useful. Do not force unrelated, latency-oriented tasks into
lockstep lanes.

### 1. Creation, joining, and lifetime

Open `lessons/01_create_join.c`. `pthread_create(&thread, NULL, square, &job)`
starts an entry function receiving `void *`; no integer-to-pointer trick is
used. Jobs are an array in `main`, so they remain alive until all joins.
`pthread_join` waits for completion and synchronizes memory: after it returns,
main may inspect that worker's output. A detached or unjoined thread must never
retain pointers to expired stack data. Return values are pointers too; do not
return a pointer to a worker's dead local variable.

**Try it:** print before and after each square. Lines may reorder. POSIX stdio
functions lock their stream sufficiently to avoid corrupting stdio's internal
state, but several calls can still interleave semantically; `printf` does not
make unrelated shared data safe. **Done when:** you can add eight jobs without
passing `&i` from the creation loop and still get the formula result.

### 2. Data races are a language error

Read `lessons/02_race_mutex.c`, then run its safe default. In C, a **data race**
exists when threads access the same memory without ordering, at least one access
is a write, and the accesses are not atomic. That is undefined behavior (UB),
not merely “occasionally loses an increment.” The compiler is allowed to assume
it never happens. `counter++` is read/modify/write, not indivisible.

Only deliberately run `./build/02_race_mutex --unsafe`; its number proves
nothing, whether right or wrong. `volatile` would only constrain certain
compiler accesses; it provides neither atomicity nor inter-thread ordering.
**Try it:** rebuild with `-O0` and `-O2`, varying counts. Do not infer a guarantee
from frequency. **Done when:** you can state which two accesses conflict.

### 3. Mutexes protect invariants

`lessons/03_invariant.c` protects the relationship
`balance == deposits - withdrawals` and `balance >= 0`. The check and withdrawal
must be one critical section. A mutex has ownership: lock, briefly inspect/update
all state in the invariant, unlock. It also supplies synchronization so prior
writes become visible to the next successful locker.

Keep lock ordering globally consistent. If one path locks A then B while another
locks B then A, both can wait forever (**deadlock**). **Livelock** means threads
keep reacting but make no progress. **Starvation** means one runnable participant
repeatedly loses access; ordinary pthread mutexes do not promise broad fairness.
Avoid calling slow/unknown code while holding a lock.

Next, inspect the executable two-account extension in
`lessons/04_transfers.c`. It uses stable IDs rather than comparing unrelated
object pointers. **Done when:** total money is unchanged under `make stress` and
no opposite lock order exists.

### 4. Multiple locks need one order

`lessons/04_transfers.c` sends money in opposite directions, so either worker
may need both account mutexes. Both paths lock the lower stable account ID first
and unlock in reverse order. Transfer direction does not determine lock order.
The protected invariant is that neither balance is negative and total money
remains 2,000. **Done when:** you can explain why opposite transfers cannot form
a lock cycle.

### 5. Sleeping on predicates and shutting down

`lessons/05_bounded_queue.c` uses `not_empty` conceptually as
`count > 0 || closed`, and `not_full` as `count < capacity`. A condition variable
is not an event mailbox. `pthread_cond_wait` atomically releases the mutex and
sleeps, then reacquires it. It may wake spuriously, or another consumer may take
the item first, so always write:

```c
pthread_mutex_lock(&mutex);
while (!predicate) pthread_cond_wait(&condition, &mutex);
/* predicate is true while mutex is held */
```

Shutdown is data, not thread cancellation: main, the designated closer, sets
`closed` under the mutex and broadcasts; consumers drain queued work and exit
when closed+empty.
Join before destroying queue storage, mutexes, or conditions. Cancellation can
occur at cancellation points and demands cleanup handlers for held locks and
owned allocations; this course prefers explicit cooperative shutdown.

Its two producers own disjoint numeric ranges. Main—and only main—owns closure:
it joins both producers, sets `closed`, and broadcasts. **Try it:** set
`QUEUE_CAPACITY` to 1. **Done when:** every produced value is consumed exactly
once and all joins finish.

### 6. Atomics: small explicit agreements

In `lessons/06_atomic_claim.c`, atomic `fetch_add` gives unique work indices.
`memory_order_relaxed` guarantees atomic modification and a single order for
that counter, but does not publish unrelated memory. That is sufficient because
the ticket only claims disjoint result slots and joins publish completion.

A practical ordering ladder:

* relaxed: atomicity only; counters/tickets where other data needs no ordering;
* release store publishes preceding ordinary writes; an acquire load that reads
  that value makes those writes visible (a **synchronizes-with** edge);
* acquire/release read-modify-write can do both; sequential consistency adds one
  global order among seq-cst operations and is easiest to reason about, not a
  magic repair for ordinary races.

Atomics do not automatically make a compound invariant atomic. Start with a
mutex unless the protocol can be written down precisely. **Try it:** claim
chunks of 16 indices per fetch. Outcome: recognize synchronization overhead and
overshoot handling. **Done when:** every value is validated and you can prove
ticket uniqueness from the atomic read-modify-write's modification order.

### 7. Release/acquire publishes ordinary data

`lessons/07_atomic_publish.c` writes an ordinary `Payload`, then release-stores
an atomic `ready` flag. The consumer's acquire load observes that store, so the
payload writes happen-before its ordinary reads. `sched_yield` only makes the
spin loop less antisocial; it provides no ordering. **Done when:** you can draw
the exact sequenced-before → synchronizes-with → happens-before chain.

### 8. Caches and false sharing

Cores cache memory in fixed-size **cache lines**. A coherence protocol gives a
core ownership before writes and invalidates peer copies. Separate variables on
one line can therefore ping-pong even without a C data race: **false sharing**.
Run `lessons/08_false_sharing.c`; its 64-byte padding is illustrative, not a
portable cache-line query or guaranteed alignment. `volatile` is used only to
keep benchmark stores observable; each counter has one writer, and it does not
make synchronization. Timing direction and magnitude vary with CPU, affinity,
compiler, power state, and noise. Use repeated medians before conclusions.

**Exercise:** include `<stdalign.h>`, use `alignas(64)` (or C11 `_Alignas(64)`),
and inspect `sizeof` and addresses. Alignment controls where an object begins;
structure size controls array stride. Neither proves the hardware line size.

### 9. Partition, schedule, reduce

`lessons/09_reduction.c` uses static contiguous ranges over an immutable array:
`lo=N*lane/L`, `hi=N*(lane+1)/L`. Their half-open intervals cover `[0,N)` once,
even when `N % L != 0`. Each lane accumulates locally; main reduces after join,
avoiding a lock and false sharing in the hot loop.

Static scheduling has almost no coordination and good locality, but one costly
range delays everyone. Dynamic scheduling claims small units from a shared
counter: better load balance, more atomic traffic, potentially worse locality.
Chunk size trades those costs. `showcase.c` dynamically claims rows because
Mandelbrot row costs differ.

### 10. Reusable barriers separate phases

A barrier means no participant enters the next phase until all arrive.
`lessons/10_barrier.c` implements one with mutex+condition. Its generation
distinguishes round *g* waiters from fast round *g+1* arrivals; its wait loop
handles spurious wakeups. Every lane must reach barriers in the same order.

### 11. A persistent lane program

`lessons/11_lane_program.c` keeps one fixed group alive for three rounds. Lane 0
prepares input narrowly, all lanes process balanced ranges widely, lane 0 reduces
narrowly, and a final barrier protects reuse. Run it with `--serial`,
`--lanes 4`, and `--lanes 4 --trace`. This is the bridge from isolated primitives
to the article's program-shaped lane context.

### Performance without self-deception

Measure wall time for user-visible speedup, warm up, use release flags, repeat,
report median and spread, keep input/output identical, and benchmark enough work
to dwarf creation and timing overhead. Avoid concurrent background load. Process
CPU time can rise with parallelism even when wall time falls. The showcase uses
`clock_gettime(CLOCK_MONOTONIC)` and reports end-to-end wall time directly;
`/usr/bin/time` is still useful as an independent measurement.

With parallel fraction `P` and `L` lanes, Amdahl's ideal bound is

```
speedup <= 1 / ((1-P) + P/L)
```

Thread startup, synchronization, imbalance, cache misses, and reduction lower
it. Memory-bandwidth-bound work may saturate long before all cores are useful;
extra threads can slow down. Default showcase lanes come from online CPUs and
are capped at 64; `--threads` is explicit and may oversubscribe. Compare:

```sh
/usr/bin/time -f '%e wall' ./build/showcase --serial --output /tmp/s.ppm
/usr/bin/time -f '%e wall' ./build/showcase --threads 4 --output /tmp/t.ppm
cmp /tmp/s.ppm /tmp/t.ppm
```

### Final architecture: multi-core by default, explicitly

Ryan Fleury's *Multi-Core By Default* motivates a fixed lane group whose lanes
run one program-like entry point with explicit lane index/count, synchronized
phases, lane-local state, and narrow serial duties. `showcase.c` reconstructs
that computation shape directly: lane 0 participates in the same work loop;
other pthreads run the exact same `lane_main`; an atomic row counter balances
work; all meet a custom reusable barrier; per-lane iteration totals are reduced;
only lane 0 writes the PPM; a second barrier closes that narrow phase before any
lane leaves the group. Create once, join all, then destroy shared state. The
joins—not the second barrier—ultimately protect teardown lifetime. Serial is the
same machinery with one lane and produces a byte-identical PPM and matching
pixel checksum.

This is one valuable shape, not universal dogma. Independent I/O pipelines,
latency-sensitive services, actors, and task graphs have heterogeneous timelines
that a lockstep lane program may fit poorly.

```sh
./build/showcase --threads 4 --width 1280 --height 720 \
  --iterations 500 --output picture.ppm
./build/showcase --serial --width 320 --height 180 --output serial.ppm
```

### Quick reconstruction roadmap

1. **Create:** retype `01_create_join.c`. Done when every argument/result stays
   alive through join and sum is 30.
2. **Protect:** recreate `02`, `03`, and ordered transfers in `04`. Done when the
   unsafe race is opt-in and both invariants pass.
3. **Wait:** build `05` from its predicates. Done when capacity 1 shuts down.
4. **Claim:** recreate relaxed ticketing in `06`, then trace and chunk it.
5. **Publish:** recreate `07` and draw its release/acquire happens-before chain.
6. **Observe hardware:** run `08` repeatedly. Done when you can explain why its
   addresses and timing distribution are evidence, not a promise.
7. **Partition/reduce:** recreate `09`. Done when tiny and empty ranges work.
8. **Phase:** recreate `10`, then raise `ROUND_COUNT` to 1,000 for a stress run.
9. **Stay wide:** recreate the three-round lane program in `11` with 1 and N lanes.
10. **Render:** first static rows, then dynamic rows in `showcase.c`. Done
   when `make check` confirms its tested serial/threaded PPM files are identical.

### Project preview (in increasing ambition)

1. Parallel histogram: lane-local 256-bin arrays, reduction, deterministic test.
2. PPM blur: tiled static partition, separate input/output buffers, edge tests.
3. Directory word counter: bounded queue, producer lifecycle, per-worker maps or
   sorted vectors, deterministic reduction; keep filesystem errors explicit.
4. Fixed-lane software renderer: update/tile/render phases, reusable barrier,
   dynamic tiles, frame checksum, graceful stop.
5. Stretch: dependency-aware build executor with cycle detection, bounded jobs,
   cancellation protocol, and stress tests—use locks first, measure later.

### Debugging preview

Build `make debug`, then `gdb ./build/05_bounded_queue`; useful commands are
`run`, `info threads`, `thread apply all bt`, and `thread N`. For deadlock, inspect
all backtraces and lock order. Logging changes timing and does not prove safety.
`make tsan` requests compiler ThreadSanitizer; then run safe programs and the
explicit `--unsafe` demonstration. TSan is powerful but not guaranteed available,
may not support the host/runtime, has overhead, can miss unexecuted paths, and a
clean run is not proof. A real report in safe code is a failure, not something
the target suppresses.

Common errors: passing `&loop_index`; forgetting joins; reading output before
join; using `if` around `cond_wait`; changing a predicate without its mutex;
forgetting broadcast on shutdown; holding locks across I/O; inconsistent lock
order; assuming `volatile` is atomic; publishing data with relaxed atomics;
multiple lanes writing one partial; unmatched barrier control flow; benchmarking
debug builds or tiny inputs; and assuming more threads means faster.

### Vocabulary preview and next reading

**atomic:** indivisible object operation with specified ordering. **critical
section:** code executing under mutual exclusion. **happens-before:** C relation
that makes earlier effects visible and excludes a race. **invariant:** statement
that must hold at synchronization boundaries. **predicate:** state condition a
waiter tests. **reduction:** combine lane-local values. **contention:** parties
compete for a resource. **coherence:** per-location cache consistency machinery.
**oversubscription:** more runnable threads than execution contexts.

Next: Ryan Fleury, *Multi-Core By Default* (architecture); POSIX `pthread_create`,
`pthread_mutex_lock`, and `pthread_cond_wait` manual pages (exact contracts);
cppreference C atomics and memory order (language model); *The Art of
Multiprocessor Programming* (algorithms); Brendan Gregg's systems performance
materials (measurement); and ThreadSanitizer's official Clang/GCC documentation.
Read implementation manuals for your actual libc/compiler: portability ends at
the documented POSIX and C11 boundaries, not at what happened once on Linux.

## Part II: source-driven workbook

### Course map and study workflow

Do not read this as a list of APIs to memorize. Use this loop for every lesson:

1. Read the opening comment, structures, worker function, and then `main`.
2. Write down what is shared, what is private, and what keeps each object alive.
3. Predict the output and identify every synchronization edge.
4. Build and run the unchanged program. Record observed output beside predicted
   output; disagreement is useful information.
5. Make one small experiment, rebuild, and restore it before moving onward.
6. Run `make check` after restoration. Keep intentional races out of commits.

| Stage | Exact file | Central question | Completion evidence |
|---|---|---|---|
| 0 | `Makefile` | How is pthread support enabled? | Explain `-pthread` |
| 1 | `lessons/01_create_join.c` | Who owns worker arguments? | Sum is 30 |
| 2 | `lessons/02_race_mutex.c` | What makes an increment safe? | Safe mode passes |
| 3 | `lessons/03_invariant.c` | What statement does the lock protect? | Invariant says yes |
| 4 | `lessons/04_transfers.c` | How do two locks avoid a cycle? | Total remains fixed |
| 5 | `lessons/05_bounded_queue.c` | How do sleepers and shutdown cooperate? | Every item appears once |
| 6 | `lessons/06_atomic_claim.c` | When is relaxed atomic ordering enough? | Every slot filled |
| 7 | `lessons/07_atomic_publish.c` | How are ordinary writes published? | Payload validates |
| 8 | `lessons/08_false_sharing.c` | Can correct code still scale badly? | Explain measurements |
| 9 | `lessons/09_reduction.c` | How is work partitioned and combined? | Formula agrees |
| 10 | `lessons/10_barrier.c` | How are reusable phases separated? | All rounds finish |
| 11 | `lessons/11_lane_program.c` | How does a fixed group stay wide? | Three rounds pass |
| capstone | `showcase.c` | How do the ideas form one lane program? | Tested PPM bytes match |

## Before threads: establish the serial truth

Parallelizing an unclear serial algorithm multiplies uncertainty. Begin with a
single loop, a known input, and an independently checkable result. State:

* the input domain and output;
* which loop iterations are independent;
* mutable state and its invariant;
* the serial reference result;
* ownership and lifetime of every buffer.

For the renderer, one serial loop computes every pixel and one output operation
writes the finished buffer. `--serial` is not a separate algorithm: it runs the
same lane machinery with one participant. This makes `cmp` a powerful oracle.

### Process and thread lifecycle

```
process main thread:  [allocate]--create----create--work/join--[free]--exit
                                      |         |
worker 1:                             +-[start--work--return]
worker 2:                                       +-[start--work--return]

pthread_create: overlapping lifetime begins
pthread_join:   worker has returned; resources/result may be reclaimed/read
```

`pthread_create` does not wait until the worker has started. `pthread_join` is
both a wait and a memory synchronization point. Join every joinable thread once.

### Shared address space, private execution state

```
one process address space
+--------------------------------------------------------------+
| program text | globals | heap: shared pixels/jobs/queue       |
+--------------------------------------------------------------+
       thread 0                 thread 1              thread 2
       registers               registers             registers
       private stack           private stack         private stack
```

“Private stack” means each thread has a distinct stack, not that pointers into
it are inaccessible. A worker may dereference a pointer into `main`'s stack
while that object is alive. This is exactly what lesson 01 deliberately does.

```
main stack:  jobs[0] jobs[1] jobs[2] jobs[3]   (stable until main returns)
                ^       ^       ^       ^
worker args:    |       |       |       |

BAD: loop variable i at one address <--- every worker receives &i
     i changes during creation and expires when its block ends
```

### Typical Linux machinery—not the POSIX contract

POSIX specifies behavior, not one implementation. On typical current Linux
systems, `pthread_create` creates an OS-schedulable thread with kernel state.
Uncontended mutex operations commonly update atomic user-space state without a
system call. Under contention, mutex and condition-variable implementations can
park and wake threads through futex-like kernel facilities. A parked thread is
not runnable; a spinning thread remains runnable and consumes an execution
context. When woken, the scheduler may resume it on a different CPU.

This explains two real costs without making them API promises: synchronization
can generate cache-line traffic even on a user-space fast path, and blocking or
thread creation can cross into the kernel and scheduler. Optional Linux tools
such as `strace -f -c ./build/05_bounded_queue` can show system calls, but their
exact names and counts depend on libc, kernel, contention, and timing.

## Concrete interleavings and the C rule

Imagine `counter` starts at 7. At machine-like pseudocode level, two increments
can overlap:

| Time | Thread A | Thread B | Memory |
|---|---|---|---|
| 1 | load 7 | | 7 |
| 2 | | load 7 | 7 |
| 3 | compute 8 | | 7 |
| 4 | | compute 8 | 7 |
| 5 | store 8 | | 8 |
| 6 | | store 8 | 8 |

Two requested increments produced one: a **lost update**. This table is useful
intuition, but C says something stronger. Conflicting non-atomic accesses with
no happens-before relation form a data race, and a data race gives the whole
program undefined behavior. The compiler need not preserve the imagined loads
and stores. It may cache values, eliminate work, or transform loops under the
valid-program assumption that races do not exist. “It only loses updates on my
machine” is therefore not a portable model. Neither `volatile` nor a sleep fixes
it. Use a mutex or an appropriate atomic protocol.

### Exercise 1: lifecycle prediction

Add one `printf` before each create, at worker entry, and after each join in
lesson 01. **Predict:** which order is guaranteed? **Observe:** run ten times.
Only each thread's own sequenced output and each worker-before-its-join return
are meaningful guarantees. Restore the file deliberately using the workflow in
the debugging section, rebuild, then run `make check`.

### Exercise 2: intentional race, safely contained

Run the already-provided `--unsafe` mode; do not introduce a new race. Predict
whether the answer must be low, then observe several runs and optimization
levels. It may even equal the expected value. Restore the normal build with
`make clean && make`; never use unsafe output as a test oracle.

## Mutex design: state first, lock second

A disciplined lock workflow is:

1. Write the invariant in plain language or algebra.
2. List every field participating in it.
3. Assign one mutex to that state.
4. Require the mutex whenever code reads state to make a decision or changes it.
5. Check the invariant while locked at critical-section boundaries.
6. Unlock before slow I/O or unknown callbacks when possible.

For `Bank`, the protected state is not merely `balance`; it is the relationship
among `balance`, `deposits`, and `withdrawals`. Splitting “is balance positive?”
from decrement permits another thread to invalidate the decision.

**Lock granularity** is a design tradeoff. One coarse lock is easy to audit and
preserves broad invariants, but unrelated operations contend. Fine locks permit
more parallel work, but add ordering rules, overhead, and intermediate states.
Start coarse. Split only after measurement identifies contention and after the
new invariants and lock order can be stated precisely.

Lesson 04 makes lock order executable. Two transfers need the same pair of
mutexes in opposite semantic directions, but both acquire lower account ID then
higher account ID. The resulting wait graph cannot contain an A→B/B→A cycle.
IDs are stable ordering keys; comparing unrelated object pointers with `<` is
not a portable substitute. Unlocking in reverse order keeps the nested lifetime
visually clear.

### Exercise 3: measure critical-section size

In lesson 02, temporarily acquire the mutex once around the entire worker loop.
Predict correctness and contention versus locking each increment. Time both,
but do not confuse faster with better general design. Restore the per-increment
version and verify `make check`.

### Exercise 4: invariant audit

Make the compound decision stale without creating a raw data race: lock, copy
`balance > 0` into a local `may_withdraw`, unlock, then later lock again and
decrement if `may_withdraw` without rechecking. Every access is synchronized,
yet two withdrawers can make the same stale decision. Draw that interleaving.
This demonstrates that race-free individual accesses do not make a compound
operation atomic. Restore the original one-lock check-and-update afterward.

## Condition variables: waiting without missing state

A condition variable has no stored signal count. A signal with no waiter simply
disappears. Correctness comes from protected state—the **predicate**—not from
remembering notifications.

```
consumer                          producer
lock mutex
count == 0
cond_wait atomically:
  enqueue waiter + unlock  -----> lock same mutex
                                  put item; count = 1
                                  signal; unlock
wake, reacquire mutex <----------
re-check count != 0
remove item; unlock
```

The atomic release-and-wait closes the dangerous gap between checking and
sleeping. If the producer changes the predicate before the consumer locks, the
consumer observes true and does not sleep. If afterward, producer cannot change
it until `cond_wait` has atomically released the lock and become a waiter.
Always modify the predicate under the same mutex, always wait in `while`, and
regard waking only as a request to check again.

`pthread_cond_signal` wakes at least one waiter and suits “one new item permits
one consumer.” `pthread_cond_broadcast` wakes all and suits a global state
transition such as `closed = 1`, where every consumer may now be able to exit.
Broadcast may cause a thundering herd; signal may strand waiters if several can
proceed. Choose from the predicate transition, not habit.

### Exercise 5: capacity one

Set `QUEUE_CAPACITY` to 1 in lesson 05. Predict which predicate alternates after
every operation. Observe with `make stress`, restore it to 8, and rerun checks.

### Exercise 6: signal versus broadcast

Temporarily change the shutdown broadcast to signal. Predict what happens with
three consumers when all are already waiting on an empty queue: one signal can
wake only one, leaving the others stranded. This source does not force that exact
starting state, so the edited program may also finish; either outcome is an
observation, not proof. On Linux with coreutils, bound the experiment with
`timeout 5s ./build/05_bounded_queue`. A timeout only reports nontermination; it
does not prove the diagnosis. Restore `pthread_cond_broadcast` immediately.

The source already has two producers with disjoint ranges. Main joins both
before closing the queue; `queue_push` itself does not enforce “no pushes after
close.” That is an external protocol invariant. As an extension, add a third
producer, recompute three disjoint ranges, and keep main as the sole closer.

## Happens-before: an engineering checklist

Happens-before answers: “What guarantees this write is visible before that
read?” Useful edges here are:

* actions before `pthread_create` are available to the new thread at entry;
* a mutex unlock synchronizes with a later successful lock of that mutex;
* predicate writes before unlock become visible after `pthread_cond_wait`
  reacquires that mutex; the signal alone is not the data-transfer mechanism;
* a release atomic operation can publish earlier writes to an acquire operation
  that reads from its release sequence;
* all actions in a worker happen before successful `pthread_join` returns.

Program order chains these edges. If no chain orders conflicting accesses, use
synchronization or redesign ownership. Do not substitute “the CPU probably
flushes caches”; language guarantees, not folklore, make the program valid.

## Atomics beyond counters

`atomic_fetch_add` is a read-modify-write (RMW): no other atomic modification of
that object can slip between its read and write. Thus every ticket in lesson 06
is unique. Relaxed ordering is correct because ticket values allocate disjoint
ordinary `values[index]` elements, and join publishes completion. The runtime
check verifies every final element; it cannot by itself prove that a slot was
written exactly once. That uniqueness follows from the atomic RMW reasoning.
The ticket is not being used to publish those elements.

Lesson 07 implements the following publication pattern with a multi-field
`Payload`. This smaller snippet distills its ordering contract:

```c
/* producer */
message = 42;
atomic_store_explicit(&ready, true, memory_order_release);

/* consumer */
if (atomic_load_explicit(&ready, memory_order_acquire)) {
    printf("%d\n", message); /* sees 42 */
}
```

Release publishes earlier writes; an acquire that observes it imports them.
With relaxed `ready` operations, the non-atomic message access lacks that edge
and the program has a data race. Do not try to prove this by waiting for a bad
value; reason from the missing edge and use TSan only as supporting evidence.

Compare-exchange conditionally replaces an expected value. On failure it also
updates the caller's `expected` with the observed value, so retry loops must
recompute from that value and handle spurious failure for `compare_exchange_weak`.
Correct lock-free algorithms also face object lifetime and ABA/reclamation
problems. “Lock-free” means system-wide progress, not wait-free per-thread
progress, fairness, simplicity, or speed. Check `atomic_is_lock_free` if the
property matters. Prefer locks until profiling and expertise justify more.

### Exercise 7: chunked ticket claims

Run lesson 06 with `--trace`; predict whether claim counts must be equal, then
observe several runs. Next, change it so each RMW claims 16 consecutive indices.
Clamp the final chunk to `VALUE_COUNT`. Predict the number of RMW operations and
overshoot.
Observe the same deterministic sum, then restore one-at-a-time claims.

### Exercise 8: extend the published payload

Add a small character array to `Payload`, initialize every byte before the
release store, and validate every byte after the acquire load. Before running,
write the three-link happens-before chain for one byte. Keep release/acquire in
place—do not create a relaxed-data-race experiment. Done when the expanded
payload passes normally and under TSan; then restore and run `make check`.

## Caches, locality, coherence, and the language model

Typical lookup cost grows from registers through L1/L2/L3 caches to DRAM.
Contiguous traversal exploits spatial locality; reuse soon exploits temporal
locality. Cache coherence keeps cached copies coherent and coordinates ownership
and write visibility for a location. It does **not** rescue a C data race:
hardware behavior and the C abstract machine are different layers.

False sharing occurs when independent, correctly owned variables occupy one
coherence line and writers repeatedly invalidate each other. Padding can help,
but 64 bytes is only a common line size; structure alignment, array stride,
allocator placement, compiler optimization, topology, and line size all matter.
Lesson 08 is deliberately an experiment. Its `volatile` prevents optimizing
away illustrative stores but is not synchronization. Measure addresses and
multiple trials; never promise a fixed ratio.

### Exercise 9: locality experiment

Lesson 09 now reads a real immutable array. Within each lane range, temporarily
use an outer loop over offsets `0..stride-1` and an inner loop beginning at
`begin + offset` and stepping by `stride`. This covers each index once even when
the range length is not divisible by the stride. Predict cache behavior, confirm
the sum stays equal, and measure only with enough work. Restore contiguous reads.

## Partitioning, scheduling, and reductions

For `N` items and `L` lanes, article-style quotient/remainder partitioning is:

```
q = N / L, r = N % L
length(i) = q + (i < r ? 1 : 0)
begin(i)  = i*q + min(i, r)
end(i)    = begin(i) + length(i)
```

The first `r` lanes receive one extra item. Lesson 09 uses another balanced
endpoint formula:

```
begin = N * lane / L
end   = N * (lane + 1) / L
```

These formulas both cover every item with lengths differing by at most one, but
they do not assign the same ranges. For `N=10,L=4`, quotient/remainder gives
lengths `3,3,2,2`; endpoints give `2,3,2,3`. With sufficiently wide arithmetic,
adjacent endpoints match. In general-purpose code, avoid overflow in `N * lane`;
use a suitable unsigned width or the quotient/remainder form. Lesson 11 uses the
article-style formula so you can compare both directly.

A reduction gives each lane private accumulation state and combines partials
after synchronization. This removes hot-loop lock contention. Integer addition
in lesson 09 is exact in range. Floating-point addition is not associative, so
different partition/reduction trees can change low bits. Determinism may require
a fixed tree, compensated summation, wider accumulators, or documented tolerance.

Static scheduling has no per-item coordination and preserves locality:

```
lane 0: [cheap cheap expensive expensive----------------]
lane 1: [cheap cheap]                    [idle----------]
```

Dynamic tickets improve balance:

```
lane 0: [chunk][chunk-------][chunk]
lane 1: [chunk------][chunk][chunk---]
counter: claim claim claim claim claim claim
```

Tiny chunks balance well but increase atomic traffic and may harm locality.
Large chunks amortize claims but recreate tail imbalance. Guided schedules start
large and shrink. Measure chunk size against realistic, uneven work.

### Exercise 10: awkward partition sizes

Run lesson 09 with `--trace`. Then set `ITEM_COUNT=11` and list all four ranges
before running. Next set `ITEM_COUNT=3, THREAD_COUNT=8`; empty ranges are valid
and every item must still appear once. Restore the defaults and run `make check`.

## Reusable barriers and two-phase reuse

A barrier tracks `arrived`, `participant_count`, and `generation` under a mutex.
Each arrival snapshots generation. The last arrival resets the count, advances
generation, and broadcasts. Others loop until generation differs. Resetting
before broadcast allows reuse; generation prevents a fast next-round arrival
from being mistaken for a late current-round participant.

Lesson 10 uses two barriers per round. The first publishes each lane's phase so
all lanes may read all entries. The second ensures every reader has finished
before a fast lane overwrites its entry for the next round. One barrier separates
write from read; the other separates read from reuse.

### Exercise 11: reason about the second barrier

Do not begin by deleting it. Draw a timeline where lane 0 starts round `r+1`
while lane 3 still checks round `r`. Then, only as an intentional experiment,
remove it. This intentionally creates a C data race/UB between a next-round
write and a prior-round read. No observed output—not even PASS—proves anything;
TSan is only supporting evidence. Run under a deadline such as
`timeout 5s ./build/10_barrier`, restore immediately, and run the full checks.

Run `./build/10_barrier --trace` to see arrivals, generations, and which lane
arrives last. Logging occurs under the barrier mutex to keep lines readable, so it
perturbs scheduling and must never be used as performance evidence.

## Persistent lanes before the capstone

Read `lessons/11_lane_program.c` before Mandelbrot. Its OS threads are created
once, then remain inside one `lane_main` call for three complete rounds:

```text
lane 0 prepare -> barrier -> all lanes sum ranges -> barrier
              -> lane 0 reduce -> reuse barrier -> next round
```

Lane 0 changes `active_count` and fills the corresponding values each round. The
first barrier broadcasts that count and prepared ordinary data through
mutex/condition synchronization. The second publishes lane partials. The third
prevents a fast lane from overwriting a partial while lane 0 still reduces the
current round.
Unlike a job callback, each lane retains one normal call stack and local context
across the whole computation. Run `--serial` to see that one lane is simply
another parameterization of the same program; run `--lanes 4 --trace` to inspect
the article-style quotient/remainder ranges.

This lesson passes `Lane *` explicitly because visible data flow is best while
learning. Fleury's `LaneIdx`, `LaneCount`, and `LaneSync` place equivalent group
context behind thread-local accessors. TLS can reduce parameter plumbing later,
but it is not fundamental machinery. The article also broadcasts pointers or
counts from lane 0 through shared group storage plus barriers; this lesson's
`active_count` and `values` are the concrete same-group version. Supporting one
OS thread in different temporary groups or concurrently executing groups
requires changing the selected lane context and avoiding function-static group
data—advanced extensions, not assumptions hidden here.

### Exercise 12: inspect a persistent group

Predict the ranges for 37 values and 6 lanes, then run
`./build/11_lane_program --lanes 6 --trace`. Change `VALUE_COUNT` to 3 with 8
lanes and identify empty lanes.
Finally add a fourth round without changing thread creation. Done when serial
and multi-lane totals agree in every round; restore and run `make check`.

## Capstone walkthrough: `showcase.c`

Read the capstone by function rather than top to bottom:

1. `main` parses unchanged flags, sizes allocations, initializes the barrier,
   creates lanes 1..N-1, and runs lane 0 on the main thread.
2. `lane_main` repeatedly claims a row with relaxed `atomic_fetch_add`.
3. Each claimed row is rendered into disjoint pixel storage; each lane accumulates
   into a stack-local total and publishes it once before the barrier.
4. The first `barrier_wait` proves rendering and local totals are complete.
5. Lane 0 enters the narrow serial section: reduction, PPM output, and reporting.
6. The second barrier closes the narrow section, keeping the lane group in
   lockstep before workers return. Main then joins them; those joins make final
   teardown safe in this one-shot program.
7. Main destroys condition/mutex objects and frees storage only after joins.

Data flow is:

```
flags -> Shared configuration -> atomic row claims -> disjoint pixel rows
                                -> lane-local totals -> lane-0 reduction
pixels ------------------------------------------------> PPM + checksum
```

Control flow is:

```
bootstrap -> all lanes render -> barrier -> lane 0 narrow work -> barrier -> join
```

Fleury's article vocabulary translates directly: `LaneIdx` is `Lane.lane_index`,
`LaneCount` is the shared lane count, `LaneSync` is `barrier_wait`, and `go
narrow` is the `lane_index == 0` branch between barriers. This source uses pthreads
and a reusable condition-variable barrier rather than copying article syntax.

Reconstruct it incrementally: (1) write a serial pixel renderer and checksum;
(2) statically partition rows; (3) give every lane an explicit index/count;
(4) replace static rows with atomic tickets; (5) add lane-local totals and a
join-time reduction; (6) add the first barrier and move reduction/output into
lane 0; (7) add the second barrier to delimit narrow work; (8) add robust parsing,
allocation checks, explicit init/destroy, and serial/threaded PPM comparison.

### Exercise 13: static versus dynamic rows

Replace dynamic claims temporarily with endpoint-partitioned rows. Predict which
Mandelbrot regions cause imbalance. Compare output bytes (they must match) and
wall times. Restore atomic row claims and run `make check`.

### Exercise 14: deterministic capstone check

Render the same dimensions with 1, 2, 3, and 8 lanes. Predict checksum equality.
Use distinct paths and `cmp`, not visual inspection:

```sh
./build/showcase --threads 1 --output /tmp/m1.ppm
./build/showcase --threads 2 --output /tmp/m2.ppm
./build/showcase --threads 3 --output /tmp/m3.ppm
./build/showcase --threads 8 --output /tmp/m8.ppm
cmp /tmp/m1.ppm /tmp/m2.ppm
cmp /tmp/m1.ppm /tmp/m3.ppm
cmp /tmp/m1.ppm /tmp/m8.ppm
rm -f /tmp/m1.ppm /tmp/m2.ppm /tmp/m3.ppm /tmp/m8.ppm
```

Any difference is a correctness bug. Run once with `--trace-lanes`; row and
iteration counts reveal dynamic distribution but are not scheduling guarantees.

## Benchmarking and scalability

Use wall-clock time for elapsed performance. Warm caches and dynamic libraries,
run multiple trials, randomize configuration order, report median plus spread,
and retain raw data. Keep input, compiler flags, output, machine load, and CPU
frequency policy controlled. Confirm results before timing; fast wrong code wins
no benchmark. Separate setup, useful work, and teardown where appropriate.

The showcase labels its one sample **end-to-end**: it includes worker creation,
rendering, PPM output, barriers, and joins. That is a valid user-visible number,
not an isolated render-kernel measurement. Collect repeated samples rather than
quoting one run:

```sh
for lanes in 1 2 4 8; do
  for trial in 1 2 3 4 5; do
    ./build/showcase --threads "$lanes" --width 1280 --height 720 \
      --iterations 500 --output /tmp/bench.ppm | tail -1
  done
done
rm -f /tmp/bench.ppm
```

Randomize the lane order for serious measurement and compute medians/spread from
the recorded numbers. Do not enable `--trace-lanes` while timing.

For `P = 0.90` parallel work and four lanes, Amdahl gives:

```
1 / ((1 - .90) + .90 / 4) = 1 / .325 = 3.08x maximum
```

Even infinitely many lanes cap speedup at 10x because 10% is serial. Real thread
creation, atomic claims, barriers, output, cache misses, and imbalance reduce it.
For small images overhead dominates. For large images memory bandwidth or shared
cache can saturate. More threads may then make performance worse.

An online CPU count usually reports logical CPUs. Two SMT siblings share much of
a physical core and do not equal two full cores. Containers and affinity can
further constrain usable CPUs. Compare 1, powers of two, physical-core count,
and logical-CPU count. Oversubscription—more runnable compute threads than
hardware contexts—adds preemption and cache disruption, though it can help when
threads spend time blocked on I/O. The renderer is compute-heavy.

## Safety, liveness, ownership, and shutdown

**Safety** means nothing bad happens: no race, invalid access, duplicate item,
broken invariant, or premature free. **Liveness** means something good eventually
happens: work completes, waiters wake, and shutdown terminates. A mutex can give
safe state access while deadlocking; a polling loop can remain safe but starve.

Audit every threaded component with this checklist:

* Who allocates each object, and who frees it?
* Which threads may access it, and until what event?
* Is state immutable, thread-owned, mutex-protected, or atomic?
* What invariant/predicate accompanies each mutex/condition?
* Are lock orders acyclic and documented?
* Can every blocking wait eventually become true or observe shutdown?
* Does shutdown stop production, publish `closed`, broadcast, drain, and join?
* Are all joinable threads joined exactly once?
* Are mutexes/conditions destroyed only after their final possible use?
* On errors, who joins already-created threads and releases partial resources?

This teaching code exits the process on any reported pthread error. That policy
also avoids a partial-start deadlock: already-created lanes cannot be joined
normally if the fixed barrier still expects a lane whose `pthread_create` failed.
A recoverable implementation needs more than a return-and-join loop; use a launch
gate, an abortable barrier, or finalize the participant count before releasing
workers. Production libraries also need structured ownership and error unwinding
instead of calling `exit` from a worker.

## Debugging playbook

1. Reproduce with the smallest input while retaining concurrency.
2. Write expected invariants and synchronization edges before adding logs.
3. Build `make debug`; in GDB use `run`, `info threads`, `thread apply all bt`,
   `thread N`, `frame`, and `print`. Deadlock backtraces often expose lock order.
4. Build `make tsan`, then run the relevant path. Read both reported stacks and
   find the missing edge. TSan availability is platform/toolchain dependent.
5. Add deterministic checks: counts, sums, invariant assertions, checksums, and
   serial-vs-parallel `cmp`.
6. Stress scheduling with many repetitions, lane counts, capacities, and awkward
   input sizes. A failure proves a bug; repeated success does not prove absence.
7. Reduce a failure without removing the synchronization pattern that triggers it.

Before an experiment, make a small commit or branch if you want to preserve it.
When you intentionally want to discard a completed experiment in a normal cloned
repository, inspect first and restore only that lesson:

```sh
git diff -- lessons/05_bounded_queue.c
git restore lessons/05_bounded_queue.c
make clean && make check
```

`make clean` removes generated binaries and images; it does not restore edited
source. Never use `git restore` when the experiment contains work you want to keep.

For a safe sanitizer regression pass, run `make tsan-check`. Any report in safe
code is a failure. The lesson-02 race remains a separate manual demonstration;
when executed, TSan should report it, but one missed report would not make it
valid. Sanitizer runtimes are not available on every platform. Restore the normal
optimized build afterward with `make clean && make`.

Sleeps widen or narrow timing windows but establish no ordering unless the API
itself says so. Logging locks streams and perturbs scheduling, often hiding a
race or deadlock. Neither is a proof technique. Assertions under the protecting
lock and deterministic end-state checks are stronger. TSan covers only executed
paths and can have false limitations; combine tools with reasoning.

## Common mistakes: symptom to repair

| Symptom | Root cause | Fix |
|---|---|---|
| Workers print same ID | passed `&loop_index` | stable argument per worker |
| Correct output only sometimes | data race/lost update | mutex or atomic protocol |
| Consumer hangs at shutdown | signal not stored / waiter stranded | `closed` predicate + broadcast |
| Queue underflow after wake | `if` around cond wait | predicate `while` loop |
| Deadlock under load | inconsistent lock order | one global order |
| Crash after function returns | stack argument expired | extend lifetime and join |
| Main sees stale partial | read before join | join then reduce |
| Atomic flag sees stale payload | relaxed used for publication | release/acquire edge |
| Barrier hangs | lane skipped or reordered barrier | identical control-flow sequence |
| Next round corrupts checks | missing reuse barrier/generation | second phase + generation |
| Correct but slow counters | false sharing/contention | lane-local state, measured padding |
| More lanes are slower | overhead/bandwidth/oversubscription | benchmark and cap lanes |
| Different floating sums | reassociated reduction | fixed tree/tolerance/compensation |
| Debug prints “fix” bug | timing perturbation | sanitizer + invariant reasoning |

## Staged projects with milestones and tests

### Project 1: parallel histogram

Milestones: serial 256-bin reference; static byte ranges; one private histogram
per lane; joined deterministic reduction; malformed-input handling. Test empty,
one-symbol, all-byte-values, odd lengths, and lane counts above input length.
Pass when every parallel bin matches serial and total bins equal input length.

### Project 2: PPM box blur

Milestones: parser and serial blur; separate immutable input/mutable output;
static row partition; explicit border policy; optional dynamic tiles. Test 1x1,
thin images, known impulse image, and byte identity across lane counts. Run TSan.

### Project 3: bounded directory word counter

Milestones: one producer walks files; bounded queue owns copied path strings;
workers consume until closed; per-worker maps; deterministic sorted reduction;
error and shutdown propagation. Test empty trees, unreadable files, long paths,
queue capacity one, and injected producer failure; every allocation has one owner.
This project assumes directory traversal, owned C strings, tokenization, and a
dynamic map or sorted-vector implementation. If those are new, first process a
fixed in-memory array of strings through the same queue and reduction protocol.
Process regular files only and do not follow directory symlinks unless you
deliberately add `(device,inode)` cycle detection. On a traversal or read error,
publish failure, stop producing paths, close/drain the queue, join workers, print
no partial result as success, and return a nonzero status.

### Project 4: fixed-lane frame renderer

Milestones: lane-0 update; **update-publication barrier**; dynamic tile render;
**render-completion barrier**; lane-0 checksum/output; **frame-reuse barrier**;
cooperative stop predicate; join and teardown. Lane 0 alone may resize: after all
lanes finish the old frame and reach the named between-frame reuse boundary, it
allocates/swaps buffers, publishes the new dimensions, and frees old storage only
when no lane can still reference it. Test 1/2/odd lane counts, 1,000 frames, fixed
checksums, resize boundaries, and graceful stop.

### Project 5: dependency-aware build executor

Milestones: parse DAG; reject cycles; mutex-protected ready queue; condition wait
on ready-or-shutdown; bounded workers; dependent release; first-error shutdown;
deterministic event log for tests. Test diamond dependencies, disconnected DAGs,
cycles, worker failures, zero-ready deadlock detection, and stress under TSan.
Define first-error behavior before coding: after failure, start no new ready or
dependent task; let already-running child processes finish and reap them; abandon
queued work; wake all waiters; join workers; and return failure. Process-group
cancellation of already-running commands is an optional later extension.

## Expanded glossary

- **Acquire:** ordering that imports writes from a release it observes.
- **Atomic RMW:** indivisible read plus modification of one atomic object.
- **Barrier:** phase boundary reached by every participant.
- **Cache line:** hardware transfer/coherence unit, commonly but not always 64 B.
- **Condition variable:** wait queue used with a mutex-protected predicate.
- **Data race:** unordered conflicting accesses, at least one write, producing UB.
- **Deadlock:** participants wait in a cycle and no one can progress.
- **False sharing:** independent writes contend because they share a cache line.
- **Half-open range:** includes its beginning and excludes its end, `[begin,end)`.
- **Happens-before:** language relation establishing visibility and race freedom.
- **Lane:** indexed participant running the same phased entry program.
- **Livelock:** participants run but repeatedly prevent useful progress.
- **Mutex:** ownership-based mutual exclusion with synchronization.
- **Predicate:** state expression whose truth permits a waiter to proceed.
- **Reduction:** deterministic or specified combination of lane-local results.
- **Release:** ordering that publishes preceding effects to an observing acquire.
- **Starvation:** one participant is indefinitely denied progress.
- **UB:** undefined behavior; the C standard imposes no requirements.

## Curated reading order

1. Read [OSTEP's free *Concurrency* chapters](https://pages.cs.wisc.edu/~remzi/OSTEP/)
   for motivation, locks, conditions, and common bugs.
2. Read Ryan Fleury's
   [*Multi-Core By Default*](https://www.dgtlgrove.com/p/multi-core-by-default)
   before reconstructing the capstone; map its vocabulary using the walkthrough.
3. Keep the Linux man-pages for
   [`pthread_create`](https://man7.org/linux/man-pages/man3/pthread_create.3.html),
   [`pthread_join`](https://man7.org/linux/man-pages/man3/pthread_join.3.html),
   [`pthread_mutex_lock`](https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html),
   and [`pthread_cond_wait`](https://man7.org/linux/man-pages/man3/pthread_cond_wait.3p.html)
   beside the lessons. The `3p` pages document POSIX contracts.
4. Study cppreference's [C atomic operations](https://en.cppreference.com/w/c/atomic)
   and [memory order](https://en.cppreference.com/w/c/atomic/memory_order) only
   after mutex/condition reasoning is comfortable.
5. Use Clang's
   [ThreadSanitizer documentation](https://clang.llvm.org/docs/ThreadSanitizer.html)
   while running experiments, and read Google's
   [TSan C++ manual](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual)
   for report patterns applicable to C.
6. Continue with *The Art of Multiprocessor Programming* for advanced algorithms
   and Brendan Gregg's [Systems Performance](https://www.brendangregg.com/systems-performance-2nd-edition-book.html)
   for rigorous measurement. Always defer to your actual libc/compiler manuals.
