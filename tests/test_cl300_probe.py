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
from test_diagnostics import Fake,wire
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cl300_probe',ROOT/'diagnostics/cl300_pid_test.py')
p=importlib.util.module_from_spec(spec);sys.modules[spec.name]=p;spec.loader.exec_module(p)

class Bike(Fake):
    def __init__(self,protocol=7):super().__init__(protocol);self.synchronized=True;self.deadline=None
    async def command(self,cmd,timeout=1.8):
        data={'0100':'410008180001','0120':'412080020000','012F':'412F80','0121':'41210001'}.get(cmd)
        if data:
            self.commands.append(cmd)
            if self.asleep:return 'NO DATA'
            return wire(bytes.fromhex(data),self.protocol) if self.headers else data
        return await super().command(cmd,timeout)

class Client:
    is_connected=True
    async def disconnect(self):self.is_connected=False

class ProbeTests(unittest.IsolatedAsyncioTestCase):
    def args(self,*extra):return p.parse_args(['--samples','1','--no-identity','--protocol','auto',*extra])
    async def test_screenshot_profile_uses_kwp_fast_without_can(self):
        args=p.parse_args(['--samples','1','--no-identity']);fake=Bike(5);state=p.ProbeState();probe=p.PidProbe(fake,args,state)
        await probe.discover();await probe.sample()
        self.assertEqual(state.protocol,5);self.assertIn('ATTP5',fake.commands);self.assertNotIn('ATSP0',fake.commands)
        self.assertFalse(any(cmd.startswith(('22','ATSH','ATCRA','ATCAF','ATCFC')) for cmd in fake.commands))
        self.assertEqual(state.rows[12]['last_responses'][0]['value'],738)
    async def test_intake_temperature_capture_and_engine_load(self):
        state=p.ProbeState();fake=Bike(5);original=fake.command
        async def command(cmd,timeout=3):
            if cmd=='0100':
                data=bytes.fromhex('410010020000')
                return wire(data,5) if fake.headers else data.hex()
            if cmd=='010F':return '83 F1 10 41 0F 51 25 \r\r'
            if cmd=='0104':return wire(bytes.fromhex('410480'),5)
            return await original(cmd,timeout)
        fake.command=command
        args=p.parse_args(['--pids','0F,04','--samples','1','--no-identity'])
        probe=p.PidProbe(fake,args,state);await probe.discover();await probe.sample()
        self.assertTrue(state.complete)
        self.assertEqual(state.rows[0x0F]['last_responses'][0]['value'],41)
        self.assertAlmostEqual(state.rows[0x04]['last_responses'][0]['value'],128*100/255)
        self.assertEqual(state.rows[0x0F]['successes'],1)
        self.assertEqual(state.rows[0x04]['successes'],1)
    async def test_default_targets_skip_unadvertised(self):
        state=p.ProbeState();fake=Bike();probe=p.PidProbe(fake,self.args(),state)
        await probe.discover();await probe.sample()
        self.assertTrue(state.confirmed and state.complete)
        self.assertEqual(state.rows[0x0C]['last_responses'][0]['value'],738)
        self.assertEqual(state.rows[0x0D]['last_responses'][0]['value'],0)
        self.assertEqual(state.rows[0x11]['requests'],0)
        self.assertNotIn('0111',fake.commands)
        self.assertFalse(any(cmd.startswith(('22','04')) for cmd in fake.commands))
    async def test_all_advertised_includes_unknown_raw(self):
        state=p.ProbeState();fake=Bike();probe=p.PidProbe(fake,self.args('--all-supported'),state)
        await probe.discover();await probe.sample()
        self.assertEqual(set(state.rows),{5,12,13,0x21,0x2F})
        raw=state.rows[0x21]['last_responses'][0]
        self.assertIsNone(raw['value']);self.assertEqual(raw['raw'],'00 01')
        self.assertEqual(state.rows[0x21]['successes'],1)
        self.assertNotIn(0x20,state.rows)
    async def test_wrong_pid_and_wrong_source_rejected(self):
        state=p.ProbeState();fake=Bike();probe=p.PidProbe(fake,self.args('--pids','0C'),state);await probe.discover()
        async def wrong(cmd,timeout=3):return wire(bytes.fromhex('410D00'),7)+wire(bytes.fromhex('410C0B88'),7,0x18DAF199)
        fake.command=wrong;await probe.read(12)
        self.assertEqual(state.rows[12]['successes'],0)
        self.assertEqual(state.rows[12]['last_responses'],[])
    async def test_no_data_is_not_success(self):
        state=p.ProbeState();fake=Bike();probe=p.PidProbe(fake,self.args('--pids','0C'),state);await probe.discover()
        async def empty(cmd,timeout=3):return cmd+'\rNO DATA\r'
        fake.command=empty;await probe.read(12)
        self.assertEqual(state.rows[12]['status'],'无有效响应')
        self.assertEqual(state.rows[12]['requests'],1)
    async def test_pid_and_vin_on_k_line(self):
        state=p.ProbeState();fake=Bike(3);probe=p.PidProbe(fake,p.parse_args(['--samples','1','--protocol','3']),state)
        await probe.discover();await probe.sample()
        self.assertEqual(state.protocol,3);self.assertEqual(state.identity.vin,'1D4GP00R55B123456')
        self.assertNotIn('ATCAF1',fake.commands)
    async def test_runner_saves_report_and_disconnects(self):
        with tempfile.TemporaryDirectory() as folder:
            args=self.args('--log',str(Path(folder)/'cl300.log'));client=Client();fake=Bike()
            async def connect(*args):return client,fake
            with patch.object(p.obd,'connect',connect),contextlib.redirect_stdout(io.StringIO()):code=await p.run(args)
            self.assertEqual(code,0);self.assertFalse(client.is_connected)
            report=json.loads(args.report.read_text());self.assertTrue(report['completed'])
            self.assertEqual(report['connection_timeout_seconds'],60)
            self.assertEqual(report['confirmed_protocol'],7)
            self.assertEqual(report['pids']['11']['requests'],0)
            self.assertEqual(report['valid_pid_responses'],4)
    async def test_completed_without_values_is_not_success(self):
        with tempfile.TemporaryDirectory() as folder:
            args=self.args('--pids','0C','--log',str(Path(folder)/'no_data.log'));client=Client();fake=Bike();original=fake.command
            async def command(cmd,timeout=3):return 'NO DATA' if cmd=='010C' else await original(cmd,timeout)
            fake.command=command
            async def connect(*args):return client,fake
            with patch.object(p.obd,'connect',connect),contextlib.redirect_stdout(io.StringIO()):code=await p.run(args)
            self.assertEqual(code,2);report=json.loads(args.report.read_text());self.assertTrue(report['completed']);self.assertEqual(report['valid_pid_responses'],0);self.assertTrue(report['error'])
    async def test_cancellation_saves_partial_and_releases_ble(self):
        with tempfile.TemporaryDirectory() as folder:
            args=self.args('--log',str(Path(folder)/'partial.log'));client=Client();fake=Bike()
            async def connect(*args):return client,fake
            with patch.object(p.obd,'connect',connect),contextlib.redirect_stdout(io.StringIO()):
                task=asyncio.create_task(p.run(args));await asyncio.sleep(.03);task.cancel()
                with self.assertRaises(asyncio.CancelledError):await task
            self.assertFalse(client.is_connected)
            report=json.loads(args.report.read_text());self.assertFalse(report['completed']);self.assertEqual(report['error'],'用户取消')
    async def test_connection_deadline_saves_failure(self):
        with tempfile.TemporaryDirectory() as folder:
            args=self.args('--log',str(Path(folder)/'timeout.log'));args.connection_timeout=.02
            async def slow(*args):await asyncio.sleep(1)
            with patch.object(p.obd,'connect',slow),contextlib.redirect_stdout(io.StringIO()):code=await p.run(args)
            self.assertEqual(code,2);report=json.loads(args.report.read_text());self.assertFalse(report['completed']);self.assertIsNone(report['confirmed_protocol'])
    def test_cli_and_shared_timeouts(self):
        args=p.parse_args([]);self.assertEqual(args.connection_timeout,60);self.assertEqual(args.protocol,'5')
        self.assertEqual(p.obd.TIMING['notice_ms'],60000)
        self.assertEqual(p.pid_list('0C,0D,05,0c'),(12,13,5))
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):p.parse_args(['--all-supported','--pids','0C'])
        for raw in ['00','20','100','XX']:
            with self.assertRaises(Exception):p.pid_list(raw)
    def test_report_and_log_cannot_collide(self):
        with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):p.parse_args(['--log','same.json','--report','same.json'])

if __name__=='__main__':unittest.main()
