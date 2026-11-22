#!/usr/bin/env python3
"""Build/flash the one-slot IPC benchmark and verify cross-core execution.

No parallel-speedup claim: M4 waits for each M0+ completion. Measurements cover
zero-work handoff and FIR handoff, with preparation/verification outside timing.
"""
import argparse
import hashlib
import json
import pathlib
import re
import statistics
import subprocess
import time

from measure_sequential import PROJECT, PROGRAM_TOOLS, GCC, configure_clock, run


def read_symbols():
    symbols = {}
    shared = []
    for core in ['proj_cm0p', 'proj_cm4']:
        elf = PROJECT/core/'build/last_config'/(core+'.elf')
        listing = subprocess.check_output([GCC/'arm-none-eabi-nm','-S',elf],text=True)
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) != 4:
                continue
            address, size, _, name = fields
            if name == 'runtime_mailbox':
                shared.append((int(address,16),int(size,16)))
            if name.startswith(('rbench_', 'rworker_')):
                symbols[name] = (int(address,16),int(size,16))
    assert len(shared) == 2 and shared[0] == shared[1]
    assert shared[0][0] == 0x08043800 and shared[0][1] <= 0x4000
    return symbols, shared[0]


def inspect(out, symbols, shared):
    status = symbols['rbench_status'][0]
    commands = ['set ENABLE_ACQUIRE 0', 'source [find interface/kitprog3.cfg]',
                'source [find target/psoc6.cfg]', 'init', 'targets psoc6.cpu.cm4',
                'set finished 0', 'for {set i 0} {$i < 120} {incr i} {',
                f'  set s [lindex [read_memory 0x{status:08x} 32 1] 0]',
                '  if {$s == 2 || $s == 3} {set finished 1; break}', '  sleep 500', '}',
                'if {!$finished} {error "dispatch benchmark did not complete"}', 'halt']
    for name,(address,size) in sorted(symbols.items()):
        assert size%4 == 0
        commands.append(f'echo "DISPATCH_RESULT {name}=[read_memory 0x{address:08x} 32 {size//4}]"')
    address,size = shared
    commands += [f'echo "DISPATCH_RESULT mailbox_header=[read_memory 0x{address:08x} 32 9]"',
                 f'echo "DISPATCH_RESULT mailbox_guard_end=[read_memory 0x{address+size-4:08x} 32 1]"',
                 'resume', 'shutdown']
    config = out/'inspect.cfg'
    config.write_text('\n'.join(commands)+'\n')
    log = out/'debugger.log'
    run([PROGRAM_TOOLS/'openocd/bin/openocd','-s',PROGRAM_TOOLS/'openocd/scripts','-f',config],log,90)
    raw = {}
    for line in log.read_text().splitlines():
        match = re.fullmatch(r'DISPATCH_RESULT (\w+)=(.*)',line)
        if match:
            raw[match[1]] = [int(x,0) for x in match[2].split()]
    return raw


def summarize(raw, clock):
    def one(name):
        return raw[name][0]
    assert one('rbench_status') == 2, {k:v for k,v in raw.items() if k!='rbench_results'}
    assert one('rbench_error') == one('rworker_error') == 0
    assert one('rbench_core_clock_hz') == clock*1_000_000
    peripheral_hz = clock*1_000_000//(2 if clock>100 else 1)
    assert one('rworker_clock_hz') == one('rbench_timer_clock_hz') == peripheral_hz
    assert one('rbench_records') == 14 and one('rbench_jobs_verified') == 604
    assert one('rbench_protocol_checks') == 388
    assert one('rworker_interrupts') == 604 and one('rworker_sleep_entries') > 0
    guard,state,ready,sequence,operation,samples,result,worker_ticks,executions = raw['mailbox_header']
    assert guard == one('mailbox_guard_end') == 0xA15418B6
    assert state == 0 and ready == 1 and sequence == executions == 604 and result == 0
    width = one('rbench_record_words')
    records = raw['rbench_results']
    assert width == 68 and len(records) == 14*width
    scale = 1_000_000/peripheral_hz
    rows = []
    for i in range(0,len(records),width):
        op,n,k,paced,trials,verified,*ticks = records[i:i+width]
        assert trials == verified == 31
        roundtrip,worker = ticks[:31],ticks[31:]
        residual = [r-w for r,w in zip(roundtrip,worker)]
        assert min(residual) >= 0
        rows.append(dict(operation=['noop','fir'][op],samples=n,taps=k,
                         pacing=['no deliberate delay','100us delay outside timing'][paced],
                         trials=trials,roundtrip_ticks=roundtrip,worker_ticks=worker,
                         roundtrip_us_median=statistics.median(roundtrip)*scale,
                         roundtrip_us_min=min(roundtrip)*scale,
                         roundtrip_us_max=max(roundtrip)*scale,
                         worker_us_median=statistics.median(worker)*scale,
                         residual_us_median=statistics.median(residual)*scale))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    parser.add_argument('--clock-mhz',type=int,choices=[100,150],default=150)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    configure_clock(args.clock_mhz)
    print('Building IPC benchmark',args.clock_mhz,'MHz',flush=True)
    run(['make','build','-j4','APP_MODE=dispatch','BENCH_OPT=-O2'],out/'build.log')
    print('Programming both cores',flush=True)
    run(['make','program','APP_MODE=dispatch','BENCH_OPT=-O2'],out/'program.log',180)
    image = PROJECT/'build/app_combined.hex'
    (out/'app_combined.hex').write_bytes(image.read_bytes())
    symbols,shared = read_symbols()
    raw = inspect(out,symbols,shared)
    (out/'raw.json').write_text(json.dumps(raw,indent=2)+'\n')
    rows = summarize(raw,args.clock_mhz)
    result = dict(clock_mhz=args.clock_mhz,optimization='O2',collected_unix=time.time(),
                  shared_address=hex(shared[0]),shared_bytes=shared[1],
                  hex_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                  source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=PROJECT,text=True).strip(),
                  source_diff=subprocess.check_output(['git','diff','HEAD'],cwd=PROJECT,text=True),
                  raw=raw,measurements=rows,
                  notes=['Single outstanding job; M4 waits and M0+ executes. No parallel speedup measured.',
                         'Roundtrip includes publication, wakeup, validation, kernel, completion polling and collection.',
                         'Residual is roundtrip minus worker interval, not an isolated communication cost.',
                         'Preparation, reference, verification and optional 100us pacing are outside timing.'])
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: 604 jobs, 388 protocol checks, 434 measured round trips',flush=True)
    for row in rows:
        if row['operation']=='noop' or (row['samples']==256 and row['taps']==8):
            print(row['operation'],row['samples'],row['taps'],row['pacing'],
                  f"roundtrip={row['roundtrip_us_median']:.3f}us worker={row['worker_us_median']:.3f}us",flush=True)


if __name__ == '__main__':
    main()
