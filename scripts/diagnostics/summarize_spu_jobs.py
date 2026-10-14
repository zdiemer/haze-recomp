"""Summarize opt-in SPU_JOB_TIMING cycles for completed jobs in a frame interval.

CPU cycles distinguish work from scheduling waits but are not milliseconds.
The ABI run includes translated code, runtime helpers and context initialization.
Persistent jobs that do not return are absent; use instruction sampling too.
Per-job logging adds overhead, so this is a separate diagnostic, not an FPS test.
Thread creation is recorded separately in the submitting thread; worker-cycle
totals exclude it. Native execution includes runtime helpers and guest work.
"""
import argparse
import collections
import csv
import ctypes
import json
from pathlib import Path
import re
import sys


def summarize(log, renderer, first, last, frequency):
    frames={}
    with Path(renderer).open(newline='') as f:
        for row in csv.DictReader(f):
            try: frames[int(row['frame'])]=int(row['qpc'])
            except (ValueError,TypeError,KeyError): continue
    begin,end=frames[first],frames[last]
    jobs=collections.defaultdict(collections.Counter)
    with Path(log).open(encoding='utf-8',errors='replace') as f:
        for line in f:
            if not line.startswith(('[spu-job-time]','[spu-thread-time]','[spu-dispatch-time]')): continue
            fields=dict(re.findall(r'(\w+)=(\w+)',line))
            if 'end_qpc' not in fields: continue
            qpc=int(fields['end_qpc'])
            if not begin <= qpc <= end: continue
            row=jobs[int(fields['image'])]
            if line.startswith('[spu-job-time]'):
                # Exclude a long-lived task that began before this interval.
                if int(fields['begin_qpc']) < begin: continue
                row['jobs']+=1
                row['setup_cycles']+=int(fields['setup_cycles'])
                row['abi_run_cycles']+=int(fields['run_cycles'])
                row['setup_wall_ticks']+=int(fields['execute_qpc'])-int(fields['begin_qpc'])
                row['abi_wall_ticks']+=qpc-int(fields['execute_qpc'])
                for key in ('abi_prepare_cycles','native_cycles','abi_finish_cycles'):
                    if key in fields: row[key]+=int(fields[key])
            elif line.startswith('[spu-thread-time]'):
                row['threads']+=1
                for key in ('headroom_cycles','job_cycles','cleanup_cycles'):
                    row[key]+=int(fields[key])
            else:
                if int(fields['begin_qpc']) < begin: continue
                row['dispatches']+=1
                row['create_cycles']+=int(fields['create_cycles'])
                row['create_wall_ticks']+=qpc-int(fields['begin_qpc'])
    output=[]
    for image,row in sorted(jobs.items(),key=lambda p:p[1]['abi_run_cycles'],reverse=True):
        abi=row['setup_cycles']+row['abi_run_cycles']
        thread=row['headroom_cycles']+row['job_cycles']+row['cleanup_cycles']
        output.append(dict(image=image,**row,
            setup_share_percent=100*row['setup_cycles']/abi if abi else None,
            wrapper_share_percent=100*(row['headroom_cycles']+row['cleanup_cycles'])/thread if thread else None,
            mean_setup_ms=row['setup_wall_ticks']*1000/frequency/row['jobs'] if row['jobs'] else None,
            mean_abi_wall_ms=row['abi_wall_ticks']*1000/frequency/row['jobs'] if row['jobs'] else None))
    return dict(first_frame=first,last_frame=last,elapsed_ms=(end-begin)*1000/frequency,
                qpc_frequency=frequency,images=output,note=__doc__)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log',type=Path)
    p.add_argument('renderer',type=Path)
    p.add_argument('--first',type=int,required=True)
    p.add_argument('--last',type=int,required=True)
    p.add_argument('--frequency',type=int,help='QPC frequency from the capture machine')
    a=p.parse_args()
    if a.last<=a.first: p.error('last must exceed first')
    if a.frequency is None:
        if sys.platform!='win32': p.error('Supply the capture machine QPC frequency')
        value=ctypes.c_longlong()
        ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(value))
        a.frequency=value.value
    if a.frequency<=0: p.error('QPC frequency must be positive')
    print(json.dumps(summarize(a.log,a.renderer,a.first,a.last,a.frequency),indent=2))
