#!/usr/bin/env python3
"""Build, flash, and inspect the M4-only FIR benchmark through KitProg3.

The debugger waits for bench_status before halting either core. Inputs and the
64-bit reference are generated on the board outside timing. Raw elapsed ticks
include batch traversal and function calls; timer overhead is recorded separately.
"""
import argparse
import hashlib
import json
import pathlib
import re
import statistics
import subprocess
import time

PROJECT = pathlib.Path(__file__).resolve().parents[1]
PROGRAM_TOOLS = pathlib.Path('/Applications/ModusToolboxProgtools-2.0')
GCC = pathlib.Path.home() / 'Applications/mtb-gcc-arm-eabi/14.2.1/gcc/bin'


def run(command, logfile, timeout=300):
    with logfile.open('w') as stream:
        subprocess.run([str(x) for x in command], cwd=PROJECT, stdout=stream,
                       stderr=subprocess.STDOUT, check=True, timeout=timeout)


def symbols():
    result = {}
    for core in ['proj_cm0p', 'proj_cm4']:
        elf = PROJECT / core / 'build/last_config' / (core + '.elf')
        listing = subprocess.check_output([GCC / 'arm-none-eabi-nm', '-S', elf], text=True)
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) == 4 and (fields[3].startswith('bench_') or fields[3].startswith('baseline_m0_')):
                result[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    return result


def inspect(out, table):
    # No reset here: make program already reset and started the combined image.
    # Poll non-intrusively via the debug access port, then halt only to dump data.
    status = table['bench_status'][0]
    config = out / 'inspect.cfg'
    commands = ['set ENABLE_ACQUIRE 0', 'source [find interface/kitprog3.cfg]',
                'source [find target/psoc6.cfg]', 'init', 'targets psoc6.cpu.cm4',
                'set finished 0', 'for {set i 0} {$i < 120} {incr i} {',
                f'  set s [lindex [read_memory 0x{status:08x} 32 1] 0]',
                '  if {$s == 2 || $s == 3} {set finished 1; break}', '  sleep 500', '}',
                'if {!$finished} {error "benchmark did not complete"}', 'halt']
    for name, (address, size) in sorted(table.items()):
        if size % 4 != 0:
            raise RuntimeError('Unexpected symbol layout: ' + name)
        commands.append(f'echo "FIR_RESULT {name}=[read_memory 0x{address:08x} 32 {size//4}]"')
    commands += ['resume', 'shutdown']
    config.write_text('\n'.join(commands) + '\n')
    log = out / 'debugger.log'
    run([PROGRAM_TOOLS / 'openocd/bin/openocd', '-s', PROGRAM_TOOLS / 'openocd/scripts',
         '-f', config], log, timeout=90)
    values = {}
    for line in log.read_text().splitlines():
        match = re.fullmatch(r'FIR_RESULT (\w+)=(.*)', line)
        if match:
            values[match[1]] = [int(word, 0) for word in match[2].split()]
    for name in table:
        if name not in values:
            raise RuntimeError('Missing debugger symbol: ' + name)
    return values


def summarize(values, clock_mhz):
    def one(key):
        return values[key][0]
    assert one('bench_magic') == 0x46495231 and one('bench_version') == 1
    assert one('bench_status') == 2 and one('bench_error') == 0, {
        key: values[key] for key in values if key != 'bench_results'}
    core_hz = clock_mhz * 1_000_000
    peripheral_hz = core_hz // (2 if clock_mhz > 100 else 1)
    assert one('bench_core_clock_hz') == core_hz
    assert one('bench_slow_clock_hz') == one('baseline_m0_clock_hz') == peripheral_hz
    assert one('bench_timer_clock_hz') == peripheral_hz
    assert one('baseline_m0_ready') == one('bench_dsp_available') == 1
    assert one('bench_results_completed') == 40
    width = one('bench_record_words')
    records = values['bench_results']
    assert len(records) == 40 * width
    rows = []
    for start in range(0, len(records), width):
        samples, taps, kernel, jobs, trials, verified, *ticks = records[start:start+width]
        assert trials == verified == len(ticks) == 31 and jobs == 16
        scale = 1_000_000 / one('bench_timer_clock_hz')
        rows.append(dict(clock_mhz=clock_mhz, samples=samples, taps=taps, kernel=['scalar', 'dsp'][kernel],
                         jobs=jobs, verified_trials=verified, ticks=ticks,
                         batch_us_median=statistics.median(ticks)*scale,
                         batch_us_min=min(ticks)*scale, batch_us_max=max(ticks)*scale,
                         per_job_us_median=statistics.median(ticks)*scale/jobs))
    return rows


def configure_clock(clock_mhz):
    design = PROJECT / 'bsps/TARGET_APP_CY8CPROTO-063-BLE/config/design.modus'
    text = design.read_text()
    settings = [('pll', 'desiredFrequency', f'{clock_mhz:.3f}'),
                ('hfclk', 'sourceClockNumber', '1'),
                ('periclk', 'divider', '2' if clock_mhz > 100 else '1'),
                ('slowclk', 'divider', '1')]
    for block, param, value in settings:
        pattern = (r'(<Block location="srss\[0\]\.clock\[0\]\.'+block+
                   r'\[0\]">(?:(?!</Block>).)*?<Param id="'+param+r'" value=")[^"]*("/>)')
        text, count = re.subn(pattern, lambda m: m[1]+value+m[2], text, flags=re.S)
        if count != 1:
            raise RuntimeError('Clock design layout changed: '+block+'/'+param)
    if text != design.read_text():
        design.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--clock-mhz', type=int, choices=[100, 150], default=150)
    parser.add_argument('--optimizations', nargs='+', choices=['Os', 'O2', 'O3'], default=['Os', 'O2', 'O3'])
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    configure_clock(args.clock_mhz)
    all_rows = []
    for opt in args.optimizations:
        out = root / opt
        out.mkdir(exist_ok=True)
        print('Building', opt, flush=True)
        run(['make', 'build', '-j4', 'BENCH_OPT=-'+opt], out/'build.log')
        print('Programming', opt, flush=True)
        run(['make', 'program', 'BENCH_OPT=-'+opt], out/'program.log', timeout=180)
        image = PROJECT / 'build/app_combined.hex'
        # Archive the exact flashed image and source identity with every run.
        (out/'app_combined.hex').write_bytes(image.read_bytes())
        values = inspect(out, symbols())
        (out/'raw.json').write_text(json.dumps(values, indent=2)+'\n')
        rows = summarize(values, args.clock_mhz)
        for row in rows:
            row['optimization'] = opt
        result = dict(optimization=opt, clock_mhz=args.clock_mhz, collected_unix=time.time(),
                      hex_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                      source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=PROJECT,text=True).strip(),
                      source_diff=subprocess.check_output(['git','diff','HEAD'],cwd=PROJECT,text=True),
                      raw=values, measurements=rows)
        (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')
        all_rows.extend(rows)
        print(opt, ': PASS, 40 cases x 31 fully verified trials', flush=True)
        (root/'measurements.json').write_text(json.dumps(all_rows,indent=2)+'\n')
    winners = []
    for samples in [64,128,256,512,1024]:
        for taps in [4,8,16,32]:
            candidates = [r for r in all_rows if r['samples']==samples and r['taps']==taps]
            winners.append(min(candidates,key=lambda r:r['batch_us_median']))
    (root/'fastest-measured.json').write_text(json.dumps(winners,indent=2)+'\n')
    print('Results:', root, flush=True)


if __name__ == '__main__':
    main()
