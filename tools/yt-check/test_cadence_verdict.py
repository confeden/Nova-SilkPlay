import copy
import unittest
from cadence_verdict import cadence_errors

class CadenceVerdictTest(unittest.TestCase):
    def setUp(self):
        self.row={'before':{'paused':False,'t':1.3,'total':120,'dropped':0},
                  'after':{'paused':False,'t':3.5,'total':252,'dropped':0},
                  'capture_seconds':2,
                  'overlay_visibility':{'before':True,'after':True,'samples':40,'hidden_samples':0},
                  'dda':{'frames':329,'recorder_missed':0,'distinct_pictures_per_s':164.8}}
    def test_complete_visible_capture(self):
        self.assertEqual(cadence_errors(self.row),[])
    def test_source_only_is_rejected_even_at_high_picture_rate(self):
        self.row['overlay_visibility']['before']=False
        self.row['overlay_visibility']['after']=False
        self.row['dda']['distinct_pictures_per_s']=165
        self.assertTrue(cadence_errors(self.row))
    def test_transient_hide_is_rejected(self):
        self.row['overlay_visibility']['hidden_samples']=1
        self.assertTrue(cadence_errors(self.row))
    def test_missing_observer_is_rejected(self):
        self.row['overlay_visibility']['samples']=0
        self.assertTrue(cadence_errors(self.row))
    def test_recorder_and_source_drops_are_rejected(self):
        for target,key in [('dda','recorder_missed'),('after','dropped')]:
            row=copy.deepcopy(self.row);row[target][key]=1
            self.assertTrue(cadence_errors(row))
    def test_stalled_or_paused_source_is_rejected(self):
        for change in [{'paused':True},{'t':1.4},{'total':120}]:
            row=copy.deepcopy(self.row);row['after'].update(change)
            self.assertTrue(cadence_errors(row))

if __name__=='__main__':unittest.main()
