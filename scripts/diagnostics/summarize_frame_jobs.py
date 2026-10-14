"""Correlate renderer draw windows with recurring SPU jobs on the same QPC clock.

This is a diagnostic correlation, not proof that one job causes a frame wait.
Use a visually verified interval. Do not mix job-logging runs into FPS A/B tests.
"""
import argparse,bisect,csv,json,re,statistics
from pathlib import Path

def distribution(values):
    if not values: return None
    ordered=sorted(values)
    return dict(count=len(values),mean=statistics.mean(values),
                p50=statistics.median(values),p95=ordered[int((len(ordered)-1)*.95)],max=max(values))

def summarize(renderer,log,start_frame,frames,image):
    rows=[]
    with Path(renderer).open(newline='') as stream:
        for raw in csv.DictReader(stream):
            try: row={k:float(v) for k,v in raw.items()}
            except (ValueError,TypeError): continue
            if row['frame']>=start_frame: rows.append(row)
    rows=rows[:frames]
    if not rows: raise ValueError('No complete frames in requested interval')
    frequency=rows[0]['qpc_frequency']
    if frequency<=0 or any(r['qpc_frequency']!=frequency for r in rows):
        raise ValueError('QPC frequency missing or changed')
    jobs=[]
    for line in Path(log).read_text(errors='replace').splitlines():
        if not line.startswith('[spu-job-time] '): continue
        fields={k:int(v,16 if k=='taskset' else 10) for k,v in re.findall(r'(\w+)=(\w+)',line)}
        if not {'image','execute_qpc','end_qpc'}<=fields.keys(): continue
        if fields['image']==image: jobs.append(fields)
    jobs.sort(key=lambda j:j['end_qpc'])
    ends=[j['end_qpc'] for j in jobs]
    low=rows[0]['qpc']-rows[0]['dt_ms']*frequency/1000;high=rows[-1]['qpc']
    selected=[j for j in jobs if low<=j['execute_qpc'] and j['end_qpc']<=high]
    before=[];after=[];span=[];inside_gap=[];end_to_draw=[];end_to_present=[];counts=[];per_frame=[]
    ms=lambda ticks:ticks*1000/frequency
    for r in rows:
        begin=r['qpc']-r['dt_ms']*frequency/1000
        first,last=r['draw_begin_qpc'],r['draw_end_qpc']
        if not first or not last: continue
        before.append(ms(first-begin));after.append(ms(r['qpc']-last));span.append(ms(last-first))
        inside_gap.append(ms(last-first)-r['draw_cpu_ms'])
        i=bisect.bisect_right(ends,first)-1
        if i>=0: end_to_draw.append(ms(first-ends[i]))
        i=bisect.bisect_right(ends,r['qpc'])-1
        if i>=0: end_to_present.append(ms(r['qpc']-ends[i]))
        completed=[j for j in jobs if begin<j['end_qpc']<=r['qpc']]
        counts.append(len(completed))
        per_frame.append(dict(frame=int(r['frame']),dt_ms=r['dt_ms'],before_first_draw_ms=before[-1],
            draw_span_ms=span[-1],draw_cpu_ms=r['draw_cpu_ms'],after_last_draw_ms=after[-1],
            completed_jobs=[dict(duration_ms=ms(j['end_qpc']-j['execute_qpc']),
                start_from_previous_present_ms=ms(j['execute_qpc']-begin),
                end_from_previous_present_ms=ms(j['end_qpc']-begin)) for j in completed]))
    return dict(first_frame=int(rows[0]['frame']),last_frame=int(rows[-1]['frame']),frames=len(rows),image=image,
        job_wall_ms=distribution([ms(j['end_qpc']-j['execute_qpc']) for j in selected]),
        before_first_draw_ms=distribution(before),draw_span_ms=distribution(span),
        within_draw_span_outside_sink_ms=distribution(inside_gap),after_last_draw_ms=distribution(after),
        last_job_end_to_first_draw_ms=distribution(end_to_draw),last_job_end_to_present_ms=distribution(end_to_present),
        completed_jobs_per_frame=distribution(counts),per_frame=per_frame,note=__doc__)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('renderer',type=Path);p.add_argument('log',type=Path)
    p.add_argument('--start-frame',type=int,required=True);p.add_argument('--frames',type=int,default=300)
    p.add_argument('--image',type=int,default=40);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.frames<1 or a.start_frame<0: p.error('Use positive frames and a nonnegative start-frame')
    result=summarize(a.renderer,a.log,a.start_frame,a.frames,a.image)
    a.output.write_text(json.dumps(result,indent=2))
    print(json.dumps({k:v for k,v in result.items() if k!='per_frame'},indent=2))
