import asyncio
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('can_monitor',ROOT/'diagnostics/mercedes_can_monitor.py')
m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)

class Tx:properties=['write']
class FakeClient:
    def __init__(self,mode='frames'):
        self.is_connected=True;self.receiver=None;self.writes=[];self.protocol=6;self.mode=mode;self.active=False;self.reject=None
    def emit(self,data):self.receiver(None,data)
    async def write_gatt_char(self,tx,data,response=True):
        self.writes.append(bytes(data));cmd=data.decode().strip()
        if self.mode=='no_initial_prompt' and cmd=='ATI':return
        if data==b'x':
            self.active=False
            if self.mode!='stop_silent':self.emit(b'STOPPED\r>')
            return
        if self.active and cmd=='ATI':self.active=False;self.emit(b'STOPPED\r>?\r>');return
        if cmd==self.reject:self.emit(b'?\r>');return
        if cmd.startswith('ATTP'):self.protocol=int(cmd[-1])
        if cmd=='ATMA':
            self.active=True
            if self.mode=='disconnect':self.is_connected=False;return
            if self.mode in ('silent','stop_silent'):return
            if self.mode=='overflow':
                for _ in range(130):self.emit(b'123 2 00 01\r')
                return
            head='123' if self.protocol==6 else '18FF1234'
            text=f'{head} 2 00 01\r{head} 2 00 02\r'
            if self.mode=='buffer_full':text+='BUFFER FULL\r>';self.active=False
            elif self.mode=='early_prompt':text='STOPPED\r>';self.active=False
            elif self.mode=='malformed':text='SOME ADAPTER OUTPUT\r'
            elif self.mode=='no_dlc':text=f'{head} 00 01\r'
            elif self.mode=='split':
                self.emit(text[:8].encode());self.emit(text[8:].encode());return
            self.emit(text.encode());return
        value='ELM327 TEST' if cmd in ('ATI','ATZ') else str(self.protocol) if cmd=='ATDPN' else 'CAN 500' if cmd=='ATDP' else 'T:00 R:00 F:500' if cmd=='ATCS' else '12.6V' if cmd=='ATRV' else 'OK'
        self.emit((value+'\r>').encode())
    async def disconnect(self):self.is_connected=False

def args(folder,*extra):
    a=m.parse_args(['--output-dir',str(Path(folder)/'capture'),'--no-tui','--no-keys',*extra]);a.seconds=.025;a.stop_timeout=.025
    return a

def factory(client):
    async def connect(args,log):
        client.is_connected=True;base=m.obd.ElmSession(client,Tx(),log);client.receiver=base.receive;return client,base
    return connect

