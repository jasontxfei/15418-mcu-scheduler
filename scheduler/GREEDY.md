# Naive greedy parallel baseline

This application runs useful FIR work concurrently on M4 and M0+. It provides
the first parallel comparison against a fresh optimized M4-only measurement.
It is a bounded preloaded batch, not yet a general enqueue API or DAG runtime.

## Assignment rule

Whenever either core finishes a job, it claims the next unclaimed descriptor
in the submitted order. There is no sorting, estimated finish time, job-type
preference, speed ratio, or communication-cost model in the assignment rule.
This is a central ready list, not work stealing from per-core queues.

Both workers briefly acquire IPC hardware lock channel 9 to increment the
shared next-job index and record ownership. The lock is released before the
kernel runs. Each descriptor owns a separate output range; either core may
claim it, but it must be claimed exactly once. M4 executes jobs itself while
M0+ runs, then waits for the remote worker only after it runs out of jobs.

IPC channel/interrupt 8 wakes M0+ once per batch. M0+ sleeps between batches.
Each core publishes its own completed count and completion epoch. The shared
pool cannot be reset or reused until both cores finish and the notification
channel is released. A timeout fails the benchmark without reusing live data.

An assignment is nonpreemptive: a long job already claimed by M0+ cannot be
stolen back when M4 runs out of work. This can produce a long completion tail.
The identical-machine list-scheduling approximation proof does not establish
a bound for this heterogeneous implementation; see [PLAN.md](PLAN.md).

## Bounded storage and workloads

The existing 16 KiB application shared region holds at most 32 FIR descriptors,
2048 total int16 input samples, 2048 int32 output samples, and a guard element
after every job's output. The complete shared object is 15,684 bytes in both
ELFs at `0x08043800`. Its components are:

- 12,288 bytes of input/output sample storage.
- 128 bytes of per-job output guards for testing.
- 2176 bytes of reusable coefficient plans (68 bytes per plan).
- 1092 bytes of remaining descriptors, control, and boundary guards.

Benchmark references, timing records, and saved diagnostic traces live in
M4-private RAM. They must not be reported as runtime scheduling overhead.

Twenty uniform workloads combine 64/128/256/512/1024 samples per job with
4/8/16/32 taps. Jobs per batch are `min(32, 2048 / samples_per_job)`, giving
32/16/8/4/2 jobs respectively. Each uniform batch has 2048 input samples.

Five additional cases cover 32 tiny one-sample jobs, 32 sixteen-sample jobs,
mixed block sizes with 8 taps, mixed block sizes with 32 taps, and mixed block
sizes plus tap counts. Mixed sizes repeat 16/32/64/128 samples for 1920 total
samples; mixed taps repeat 32/4/16/8. These are still all FIR operations.
Threshold and mixed operation types remain the next workload extension.

Every job has its own deterministic pseudorandom input and coefficients.
Plans and inputs are identical across the sequential and parallel comparisons.
Filtering uses independent zero history and exact int32 output semantics as
documented in [BENCHMARK.md](BENCHMARK.md).

Each table row is a separate workload. Changing block boundaries also changes
the zero-padding work near each block's beginning; compare sequential and
parallel times within the same row, not as identical arithmetic across rows.

## Strong sequential comparison and kernel selection

For every workload, measure these five modes with GCC `-O2`:

1. M4-only direct calls to the scalar kernel.
2. M4-only direct calls to the DSP kernel.
3. M0+-only execution with its scalar kernel.
4. M0+-only execution with the portable unrolled kernel.
5. Greedy parallel execution using the faster tested kernel for each core.

The M4-only path directly calls the filter for each descriptor. It does not
acquire a queue lock, wake M0+, update per-job runtime states, or wait for IPC.
Its buffers reside in the exact same shared pool as the parallel path. The
sequential reference is the smaller of the two M4 median batch times.

Kernel selection is a benchmark calibration step outside batch timing. It
chooses an implementation for a core, not a destination for a job. The greedy
assignment still uses no execution-time estimates. On M0+, `fir_filter_dsp`
is portable unrolled C; it does not execute unsupported M4 DSP instructions.

Each mode has three warmups and 31 verified timed trials. These are warmed
steady-state comparisons, not cold-start totals including all calibration.

## Timing, correctness, and overlap

Parallel timing includes ready-list/control initialization, publication, IPC
wakeup, lock contention, claiming, filtering, completion publication, imbalance,
and waiting until both workers are done. Input/plan preparation, output
poisoning used only for testing, reference calculation, and output verification
are outside timing. Sequential verification bookkeeping is also outside its
timer; it receives no artificial queue-management work.

Every parallel/worker-only job must have one owner, one claim, and DONE state.
Per-core completion counts must match the descriptor owners. Every output is
checked against an independent int64 reference, including output-tail and
whole-region guards. Five initial tests cover empty batches, zero-length jobs,
maximum job count, a 1024-sample block with odd tap count, and tiny jobs.

Primary timings disable per-job timestamp tracing. A separate fully verified
greedy batch for each workload records kernel start/end ticks from the common
TCPWM clock on both cores. The extraction tool independently checks those
intervals and their intersections. Positive intersection proves simultaneous
kernel execution; merely seeing both cores claim jobs would not prove overlap.
The traced batch time is not used for the reported speedup.

Each complete run verifies 4280 batches and 69,834 jobs, including 3875 timed
batches, 25 diagnostic traces, and five initial checks. There should be 2580
worker wake interrupts; M4-only modes leave M0+ asleep. Startup and full-output
verification are not mixed into kernel or batch elapsed time.

