# Runtime scope and implementation plan

Updated November 22, 2026.

## Question we are investigating

Can a small runtime use operation-specific execution estimates, core
availability, dependencies, and coordination/memory costs to finish work sooner
than a tuned static schedule on the PSoC 6's unequal M4 and M0+ cores? When is
the extra scheduling work more expensive than the imbalance it corrects?

The deliverable is a C task-runtime library plus benchmark applications. An
application explicitly supplies operations, buffers, sizes, and dependencies.
The runtime assigns and dispatches those tasks. It does not automatically find
parallelism in arbitrary C programs, compile code for another core, or migrate
a running task. Both firmware images contain local implementations of supported
operation IDs. Jobs run to completion. An RTOS is not required.

Infineon supplies startup, clocks, timers, interrupt control, and IPC hardware
drivers. The FIR kernels, job descriptors, ownership protocol, scheduler, and
evaluation workloads are project code. CPU DSP instructions accelerate the M4
kernel; they are not a separate processor or a vendor-provided task scheduler.

## Completed foundation

1. **General FIR and a strong sequential reference.** Arbitrary signed
   coefficients within the documented exact-int32 bound, runtime tap counts,
   portable scalar and M4 DSP kernels, full-output checks, and repeated batch
   measurements. Across the tested 20 workloads, 100/150 MHz, and three compiler
   settings, M4 at 150 MHz with DSP and `-O2` was fastest. The original measured
   reference is commit `1017bce`; see [BENCHMARK.md](BENCHMARK.md).
2. **Single-job cross-core execution.** M4 publishes one shared-memory job;
   IPC wakes M0+, which executes it and releases the hardware channel after
   publishing completion. M4 verifies the result and reclaims the slot. The
   hardware test checks changing inputs, short/zero blocks, odd tap counts,
   exactly-once execution, invalid requests, busy submission, and stale IDs.
   See [DISPATCH.md](DISPATCH.md). This is a transport prototype, not yet a
   queue or a scheduling policy.
3. **Naive greedy parallel baseline.** A preloaded batch of up to 32 FIR jobs
   shares a bounded buffer pool. Each free core claims the next job under a
   short hardware lock, then runs outside the lock. M4 computes local jobs
   while M0+ runs; completion epochs protect reuse. The hardware comparison
   uses matched direct M4-only calls, tests both kernels on each core, checks
   full outputs and exactly-once claims, and saves separate interval traces
   proving actual overlap. See [GREEDY.md](GREEDY.md). No cost estimates are
   used for assignment; this is not the profiled static policy below.

The sequential benchmark remains selectable with `APP_MODE=sequential`.
The transport benchmark uses `APP_MODE=dispatch`; the concurrent ready-list
baseline uses `APP_MODE=greedy`. Historical milestone/proposal files
are not rewritten to imply that planned features already exist.

## Next: characterize jobs and dispatch costs

Start with two real operation types:

| Operation | Definition | Scheduling question |
| --- | --- | --- |
| FIR | Independent int16 block to exact int32 outputs; runtime coefficients and tap count | How much do DSP instructions change the relative per-core costs? |
| Threshold | Scan int32 input and produce a byte mask using a supplied threshold | Can simpler buffer-scanning work occupy M0+ while M4 does DSP-heavy work? |

Threshold is planned, not implemented yet. A reduction/count operation can be
added later to consume masks and create joins. Floating-point work is optional
if the initial operations do not expose useful variation. Do not add kernels
only to manufacture a favorable speedup.

Measure each supported implementation on both cores before fixing kernel
choices. The transport prototype currently calls the portable scalar FIR on
M0+; that is not a claim that it is M0+'s fastest implementation. Test its
unrolled fallback too. The greedy benchmark now tests both and selects a fixed
implementation per core/workload outside timing, without using job costs for
placement. Calibrate separately for 100/100 and 150/75 MHz.

Vary samples, FIR taps, job type, and input/output bytes. Include uniform jobs,
mixed job sizes, and mixed operation types. Both cores may favor the same job
type in absolute time; what matters is whether their relative speed ratio
changes across types. Do not assume M0+ must win a kernel outright.

Separate these costs:

- Once-per-run board, clock, IPC, and worker initialization.
- Once-per-plan coefficient preparation or operation registration.
- Per-job publication, interrupt/wake, validation, execution, and completion.
- Per-batch scheduling and any required buffer preparation/copies.

Use a no-work IPC request to characterize the handoff floor. The current
roundtrip-minus-worker interval is a residual, not a pure network-transfer
measurement. Input preparation and correctness checks stay outside the timed
interval, consistently for sequential and parallel execution.

Shared SRAM allows passing a descriptor/address instead of copying every input.
Actual memory costs include buffer lifetime, placement, bytes touched, and
contention between concurrent workers. Do not charge a fictitious UART/SPI
transfer for same-board jobs. Measure contention explicitly; isolated kernel
times are an initial estimate, not a complete concurrent memory model.

Exit: correct threshold operation, measured per-core cost tables, no-work
handoff measurements, and a documented distinction between reusable setup and
per-dispatch cost.

## Then: bounded queues and concurrent workers

The first step is implemented for preloaded independent FIR batches: bounded
storage, explicit ownership, a shared next-job index, and useful concurrent
work on both cores. The next extension is a general bounded enqueue API and
ready queues supporting multiple operation types and dependencies. The current
index resets between batches and is not a general streaming ring queue.

