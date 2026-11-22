# First cross-core job path

This is a one-slot transport and measurement prototype. M4 submits one request
and waits for M0+ to finish. It does not yet run useful work on both cores
simultaneously, select a core, manage a queue, or schedule dependencies.
The broader implementation plan is in [PLAN.md](PLAN.md).

## What happens on the board

1. M0+ performs board initialization, clears the application mailbox once,
   installs its IPC wake interrupt, and starts M4.
2. M0+ waits in CPU sleep (`WFI`). Its interrupt handler only acknowledges IPC
   and marks work pending; the FIR kernel runs in foreground after wakeup.
3. While the slot is FREE, M4 fills the plan/input, assigns a strictly increasing
   nonzero sequence ID, and publishes READY with memory barriers.
4. `Cy_IPC_Drv_SendMsgPtr` acquires hardware channel 8, sends the fixed mailbox
   address, and notifies IPC interrupt 8. On M0+, this maps to NVIC mux 2,
   leaving startup's system interrupt muxes 0 and 1 intact.
5. M0+ validates the descriptor and coefficient bound, marks RUNNING, computes
   the output, publishes DONE, then releases the IPC channel with a barrier.
6. M4 polls the channel lock, applies a read barrier after observing release,
   checks the sequence/result/guards, and collects the completion. The slot
   becomes FREE. Output remains valid until M4 submits another request.

The state sequence is `FREE -> READY -> RUNNING -> DONE -> FREE`. Neither
`volatile` nor an interrupt alone is the ownership protocol: channel ownership,
barriers, sequence IDs, and buffer lifetime rules work together. A timed-out
request must not be reset or reused while M0+ might still be writing it.

The descriptor and buffers currently live together in one fixed slot, holding
up to 1024 inputs, 1024 int32 outputs, and a 32-tap plan. There is no pointer to
downloaded code or shared function pointer. The operation ID selects code that
is already compiled into the worker firmware. Supported operations are FIR
and an instrumentation-only NOOP for handoff measurement. Threshold remains
planned; NOOP is not our second representative application workload.

## Memory and flash layout

| Region | Address interval (end exclusive) | Size |
| --- | --- | --- |
| M0+ private SRAM | `0x08000000–0x08004000` | 16 KiB |
| M4 private SRAM | `0x08004000–0x08043800` | 254 KiB |
| Application shared SRAM | `0x08043800–0x08047800` | 16 KiB |
| Reserved system SRAM | `0x08047800–0x08048000` | 2 KiB |

Both linker scripts reserve `.runtime_shared` as NOLOAD at the same address;
the mailbox occupies 6252 bytes of that region. Normal BSS/data/heap/stack
cannot overlap it, and M4 startup does not clear M0+'s initialized mailbox.
The controller checks both ELF symbol addresses and sizes before accepting a
run. This application region is separate from the vendor `.cy_sharedmem`
section used by startup/system code.

M0+ has a 32 KiB flash partition; M4 begins at `0x10008000`. Its launch address,
both linker scripts, and the M4 combined-image boundary are changed together.
The original sequential reference remains available on branch
`jason/sequential-baseline` at commit `1017bce`. Remeasure comparisons when
linker layout, kernels, or build configuration change.

## Build and validate

From `scheduler`, with the same macOS tool installation used for the sequential
benchmark and the board connected through KitProg:

```sh
make getlibs
python3 tools/measure_dispatch.py --output /tmp/mcu-dispatch-150
```

`--clock-mhz 100` tests 100/100 MHz; default 150 tests M4 150/M0+ 75 MHz.
The tool validates core and timer frequencies and uses the actual configured
timer frequency when converting ticks. It waits for the benchmark to finish
before halting for extraction and resumes M4 afterward. It saves raw words,
per-trial ticks, summaries, logs, exact HEX/hash, and source identity/diff.

Manual commands are `make build -j4 APP_MODE=dispatch BENCH_OPT=-O2` and
`make program APP_MODE=dispatch BENCH_OPT=-O2`. A board reset reruns the test.
M4 sleeps after success; M0+ remains a sleeping resident worker. No LED or
serial output is expected.

The default build remains `APP_MODE=sequential`. The original batch driver
can be run using `tools/measure_sequential.py`; it explicitly requests that
mode so the two test applications cannot accidentally be confused.

## Test and timing boundaries

Each run executes 604 requests: 128 changing-input FIR protocol/correctness
checks, 42 warmups, and 434 measured requests (14 cases, 31 trials each).
It checks 388 protocol conditions, including rejected invalid requests,
busy resubmission, stale collection, and rejected reuse of a sequence ID.
The worker execution count, interrupt count, last sequence, and controller
verified count must agree. Every FIR output is compared against an independent
int64 reference, with guards and an untouched output-tail sentinel.

Measured cases are NOOP and FIR with 64/256/1024 samples and 8/32 taps.
Each is tested without an intentional pause and with a 100 microsecond pause
outside timing. Both modes permit the worker to sleep; the first is not a
claim of a continuously spinning or already-awake worker.

Roundtrip timing begins before publication and ends after collection. It
includes IPC, wakeup, worker validation, computation, completion detection,
and controller polling. Input/plan preparation, reference generation, output
verification, and optional pacing are outside timing. The worker interval
brackets the kernel with timer access/barrier overhead. Its difference from
the roundtrip is a coordination residual, not an isolated copy or wire cost.
The primary roundtrip times are not adjusted by subtracting instrumentation.

At 150/75 MHz, the first run's median NOOP roundtrip was about 5.88 microseconds.
For a 256-sample, 8-tap FIR job, the M0+ scalar kernel interval was about
424.84 microseconds and the full roundtrip about 432.55 microseconds without
an intentional pause. These characterize this prototype, not a parallel
speedup or the fastest possible M0+ implementation. The runtime plan requires
profiling kernel choices and concurrent memory behavior before scheduling.

Validation also passed at 100/100 MHz and again after returning to 150/75 MHz:
1812 verified requests and 1164 protocol checks across three firmware resets,
including 1302 timed round trips. The NOOP median at 100/100 MHz was about
5.14 microseconds. Raw timing samples and per-run counters are in
[benchmarks/dispatch-results.json](benchmarks/dispatch-results.json). The
preserved sequential mode also passed its 40 kernel/workload cases with 31
verified trials each after the shared-memory reservation was added.

The next change is a real second operation and per-core calibration, followed
by bounded queues and concurrent local/remote execution. Keep this single-slot
path as a correctness and dispatch-cost reference while those are developed.