## Reproduce and interpret

From `scheduler` with the board connected through KitProg:

```sh
python3 tools/measure_greedy.py --clock-mhz 150 --output /tmp/greedy-150
python3 tools/measure_greedy.py --clock-mhz 100 --output /tmp/greedy-100
```

Manual builds use `make build -j4 APP_MODE=greedy BENCH_OPT=-O2`. The existing
`APP_MODE=sequential` and `APP_MODE=dispatch` applications remain available.
Both cores sleep after the comparison finishes; resetting reruns it.

Retain two distinct comparisons:

- **Fixed clock:** M4-only 100 MHz versus greedy 100/100 MHz, and M4-only
  150 MHz versus greedy 150/75 MHz. This isolates adding the second worker.
- **Strongest measured sequential reference:** the fastest tested M4-only
  configuration versus each parallel configuration. A win against M4 at
  100 MHz must not be presented as beating M4 at 150 MHz.

`speedup = sequential_elapsed / parallel_elapsed`. Values below 1 mean a
slowdown. Preserve these results rather than discarding cases where naive
greedy loses. Small jobs expose scheduling costs; coarse jobs can leave a
straggler on M0+; mixed sizes can change that balance. The traces and job
counts help explain specific cases, but they do not isolate every cause or
prove that one later policy will win.

Captured trials, kernel choices, job counts, traces, and cross-clock comparisons
are in [benchmarks/greedy-results.json](benchmarks/greedy-results.json).
The compact table is also available as [benchmarks/greedy-summary.csv](benchmarks/greedy-summary.csv).

<!-- RESULTS_START -->
Final accepted sweeps: 31 timed trials per mode/workload/clock, with queue
initialization included. Greedy beats the fastest tested M4-only reference
in 2 of 25 workloads; the best observed median speedup is
1.103x. At fixed 100/100 MHz, it beats M4-only 100 MHz
in 19 of 25 workloads. Those are different comparisons.

| Workload | Fastest M4-only (us) | Greedy 150/75 (us) | Greedy 100/100 (us) | Speedup vs fastest M4-only |
| --- | ---: | ---: | ---: | ---: |
| 32 x 64 samples, 4 taps | 279.28 | 319.97 | 396.81 | 0.873x |
| 32 x 64 samples, 8 taps | 436.51 | 497.63 | 570.27 | 0.877x |
| 32 x 64 samples, 16 taps | 779.12 | 848.60 | 978.16 | 0.918x |
| 32 x 64 samples, 32 taps | 1576.99 | 1531.51 | 1886.10 | 1.030x |
| 16 x 128 samples, 4 taps | 264.35 | 299.52 | 370.18 | 0.883x |
| 16 x 128 samples, 8 taps | 414.64 | 476.68 | 591.71 | 0.870x |
| 16 x 128 samples, 16 taps | 729.31 | 829.67 | 1029.40 | 0.879x |
| 16 x 128 samples, 32 taps | 1414.96 | 1524.69 | 1887.22 | 0.928x |
| 8 x 256 samples, 4 taps | 256.88 | 377.73 | 332.49 | 0.773x |
| 8 x 256 samples, 8 taps | 403.71 | 611.07 | 517.35 | 0.780x |
| 8 x 256 samples, 16 taps | 704.40 | 1076.45 | 890.92 | 0.791x |
| 8 x 256 samples, 32 taps | 1333.95 | 2000.85 | 1668.00 | 0.800x |
| 4 x 512 samples, 4 taps | 253.15 | 370.71 | 323.01 | 0.784x |
| 4 x 512 samples, 8 taps | 398.24 | 604.51 | 506.37 | 0.786x |
| 4 x 512 samples, 16 taps | 691.95 | 1070.29 | 871.92 | 0.794x |
| 4 x 512 samples, 32 taps | 1293.44 | 1999.95 | 1618.17 | 0.799x |
| 2 x 1024 samples, 4 taps | 251.28 | 714.72 | 537.04 | 0.468x |
| 2 x 1024 samples, 8 taps | 395.51 | 1171.04 | 888.78 | 0.445x |
| 2 x 1024 samples, 16 taps | 685.72 | 2085.19 | 1578.43 | 0.434x |
| 2 x 1024 samples, 32 taps | 1273.19 | 3917.69 | 2952.22 | 0.431x |
| 32 x 1 samples, 8 taps | 17.12 | 52.61 | 63.03 | 0.325x |
| 32 x 16 samples, 8 taps | 142.11 | 183.95 | 214.29 | 0.773x |
| Mixed sizes, 8 taps | 411.97 | 417.76 | 542.62 | 0.986x |
| Mixed sizes, 32 taps | 1405.89 | 1274.81 | 1704.67 | 1.103x |
| Mixed sizes and taps | 508.56 | 566.93 | 651.24 | 0.897x |

Speedup is serial time divided by parallel time; below 1 is a slowdown.
All fastest M4-only references in this sweep used the 150 MHz configuration.
Both cores overlapped in every one of the 50 separately traced batches.
The two accepted sweeps verified 8560 batches and 139,668 job executions.
M4-only and single-job dispatch regression applications also passed on hardware.
<!-- RESULTS_END -->

The next scheduling comparison is profiled greedy static assignment versus
this first-available baseline, followed by cost-aware decisions. Add threshold
jobs to test different operation speed ratios; retain the same strong
sequential-reference and timing rules for each new workload.