Do not keep M4 busy-waiting for each individual job in the actual parallel
benchmark. Include scheduling work on the core that performs it; the
coordinator's CPU time is not free. The greedy benchmark includes its per-batch
ready-list initialization in parallel elapsed time.

Exit: both cores perform useful work concurrently, every job completes exactly
once, output matches the sequential execution, and queue-full/empty behavior
is defined. Reserve bounded descriptor, queue, and buffer memory in both linkers.

## Baseline: profiled greedy static assignment

For an independent batch, start with a fixed deterministic job order: descending
`max(estimated_M4_time, estimated_M0_time)`, ties by job ID. For each job, choose
the eligible core with the lowest predicted finish time and update that core's
available time. Break equal finishes toward M4. Keep the ordering rule recorded
with results; compare another order if ordering sensitivity warrants it.

For the first additive model:

```
finish(job, core) = max(core_available, inputs_ready_on_core)
                  + estimated_execution(job, core)
                  + estimated_nonoverlapped_coordination(job, core)
```

For independent jobs, all inputs are ready at batch submission. For dependencies,
input readiness follows predecessor completion and required synchronization.
An unsupported assignment has infinite cost. Avoid counting the same waiting or
communication interval twice. Refine this initial model using measured overlap,
coordinator cost, and contention rather than assuming costs are independent.

Compute the static assignment once per submitted batch, then execute it without
redistribution. In the primary end-to-end comparison, start timing before any
per-batch planning. A repeated fixed-workload experiment may separately amortize
planning, but must label this explicitly. Treat this as a strong static baseline
that dynamic scheduling must justify its overhead against.

The familiar least-loaded-machine greedy guarantee of `2 - 1/m` assumes
identical processors (1.5 for two). It is not automatically a bound for our
heterogeneous, estimated-cost model with IPC and contention. A separate classic
2-approximation for unrelated machines uses LP rounding, not this greedy rule.
We will describe our algorithm as a heuristic and evaluate it empirically.
References: [CMU makespan notes](https://www.cs.cmu.edu/afs/cs/academic/class/15854-f05/www/scribe/lec03.pdf),
[Lenstra–Shmoys–Tardos](https://ir.cwi.nl/pub/18055).

For independent batches with at most 20 jobs, enumerate assignments offline on
the Mac and minimize `max(sum(M4 job costs), sum(M0+ job costs))`. This is an
exact optimum of the stated additive model, not an oracle for real contention,
unknown costs, or arbitrary DAG schedules. Compare model quality separately
from hardware makespan. Do not run exponential search in the MCU runtime.

## Compare dynamic policies

Keep the implemented first-available-worker greedy policy as the simplest
parallel baseline. It uses no cost model and is distinct from stealing or
profiled greedy static assignment. Compare it with optimized M4-only execution
to assess whether parallelism helps, and with the following policies to assess
the benefit of smarter placement.

Retain the proposal's three policy families:

1. **Weighted static:** the profiled greedy assignment above, with no stealing.
2. **Work stealing:** workers redistribute ready jobs when one runs out of
   work; count queue, steal, and synchronization costs.
3. **Cost-aware dynamic:** choose placements using current availability,
   operation/size-specific execution estimates, dispatch cost, and measured
   memory behavior. Maintain capability/eligibility constraints.

Hold workloads, kernel quality, clocks, buffer semantics, queue capacities, and
timing boundaries constant across policies. Uniform predictable work may favor
static assignment. Mixed job costs, prediction errors, or delayed readiness may
favor dynamic policies. Reporting that dynamic scheduling loses on small or
predictable jobs is a valid result.

## Add a small dependency graph

Use multiple independent channels or frames, each with a `FIR -> threshold`
chain. Later, add mask counts and a final aggregation to create a join. The
application declares these edges; the runtime schedules only ready jobs.

Track producer completion and consumer references so buffers remain valid until
all consumers finish. Keep blocks independent with zero filter history for the
initial workloads. A continuous stream would require explicit history/halo
handling and must not silently reset history at every chunk boundary.

For DAGs, priority by remaining critical-path cost is a later extension. HEFT
is a useful reference for heterogeneous task priorities and earliest-finish
placement; a full HEFT implementation is not required for the first scheduler.
[Original HEFT paper](https://disco.ethz.ch/courses/fs14/seminar/paper/Jochen/4.pdf).

## Evaluation and scope limits

Primary metric: total batch/graph makespan, from submission/planning through all
completions. Also report correctness, per-core useful/idle time, dispatch and
scheduling overhead, and peak runtime/working-buffer memory. Distinguish
benchmark reference/output-verification storage from runtime memory overhead.

For each new workload, establish its own strongest tested M4-only sequential
reference. The old FIR-only time is not a valid reference for a mixed pipeline.
Report optimized M4-only 150 MHz against parallel 150/75 MHz, and M4-only
100 MHz against parallel 100/100 MHz to isolate adding the worker at a fixed
clock configuration. Use comparable warmups and repeated trials, retain the
full granularity/type matrix, and include required copies and coordination in
parallel elapsed time.

Core scope remains one board. Two-board SPI/UART scheduling and bandwidth-aware
batching stay a stretch goal after the single-board runtime and evaluation.
Fallback remains independent jobs, fixed calibration, bounded storage, and
the static-versus-dynamic comparison before full dependency support.