class Parsing(unittest.TestCase):
    def test_can_raw_dlc_and_payload_not_isotp(self):
        f=m.parse_frame('7E8 8 04 41 0C 0B 88 00 00 00',11)
        self.assertEqual(f.key,'7E8');self.assertEqual(f.data,bytes.fromhex('04410C0B88000000'))
        self.assertEqual(m.parse_frame('123200FF',11).data,b'\0\xff')
        self.assertEqual(m.parse_frame('18DAF110 3 01 02 03',29).can_id,0x18daf110)
        self.assertEqual(m.parse_frame('1230',11).data,b'')
    def test_missing_dlc_reported(self):
        f=m.parse_frame('123 03 41 0D 00',11);self.assertTrue(f.format_note);self.assertEqual(f.data,bytes.fromhex('03410D00'))
        self.assertFalse(m.parse_frame('12303410D00',11,False).format_note)
    def test_invalid_frames(self):
        for text,bits in [('FFF80000000000000000',11),('FFFFFFFF80000000000000000',29),('1239',11),('1234AABB',11),('123',11),('ATMA',11),('BUFFER FULL',11)]:
            with self.subTest(text=text):self.assertIsNone(m.parse_frame(text,bits))
    def test_framer_fragmentation_and_prompt(self):
        f=m.LineFramer();self.assertEqual(f.feed(b'123 2 0'),[])
        self.assertEqual(f.feed(b'0 FF\r\n>'),[('line','123 2 00 FF'),('prompt','>')])
        self.assertEqual(f.feed(b'STOPPED>'),[('line','STOPPED'),('prompt','>')])
    def test_framer_overlong_is_bounded(self):
        f=m.LineFramer(8);self.assertIn(('oversize','line exceeded limit'),f.feed(b'A'*100))
        self.assertEqual(f.feed(b'\r>'),[('prompt','>')])
    def test_stats_batched_timing_changes_and_counter(self):
        s=m.IdStats()
        for i in range(20):s.add(bytes((i,0xFF)),1000+i*1000000)
        s.add(bytes((20,0xFF)),s.last_ns)
        r=s.report(2)
        self.assertEqual(r['count'],21);self.assertEqual(r['payload_changes'],20)
        self.assertTrue(r['bytes'][0]['incrementing_counter_candidate']);self.assertEqual(r['bytes'][1]['variable_bit_mask'],'00')
        self.assertEqual(r['host_timing']['zero_interval_pairs'],1)
        self.assertLessEqual(len(r['payload_examples_first_8']),8)
    def test_status_parser(self):
        r=m.parse_can_status('T:0A R:02 F:500');self.assertEqual(r['tx_errors'],10);self.assertEqual(r['rx_errors'],2)
        self.assertTrue(m.parse_can_status('T:OFF R:00')['tx_errors_off']);self.assertNotIn('tx_errors',m.parse_can_status('?'))
    def test_command_allowlist(self):
        for cmd in ['0100','010C','222002','ATCSM0','ATSP0','ATFI','ATRTR','ATSH7E2','12345678']:
            self.assertFalse(m.allowed_command(cmd))
        for cmd in ['ATCSM1','ATMA','ATTP6','ATTP7','ATCF00000000','ATCM000','ATCRA7E8']:self.assertTrue(m.allowed_command(cmd))
    def test_cli_bounds_and_filter(self):
        with tempfile.TemporaryDirectory() as d:
            a=args(d);self.assertEqual(a.protocols,[6,7])
            a=args(d,'--filter-id','18DAF110');self.assertEqual(a.protocols,[7])
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):args(d,'--protocol','6','--filter-id','18DAF110')
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):args(d,'--protocol','5')
    def test_conclusions_do_not_assert_gateway(self):
        phase=m.new_phase(1,6);self.assertIn('配置未完成',m.classify_phase(phase))
        phase['configuration_verified']=True;phase['end_reason']='duration_elapsed';self.assertIn('不能推断',m.classify_phase(phase))
        phase['unparsed_lines']=1;self.assertIn('原始格式',m.classify_phase(phase))
        phase['data_loss_possible']=True;self.assertIn('不完整',m.classify_phase(phase))

