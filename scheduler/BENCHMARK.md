# Sequential general FIR benchmark

`APP_MODE=sequential` establishes the M4-only reference before adding a scheduler.
It has no task queue, dependencies, or work stealing. The separate
`APP_MODE=dispatch` prototype tests a single remote job; see [DISPATCH.md](DISPATCH.md).
The mixed-workload and scheduling roadmap is in [PLAN.md](PLAN.md).
`APP_MODE=greedy` now compares concurrent first-available workers with fresh
direct M4-only timings on identical shared buffers; see [GREEDY.md](GREEDY.md).

## Work and integer semantics

One job filters one independent block:

```
output[n] = sum(coefficients[k] * input[n-k])
```

Samples before the beginning of each block are zero. There is no history carried
between jobs. Coefficient 0 multiplies the current input. Inputs and coefficients
are signed 16-bit integers; outputs are exact, unscaled signed 32-bit integers.
These are not normalized/saturated Q15 outputs.

`fir_plan_init` copies 1–32 caller-supplied coefficients into an immutable plan.
It rejects coefficient sets whose sum of absolute values exceeds 65,535. That
bound guarantees that every partial sum fits in 32 bits for every signed 16-bit
input, including -32,768. This is an explicit supported input domain, not silent
overflow or clipping. Plan initialization is separate from filtering and must
be amortized identically in subsequent sequential and parallel comparisons.

`fir_filter_scalar` is a portable nested-loop kernel with separate startup and
steady-state loops. `fir_filter_dsp` computes four outputs at a time using M4
dual signed 16-bit multiply-accumulate instructions. Both accept runtime tap
counts and coefficients and implement identical output semantics. The DSP entry
point has a portable scalar fallback for M0+ and host tests. Neither allocates
memory, modifies the plan, or performs IPC. The M4 path uses normal SRAM word
loads that can be halfword aligned; unaligned access trapping must be disabled.

## Measurement protocol

- Default M4: PLL at nominal 150 MHz. M0+: 75 MHz. Peripheral/TCPWM: 75 MHz.
  The M0+ clock is derived from the peripheral clock, so its extra divider is /1.
  The vendor Device Configurator generates the clock and flash configuration.
- The M0+ initializes the board, starts the M4, and enters CPU sleep (`WFI`).
  There is no LED heartbeat or UART traffic during the benchmark.
- Each batch has 16 separate input/output buffers and 16 plans. Deterministic
  pseudorandom coefficients and full-range inputs differ between jobs and are
  generated on the board. Both kernels receive the exact same workload.
- Sweep 64, 128, 256, 512, and 1024 samples per job and 4, 8, 16, and 32 taps.
  This gives 20 workloads, each measured with both kernels.
- Three batch warmups precede 31 timed repetitions of each kernel/workload.
  This is a warm steady-state benchmark, not a cold-start measurement.
- A dedicated free-running 32-bit TCPWM counter brackets the complete batch.
  Elapsed ticks include job traversal, kernel calls, barriers, and timer reads.
  Empty timer-read overhead is recorded separately and is not subtracted.
- Input/plan preparation, the independent 64-bit reference, full-output
  verification, and debugger extraction are outside the measured interval.
  Every output of every timed repetition must match the reference.
- Before timing, on-device checks exercise zero/short blocks, odd tap counts,
  and invalid coefficient bounds. Host tests also check numeric extremes,
  output guards, and every supported tap count under sanitizers.
- Compare GCC `-Os`, `-O2`, and `-O3`, with no LTO or fast-math. The `Custom`
  configuration makes the optimization explicit; vendor `Release` uses `-Os`.
  Record median, minimum, maximum, raw ticks, and measured configuration.

## Reproduce

Install ModusToolbox 3.9, programming tools 2.0, and Arm GCC 14.2.1 at their
standard macOS locations. Connect the board through the KitProg USB connector.
Run these commands from `scheduler`:

```sh
make getlibs
clang -O3 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Ishared shared/fir.c tests/test_fir.c \
  -o /tmp/test-general-fir
/tmp/test-general-fir
python3 tools/measure_sequential.py --output /tmp/fir-measurements
```

The optional `--clock-mhz 100` tests 100/100 MHz instead. The script updates
`design.modus` for the chosen clock and lets the vendor build regenerate code;
it leaves the design and firmware at that setting. Use a separate output folder
for each clock configuration. Compare both to determine the fastest elapsed
time rather than assuming the highest CPU frequency wins.

The measurement script flashes the board for each optimization, waits for the
benchmark to finish before halting, checks clocks and all completion statuses,
and resumes the M4 after extraction. It saves raw debugger words, all 31 timing
samples per case, the exact HEX, SHA-256, build/program/debugger logs, source
commit, and tracked source diff. A failed run is not accepted as timing data.
The resulting firmware sleeps after finishing; no blinking LED is expected.

For a single manual build: `make build -j4 BENCH_OPT=-O2`. Configuration flags
are passed to both core projects. The default is `-O2`.

## Use in the later parallel comparison

The November 22 hardware sweep at commit `1017bce` tested both clocks, all three compiler options,
and both kernels: 240 configurations with 31 fully verified trials each.
M4 at 150 MHz with the DSP kernel and `-O2` had the lowest median time in every
one of the 20 workloads. It is the default sequential reference. For example,
16 independent jobs of 256 samples and 8 taps took about 806.9 microseconds
per batch (50.4 microseconds per job). See
[benchmarks/sequential-results.json](benchmarks/sequential-results.json) for
all raw ticks, timing ranges, and the complete comparison matrix. These are
sequential measurements; they do not establish a parallel speedup. The later
cross-core branch reserves shared SRAM and changes the firmware flash boundary;
remeasure the reference when comparing that build rather than treating these
historical times as immutable across layouts and compiler changes.

The primary speedup numerator is the fastest measured correct sequential
configuration for the same batch. This is the best of the tested kernels and
compiler options, not a claim of globally optimal assembly. The output
`fastest-measured.json` retains the winner for every workload; retain the full
matrix as well to avoid hiding unfavorable cases.

The parallel version must process the same buffers, coefficients, block
boundaries, output precision, and number of jobs, with similarly optimized
per-core kernels. Time from dispatch through completion, including scheduler,
IPC, waiting, and any per-batch buffer work. Preserve comparable memory
placement and warmup conditions. Additional copies required only by the
parallel implementation belong in its elapsed time.

Keep the optimized M4-only 150 MHz comparison for the overall speedup claim.
Also run M4-only 100 MHz versus dual-core 100/100 MHz to isolate the benefit of
adding a worker at a fixed hardware configuration. Dynamic policies additionally
need varied job costs and dependencies; uniform FIR batches alone do not
establish a benefit over a tuned static assignment.
