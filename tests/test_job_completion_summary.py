import csv
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from summarize_job_completion import read_rows,summarize,PHASES


def row(begin,end,depth=0):
    return dict(qpc_frequency=1000,begin_qpc=begin,end_qpc=end,job=1,caller=16,depth=depth,
                **{p+'_ticks':0 for p in PHASES},**{p+'_calls':0 for p in PHASES})


class Summary(unittest.TestCase):
    def read(self,rows):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'rows.csv'
            with path.open('w',newline='') as f:
                w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
            return read_rows(path)

    def test_nested_and_boundary_loops(self):
        rows=[row(0,15),row(22,24,1),row(20,30),row(35,50)]
        rows[2]['predicate_ticks']=4
        result=summarize(self.read(rows),10,40)
        self.assertEqual(result['complete']['loops'],1)
        self.assertEqual(result['complete']['elapsed_ms'],10)
        self.assertEqual(result['complete']['predicate_ms'],4)
        self.assertEqual(result['boundary_loops'],2)
        self.assertEqual(result['boundary_overlap_ms'],10)

    def test_reject_bad_accounting(self):
        for key,value in [('end_qpc',-1),('qpc_frequency',0),('job',512),('delay_ticks',11),('delay_calls',-1)]:
            r=row(0,10);r[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.read([r])

    def test_overlap_order_frequency(self):
        for rows in ([row(0,10),row(5,20)],[row(20,30),row(0,10)],
                     [row(0,10),dict(row(20,30),qpc_frequency=10)]):
            with self.assertRaises(ValueError):self.read(rows)

    def test_missing_coverage(self):
        with self.assertRaises(ValueError):summarize([row(10,20)],0,30)
        with self.assertRaises(ValueError):summarize([row(0,30)],20,10)

if __name__=='__main__':unittest.main()
