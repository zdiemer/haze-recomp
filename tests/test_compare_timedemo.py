from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from diagnostics.compare_timedemo import agreement, load_run, summarize


def write_run(folder,name,frame_ms,draws,flip_base=500,escape_at=None,hash_from=None):
    prefix=Path(folder)/name
    rows=['# qpc_frequency=1000 step_us=6944 settle=2 sweep=6','boundary,gameplay,virtual_us,qpc,escaped_us,flips']
    for boundary in range(5): rows.append(f'{boundary},-1,{boundary*6944},{boundary*10},0,{boundary}')
    qpc=100
    for g in range(len(frame_ms)+1):
        rows.append(f'{g+5},{g},{(g+5)*6944},{qpc},{250 if g==escape_at else 0},{flip_base+g}')
        if g<len(frame_ms): qpc+=frame_ms[g]
    Path(f'{prefix}-timeline.csv').write_text('\n'.join(rows)+'\n')
    prints=['frame,flips,hash,constants,draws']
    for g,count in enumerate(draws):
        digest='B' if hash_from is not None and g>=hash_from else 'A'
        prints.append(f'{g},{flip_base+g+1},{digest}{g:015X},100,{count}')
    Path(f'{prefix}-fingerprint.csv').write_text('\n'.join(prints)+'\n')
    return prefix


class CompareTimedemo(unittest.TestCase):
    def test_summary_and_agreement(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            reference=load_run(write_run(folder,'a',[10,10,12,10,10,10,14,10],[5,5,5,6,6,6,6,6,6]))
            summary=summarize(reference)
            # The sweep covers gameplay frames 2..7: intervals 12,10,10,10,14,10.
            self.assertEqual(summary['frames'],6)
            self.assertAlmostEqual(summary['mean_ms'],11.0,places=3)
            self.assertEqual(summary['max_ms'],14.0)
            self.assertEqual(summary['escaped_frames'],0)

            # Same scene under a different flip base, diverging at frame 4.
            other=load_run(write_run(folder,'b',[9]*8,[5,5,5,6,7,7,7,7,7],flip_base=900,escape_at=3,hash_from=4))
            self.assertEqual(summarize(other)['first_escape'],3)
            result=agreement(reference,other,4)
            self.assertEqual(result['flip_offset'],0)
            self.assertEqual(result['identical_until'],4)
            self.assertEqual([w['equal_draws'] for w in result['windows']],[4,0])
            self.assertEqual(result['windows'][1]['draw_difference_percent'],round(100*(7-6)/6,2))

    def test_empty_sweep_is_an_error(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            run=load_run(write_run(folder,'a',[],[5]))
            with self.assertRaises(ValueError): summarize(run)


if __name__=='__main__':
    unittest.main()
