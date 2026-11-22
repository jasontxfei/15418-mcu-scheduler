#!/usr/bin/env python3
"""Measure direct M4 execution versus first-available-worker FIR batches."""
import argparse
import hashlib
import json
import pathlib
import re
import statistics
import subprocess
import time

from measure_sequential import PROJECT, PROGRAM_TOOLS, GCC, configure_clock, run

MODES = ['m4_scalar', 'm4_dsp', 'm0_scalar', 'm0_unrolled', 'greedy']


def read_symbols():
    symbols, shared = {}, []
    for core in ['proj_cm0p', 'proj_cm4']:
        elf=PROJECT/core/'build/last_config'/(core+'.elf')
        listing=subprocess.check_output([GCC/'arm-none-eabi-nm','-S',elf],text=True)
        for line in listing.splitlines():
            fields=line.split()
            if len(fields)!=4:
                continue
            address,size,kind,name=fields
            if name=='greedy_shared': shared.append((int(address,16),int(size,16)))
            # This BSP's linker marks some RAM metadata as type T. Names are
            # reserved for data; filtering by B/D would silently omit it.
            if name.startswith(('gbench_','gworker_')):
                symbols[name]=(int(address,16),int(size,16))
    assert len(shared)==2 and shared[0]==shared[1]
    assert shared[0][0]==0x08043800 and shared[0][1]<=0x4000
    return symbols,shared[0]