class Capture(unittest.IsolatedAsyncioTestCase):
    async def execute(self,folder,mode='frames',*extra):
        client=FakeClient(mode);a=args(folder,*extra)
        with patch.object(m.obd,'connect',factory(client)),contextlib.redirect_stdout(io.StringIO()):code=await m.run(a)
        return code,client,json.loads((a.output_dir/'summary.json').read_text()),a
    async def test_two_formats_and_report_exports(self):
        with tempfile.TemporaryDirectory() as d:
            code,client,report,a=await self.execute(d)
            self.assertEqual(code,0);self.assertTrue(report['completed']);self.assertFalse(client.is_connected)
            self.assertEqual([p['id_bits'] for p in report['phases']],[11,29])
            self.assertEqual([p['frames'] for p in report['phases']],[2,2])
            for file in ['raw.jsonl','frames.csv','can_ids.csv','markers.csv','summary.json','report.md']:self.assertTrue((a.output_dir/file).exists())
            for data in client.writes:
                cmd=data.decode().strip()
                self.assertTrue(data==b'x' or m.allowed_command(cmd),repr(data))
            self.assertNotIn(b'0100\r',client.writes);self.assertNotIn(b'ATCSM0\r',client.writes)
    async def test_silent_bus_is_completed_but_not_supported_claim(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,_=await self.execute(d,'silent','--protocol','6');self.assertEqual(code,2);self.assertTrue(r['completed'])
            self.assertEqual(r['phases'][0]['frames'],0);self.assertIn('不能推断',r['phases'][0]['conclusion'])
    async def test_buffer_full_marks_loss(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,_=await self.execute(d,'buffer_full','--protocol','6');self.assertEqual(code,2)
            phase=r['phases'][0];self.assertTrue(phase['data_loss_possible']);self.assertFalse(phase['completed'])
    async def test_fragmented_frame_stream(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,_=await self.execute(d,'split','--protocol','6');self.assertEqual(code,0);self.assertEqual(r['phases'][0]['frames'],2)
    async def test_queue_overflow_does_not_decode_corrupted_stream(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,_=await self.execute(d,'overflow','--protocol','6','--queue-size','128');self.assertEqual(code,2)
            phase=r['phases'][0];self.assertTrue(phase['data_loss_possible']);self.assertGreater(phase['queue_dropped'],0);self.assertEqual(phase['frames'],0)
    async def test_stop_without_prompt_prevents_followup_queries(self):
        with tempfile.TemporaryDirectory() as d:
            code,client,r,_=await self.execute(d,'stop_silent','--protocol','6');self.assertEqual(code,2)
            self.assertFalse(r['phases'][0]['stop_synchronized']);self.assertFalse(client.is_connected)
    async def test_silent_configuration_failure_never_starts_monitor(self):
        with tempfile.TemporaryDirectory() as d:
            client=FakeClient();client.reject='ATCSM1';a=args(d)
            with patch.object(m.obd,'connect',factory(client)),contextlib.redirect_stdout(io.StringIO()):code=await m.run(a)
            self.assertEqual(code,2);self.assertNotIn(b'ATMA\r',client.writes);self.assertFalse(client.is_connected)
            r=json.loads((a.output_dir/'summary.json').read_text());self.assertEqual(r['end_reason'],'safety_configuration_failed')
    async def test_initial_prompt_missing_never_starts_monitor(self):
        with tempfile.TemporaryDirectory() as d:
            code,client,r,_=await self.execute(d,'no_initial_prompt','--protocol','6')
            self.assertEqual(code,2);self.assertNotIn(b'ATMA\r',client.writes);self.assertFalse(r['completed'])
    async def test_resume_from_preexisting_monitor(self):
        with tempfile.TemporaryDirectory() as d:
            client=FakeClient();client.active=True;a=args(d,'--protocol','6')
            with patch.object(m.obd,'connect',factory(client)),contextlib.redirect_stdout(io.StringIO()):code=await m.run(a)
            self.assertEqual(code,0);self.assertEqual(client.writes[0],b'ATI\r')
    async def test_filter_is_adapter_only(self):
        with tempfile.TemporaryDirectory() as d:
            _,client,r,_=await self.execute(d,'frames','--protocol','6','--filter-id','123')
            self.assertIn(b'ATCRA123\r',client.writes);self.assertNotIn(b'ATSH123\r',client.writes)
    async def test_max_frames_limits_inside_notification(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,_=await self.execute(d,'frames','--protocol','6','--max-frames','1')
            self.assertEqual(code,0);self.assertEqual(r['phases'][0]['frames'],1);self.assertEqual(r['phases'][0]['end_reason'],'max_frames')
    async def test_early_prompt_is_not_full_window(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,_=await self.execute(d,'early_prompt','--protocol','6')
            self.assertEqual(code,2);self.assertFalse(r['completed']);self.assertIn('未完整结束',r['phases'][0]['conclusion'])
    async def test_missing_dlc_is_visible_in_report(self):
        with tempfile.TemporaryDirectory() as d:
            code,_,r,a=await self.execute(d,'no_dlc','--protocol','6')
            self.assertEqual(code,0);self.assertEqual(r['phases'][0]['missing_dlc_frames'],1)
            self.assertIn('格式提醒',(a.output_dir/'report.md').read_text())
    async def test_ble_disconnect_saved_as_partial(self):
        with tempfile.TemporaryDirectory() as d:
            code,client,r,_=await self.execute(d,'disconnect','--protocol','6')
            self.assertEqual(code,2);self.assertFalse(client.is_connected);self.assertFalse(r['completed'])
    async def test_cancellation_stops_monitor_and_writes_partial(self):
        with tempfile.TemporaryDirectory() as d:
            client=FakeClient('silent');a=args(d,'--protocol','6');a.seconds=10
            with patch.object(m.obd,'connect',factory(client)),contextlib.redirect_stdout(io.StringIO()):
                task=asyncio.create_task(m.run(a))
                for _ in range(100):
                    if client.active:break
                    await asyncio.sleep(.01)
                self.assertTrue(client.active);task.cancel()
                with self.assertRaises(asyncio.CancelledError):await task
            self.assertIn(b'x',client.writes);self.assertFalse(client.is_connected)
            r=json.loads((a.output_dir/'summary.json').read_text());self.assertFalse(r['completed']);self.assertEqual(r['end_reason'],'user_stop')
    async def test_unparsed_output_is_not_declared_quiet(self):
        with tempfile.TemporaryDirectory() as d:
            _,_,r,_=await self.execute(d,'malformed','--protocol','6');self.assertIn('原始格式',r['phases'][0]['conclusion'])

if __name__=='__main__':unittest.main()
