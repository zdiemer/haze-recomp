import copy
import csv
import math
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from validate_camera_sweep import validate_camera_sweep


class CameraSweepEvidence(unittest.TestCase):
    def test_actual_angles_and_invalid_evidence(self):
        windows=dict(qpc_frequency=1000,blocks=[dict(start_qpc=1000,end_qpc=25000,camera_token=100)])
        rows=[]
        for tick in range(0,26001,125):
            wanted=3.1+math.pi/6*math.sin(tick/1000*math.pi/2)
            rows.append(dict(qpc=tick,qpc_frequency=1000,token=100,motor=123,
                phase_s=tick/1000,wanted_yaw=wanted,actual_yaw=math.remainder(wanted,2*math.pi),
                wanted_pitch=.1,actual_pitch=.1))
        with tempfile.TemporaryDirectory(dir=ROOT/'local') as folder:
            path=Path(folder)/'camera.csv'
            def run(data):
                with path.open('w',newline='') as stream:
                    writer=csv.DictWriter(stream,fieldnames=rows[0].keys())
                    writer.writeheader();writer.writerows(data)
                return validate_camera_sweep(path,windows)
            self.assertEqual(run(rows)['blocks'][0]['phase_bins'],16)
            bad_sets=[[],rows[10:],rows[:-10],rows[::-1]]
            for key,value in [('actual_yaw',0),('actual_pitch',0),('token',101),
                              ('motor',124),('phase_s',0),('qpc_frequency',2000),
                              ('wanted_yaw',float('nan'))]:
                data=copy.deepcopy(rows);data[100][key]=value;bad_sets.append(data)
            for data in bad_sets:
                with self.subTest(data=data[100:101]),self.assertRaises(ValueError):run(data)
            shifted=copy.deepcopy(windows)
            shifted['blocks'].append(dict(start_qpc=1000,end_qpc=25000,camera_token=101))
            run(rows)  # Restore valid data: reject the second block's token specifically.
            with self.assertRaises(ValueError):validate_camera_sweep(path,shifted)


if __name__=='__main__':unittest.main()
