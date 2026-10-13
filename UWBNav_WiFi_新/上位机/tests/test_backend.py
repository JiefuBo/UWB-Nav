import json
import math
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from server import FrameParser, Store, crc16, default_config, layout_from_distances, solve_position, validate_config
from simulate import frame


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.s = Store(Path(self.temp.name)/'config.json')
    def tearDown(self):
        self.temp.cleanup()
    def packet(self, seq, data, device='test', identity='10', role='tag', session=1):
        return dict(v=1, device=device, session=session, seq=seq, role=role,
                    self_id=identity, records=1, hex=data.hex())
    def ingest(self, p, t=100):
        self.s.ingest(p, ('127.0.0.1', 1234), t)
    def test_live_samples_and_crc(self):
        p = FrameParser()
        for raw, expected in [('45910100010000000300c536af', 1.97),
                              ('45910100010000000300c60434', 1.98),
                              ('45910100010000000300c715bd', 1.99)]:
            f = p.feed(bytes.fromhex(raw))[0]
            self.assertEqual(f['peer_role'], 'anchor')
            self.assertEqual(f['rows'], [{'peer_id':'3', 'meters':expected}])
    def test_split_concatenated_noise_and_bad_crc(self):
        p = FrameParser(); f = frame('1', 2.5)
        self.assertEqual(p.feed(b'AT\r\n'+f[:6]), [])
        self.assertEqual(len(p.feed(f[6:]+f)), 2)
        bad = bytearray(f); bad[10] ^= 1
        self.assertEqual(len(p.feed(bad+f)), 1)
        self.assertGreater(p.bad_crc, 0)
    def test_two_record_manual_example(self):
        f = FrameParser(2).feed(bytes.fromhex('459101000001020304010905060708021008a0'))[0]
        self.assertEqual(f['rows'][1]['meters'], 5.28)
    def test_geometry_exact_and_height(self):
        aa = default_config()['anchors']
        for z in (0, 1.5):
            rr = [math.sqrt((3-a['x'])**2+(7-a['y'])**2+(z-a['z'])**2) for a in aa]
            p = solve_position(aa, rr, z)
            self.assertAlmostEqual(p['x'], 3, places=6)
            self.assertAlmostEqual(p['y'], 7, places=6)
    def test_noisy_position(self):
        aa = default_config()['anchors']
        rr = [math.hypot(3-a['x'], 7-a['y'])+e for a,e in zip(aa,[.05,-.07,.03,.02])]
        p = solve_position(aa, rr)
        self.assertLess(math.hypot(p['x']-3,p['y']-7), .15)
    def test_stale_and_partial(self):
        for i,a in enumerate(default_config()['anchors']):
            self.ingest(self.packet(i,frame(a['id'],math.hypot(3-a['x'],7-a['y']))))
        self.assertIsNotNone(self.s.snapshot(101)['tags'][0]['position'])
        self.assertIsNone(self.s.snapshot(105)['tags'][0]['position'])
    def test_missing_anchor(self):
        self.ingest(self.packet(0,frame('1',2)))
        self.assertIsNone(self.s.snapshot(100)['tags'][0]['position'])
    def test_gap_does_not_join_frame(self):
        f=frame('1',2)
        self.ingest(self.packet(0,f[:6]))
        self.ingest(self.packet(2,f[6:]))
        self.assertEqual(self.s.frame_count,0)
        self.ingest(self.packet(3,f))
        self.assertEqual(self.s.frame_count,1)
        self.assertEqual(self.s.snapshot(100)['devices'][0]['gaps'],1)
    def test_duplicate_and_out_of_order(self):
        self.ingest(self.packet(2,frame('1',2)))
        self.ingest(self.packet(2,frame('1',3)))
        self.ingest(self.packet(1,frame('1',4)))
        self.assertEqual(self.s.snapshot(100)['tags'][0]['ranges'][0]['meters'],2)
    def test_restart_and_multiple_tags(self):
        self.ingest(self.packet(2,frame('1',2)))
        self.ingest(self.packet(0,frame('1',3),session=2))
        self.ingest(self.packet(0,frame('1',4),device='test2',identity='11'))
        tags=self.s.snapshot(100)['tags']
        self.assertEqual(len(tags),2)
        self.assertEqual(tags[0]['ranges'][0]['meters'],3)
    def test_duplicate_identity_rejected(self):
        self.ingest(self.packet(0,frame('1',2)))
        with self.assertRaises(ValueError):
            self.ingest(self.packet(0,frame('1',4),device='second'))
    def test_anchor_uplink_mapping(self):
        raw = bytearray(frame('10',2)); raw[4]=0; raw[-2:]=crc16(raw[:-2]).to_bytes(2,'big')
        self.ingest(self.packet(0,raw,identity='1',role='anchor'))
        self.assertEqual(self.s.snapshot(100)['tags'][0]['id'],'10')
        self.assertEqual(self.s.snapshot(100)['tags'][0]['ranges'][0]['meters'],2)
    def test_layout_and_inconsistency(self):
        d=dict(AB=10,BC=20,CD=10,DA=20,AC=math.sqrt(500),BD=math.sqrt(500))
        result=layout_from_distances(d)
        self.assertAlmostEqual(result['coordinates'][2]['x'],10)
        self.assertAlmostEqual(result['coordinates'][2]['y'],-20)
        self.assertEqual(result['warnings'],[])
        self.assertTrue(layout_from_distances(dict(d,CD=15))['warnings'])
        with self.assertRaises(ValueError): layout_from_distances(dict(d,AC=1))
    def test_invalid_config_and_persistence(self):
        c=default_config(); c['anchors'][1]['id']='1'
        with self.assertRaises(ValueError): validate_config(c)
        c=default_config()
        for a in c['anchors']: a['y']=0
        with self.assertRaises(ValueError): validate_config(c)
        c=default_config(); c['pois']=[dict(name='厕所',kind='厕所',x=2,y=4)]
        self.s.save(c)
        self.assertEqual(Store(self.s.path).config['pois'][0]['name'],'厕所')
    def test_time_span(self):
        for i,a in enumerate(default_config()['anchors']):
            self.ingest(self.packet(i,frame(a['id'],5)),100+i)
        self.assertEqual(self.s.snapshot(103)['tags'][0]['status'],'四距离时间跨度过大')


if __name__=='__main__': unittest.main()