def inspect(out,symbols,shared):
    status=symbols['gbench_status'][0]
    commands=['set ENABLE_ACQUIRE 0','source [find interface/kitprog3.cfg]',
              'source [find target/psoc6.cfg]','init','targets psoc6.cpu.cm4',
              'set finished 0','for {set i 0} {$i < 120} {incr i} {',
              f' set s [lindex [read_memory 0x{status:08x} 32 1] 0]',
              ' if {$s == 2 || $s == 3} {set finished 1; break}',' sleep 500','}',
              'if {!$finished} {error "greedy benchmark did not complete"}','halt']
    # Chunk large tables so no individual debugger output line is enormous.
    for name,(address,size) in sorted(symbols.items()):
        assert size%4==0
        for offset in range(0,size//4,256):
            count=min(256,size//4-offset)
            commands.append(f'echo "GREEDY_RESULT {name} {offset}=[read_memory 0x{address+offset*4:08x} 32 {count}]"')
    address,size=shared
    commands += [f'echo "GREEDY_RESULT shared_header 0=[read_memory 0x{address:08x} 32 16]"',
                 f'echo "GREEDY_RESULT shared_end_guard 0=[read_memory 0x{address+size-4:08x} 32 1]"',
                 'resume','shutdown']
    config=out/'inspect.cfg'
    config.write_text('\n'.join(commands)+'\n')
    log=out/'debugger.log'
    run([PROGRAM_TOOLS/'openocd/bin/openocd','-s',PROGRAM_TOOLS/'openocd/scripts','-f',config],log,90)
    raw={}
    for line in log.read_text().splitlines():
        match=re.fullmatch(r'GREEDY_RESULT (\w+) (\d+)=(.*)',line)
        if match:
            name,offset,data=match.groups()
            target=raw.setdefault(name,[])
            assert len(target)==int(offset),(name,offset,len(target))
            target.extend(int(x,0) for x in data.split())
    return raw


def summarize(raw,clock):
    one=lambda name: raw[name][0]
    assert one('gbench_status')==2,{k:v for k,v in raw.items() if k not in ['gbench_results','gbench_trace']}
    assert one('gbench_error')==one('gworker_error')==0
    timer=clock*1_000_000//(2 if clock>100 else 1)
    assert one('gbench_core_clock_hz')==clock*1_000_000
    assert one('gbench_timer_clock_hz')==one('gworker_clock_hz')==timer
    assert one('gbench_records')==125 and one('gbench_traces')==25
    assert one('gbench_prechecks')==5 and one('gbench_verified_batches')==4280
    assert one('gbench_verified_jobs')==69834 and one('gworker_interrupts')==2580
    guard,ready,epoch,mode,jobs,total,k4,k0,trace,nxt,d4,d0,c4,c0,e4,e0=raw['shared_header']
    assert guard==one('shared_end_guard')==0xB15418A7
    assert ready==1 and epoch==d4==d0==4280 and mode==2
    assert jobs==nxt==c4+c0==32 and e4==e0==0
    width=one('gbench_record_words')
    assert width==103 and len(raw['gbench_results'])==125*width
    scale=1_000_000/timer
    rows=[]
    for offset in range(0,len(raw['gbench_results']),width):
        case,mode,jobs,total,n,taps,k4,k0,trials,verified,*data=raw['gbench_results'][offset:offset+width]
        assert trials==verified==31 and mode<5 and case<25
        ticks,m4,m0=data[:31],data[31:62],data[62:]
        assert len(m0)==31 and all(a+b==jobs for a,b in zip(m4,m0))
        if mode<2: assert all(x==jobs for x in m4)
        if mode in [2,3]: assert all(x==jobs for x in m0)
        rows.append(dict(case_id=case,mode=MODES[mode],jobs=jobs,total_samples=total,
                         samples_per_job=n,taps=taps,m4_kernel=k4,m0_kernel=k0,
                         ticks=ticks,m4_jobs=m4,m0_jobs=m0,
                         batch_us_median=statistics.median(ticks)*scale,
                         batch_us_min=min(ticks)*scale,batch_us_max=max(ticks)*scale))
    comparisons=[]
    for case in range(25):
        group=[r for r in rows if r['case_id']==case]
        serial=min(group[:2],key=lambda r:r['batch_us_median'])
        worker=min(group[2:4],key=lambda r:r['batch_us_median'])
        parallel=group[4]
        # Firmware and extraction must agree on both calibrated kernel choices.
        assert parallel['m4_kernel']==(0 if serial['mode']=='m4_scalar' else 1)
        assert parallel['m0_kernel']==(0 if worker['mode']=='m0_scalar' else 1)
        comparisons.append(dict(case_id=case,jobs=serial['jobs'],total_samples=serial['total_samples'],
                                samples_per_job=serial['samples_per_job'],taps=serial['taps'],
                                sequential_kernel=serial['mode'],m0_kernel=worker['mode'],
                                sequential_us=serial['batch_us_median'],greedy_us=parallel['batch_us_median'],
                                speedup=serial['batch_us_median']/parallel['batch_us_median'],
                                m4_jobs_median=statistics.median(parallel['m4_jobs']),
                                m0_jobs_median=statistics.median(parallel['m0_jobs'])))
    traces=[]
    width=one('gbench_trace_words')
    assert width==200 and len(raw['gbench_trace'])==25*width
    for offset in range(0,len(raw['gbench_trace']),width):
        case,jobs,elapsed,overlap,k4,k0,m4,m0,*timeline=raw['gbench_trace'][offset:offset+width]
        intervals=[]
        for j in range(jobs):
            n,k,owner,claims,start,end=timeline[j*6:j*6+6]
            assert owner<2 and claims==1 and start<=end<=elapsed
            intervals.append(dict(job_id=j,samples=n,taps=k,core=['M4','M0+'][owner],start_tick=start,end_tick=end))
        assert sum(r['core']=='M4' for r in intervals)==m4 and m4+m0==jobs
        by_core=[[r for r in intervals if r['core']==name] for name in ['M4','M0+']]
        for group in by_core:
            group.sort(key=lambda r:r['start_tick'])
            assert all(a['end_tick']<=b['start_tick'] for a,b in zip(group,group[1:]))
        actual=sum(max(0,min(a['end_tick'],b['end_tick'])-max(a['start_tick'],b['start_tick']))
                   for a in by_core[0] for b in by_core[1])
        assert actual==overlap and overlap<=elapsed
        traces.append(dict(case_id=case,elapsed_us=elapsed*scale,overlap_us=overlap*scale,
                           m4_jobs=m4,m0_jobs=m0,intervals=intervals))
    assert one('gbench_overlap_cases')==sum(t['overlap_us']>0 for t in traces)>0
    return rows,comparisons,traces


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    parser.add_argument('--clock-mhz',type=int,choices=[100,150],default=150)
    args=parser.parse_args()
    out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    configure_clock(args.clock_mhz)
    print('Building greedy comparison',args.clock_mhz,'MHz',flush=True)
    run(['make','build','-j4','APP_MODE=greedy','BENCH_OPT=-O2'],out/'build.log')
    print('Programming both cores',flush=True)
    run(['make','program','APP_MODE=greedy','BENCH_OPT=-O2'],out/'program.log',180)
    image=PROJECT/'build/app_combined.hex'
    (out/'app_combined.hex').write_bytes(image.read_bytes())
    symbols,shared=read_symbols()
    raw=inspect(out,symbols,shared)
    (out/'raw.json').write_text(json.dumps(raw,indent=2)+'\n')
    rows,comparisons,traces=summarize(raw,args.clock_mhz)
    result=dict(clock_mhz=args.clock_mhz,optimization='O2',collected_unix=time.time(),
                shared_address=hex(shared[0]),shared_bytes=shared[1],
                hex_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=PROJECT,text=True).strip(),
                source_diff=subprocess.check_output(['git','diff','HEAD'],cwd=PROJECT,text=True),
                control={k:v for k,v in raw.items() if k not in ['gbench_results','gbench_trace']},
                measurements=rows,comparisons=comparisons,traces=traces,
                notes=['First-available workers claim next job under a short hardware lock; no assignment cost model.',
                       'Sequential reference uses fastest tested direct M4 kernel on identical shared buffers.',
                       'Per-core kernel selection is calibrated outside timing; placement does not use predicted job costs.',
                       'Primary timings have per-job tracing disabled; separate traced batches prove overlap.',
                       'Parallel elapsed includes ready-list initialization, publication, IPC wake, claiming, computation, imbalance, and completion.',
                       'Preparation, calibration selection, reference checks, and output validation are outside batch timing.'])
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: 4,280 verified batches; 69,834 jobs; 3,875 timed batches',flush=True)
    for row in comparisons:
        if row['taps']==8 or row['case_id']==24:
            print(f"case {row['case_id']}: sequential={row['sequential_us']:.2f}us "
                  f"greedy={row['greedy_us']:.2f}us speedup={row['speedup']:.3f} "
                  f"jobs M4/M0+={row['m4_jobs_median']}/{row['m0_jobs_median']}",flush=True)
    print('Overlap confirmed in',sum(t['overlap_us']>0 for t in traces),'of 25 traced workloads',flush=True)


if __name__=='__main__':
    main()
