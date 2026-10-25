import asyncio
import contextlib
import io
import json
import importlib.util
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('hud_diagnostics',ROOT/'diagnostics/elm327_ble.py')
m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)
VIN='1D4GP00R55B123456'

def wire(data,protocol=7,source=None):
    if protocol<=5:
        source=source or 0x10;header=bytes((0x48,0x6b,source)) if protocol<=3 else bytes((0xc0|len(data),0xf1,source))
        raw=header+data;return (raw+bytes((m.checksum(raw,protocol),))).hex()+'\r'
    source=source or (0x18daf110 if protocol in (7,9) else 0x7e8);h=f'{source:08X}' if protocol in (7,9) else f'{source:03X}'
    if len(data)<=7:return h+(bytes((len(data),))+data).ljust(8,b'\0').hex()+'\r'
    out=h+(bytes((0x10|len(data)>>8,len(data)&255))+data[:6]).hex()+'\r';offset=6;seq=1
    while offset<len(data):
        out+=h+(bytes((0x20|seq,))+data[offset:offset+7]).ljust(8,b'\0').hex()+'\r';offset+=7;seq=(seq+1)&15
    return out

class Fake:
    def __init__(self,protocol=7):self.intended=protocol;self.protocol=protocol;self.headers=False;self.commands=[];self.asleep=False;self.standard_only=True
    async def at(self,cmd):
        result=await self.command(cmd)
        if m.clean_answer(result,cmd)!='OK':raise ConnectionError(cmd)
    async def command(self,cmd,timeout=1.8):
        self.commands.append(cmd)
        if cmd=='ATZ':self.headers=False;return 'ELM327 TEST'
        if cmd=='ATH0':self.headers=False
        if cmd=='ATH1':self.headers=True
        if cmd.startswith('ATTP'):self.protocol=int(cmd[-1])
        if cmd=='ATSP0':self.protocol=self.intended
        if cmd=='ATDPN':return 'A'+str(self.protocol)
        if cmd=='ATDP':return f'Protocol {self.protocol}'
        if cmd.startswith('AT'):return 'OK'
        if self.asleep or self.protocol!=self.intended:return 'NO DATA'
        values={'0100':'410008180000','010D':'410D00','010C':'410C0B88','0105':'410589','0900':'490040000000'}
        if cmd=='0902':
            if self.protocol<=5:
                raw=b'\0\0\0'+VIN.encode();return ''.join(wire(bytes((0x49,2,i+1))+raw[i*4:(i+1)*4],self.protocol) for i in range(5))
            return wire(b'\x49\x02\x01'+VIN.encode(),self.protocol)
        data=values.get(cmd)
        if data is None:return 'NO DATA'
        return wire(bytes.fromhex(data),self.protocol) if self.headers else data

class Pure(unittest.TestCase):
    def test_shared_wire_fixtures(self):
        for row in (ROOT/'tests/fixtures/protocol.tsv').read_text().splitlines():
            if row.startswith('#') or not row:continue
            p,raw,source,data=row.split('|')
            with self.subTest(protocol=p,wire=raw):
                replies=m.parse_replies(raw.replace('\\r','\r'),int(p))
                self.assertEqual(replies,[] if source=='-' else [m.ObdReply(int(source,16),bytes.fromhex(data))])
    def test_identity_legacy_and_can(self):
        for protocol in range(1,10):
            fake=Fake(protocol)
            answer=asyncio.run(fake.command('0902'))
            identity=m.VehicleIdentity();identity.add(m.parse_replies(answer,protocol),2,protocol)
            self.assertEqual(identity.vin,VIN);self.assertEqual(identity.model,'未识别')
    def test_conflict_and_invalid(self):
        identity=m.VehicleIdentity();data=b'\x49\x02\x01'+VIN.encode()
        identity.add([m.ObdReply(0x7e8,data),m.ObdReply(0x7e9,data)],2,6)
        self.assertFalse(identity.conflict)
        identity.add([m.ObdReply(0x7e9,data[:-1]+b'8')],2,6);self.assertTrue(identity.conflict)
        for raw in [data[:-1],data.replace(b'G',b'I'),data+b'1']:
            invalid=m.VehicleIdentity();invalid.add([m.ObdReply(0x7e8,raw)],2,6);self.assertEqual(invalid.vin,'')
    def test_historical_mercedes_capture(self):
        captures=json.loads((ROOT/'tests/fixtures/mercedes_capture.json').read_text())
        self.assertEqual(len(captures),6)
        for c in captures:
            self.assertIn(m.ObdReply(c['source'],bytes.fromhex(c['payload'])),m.parse_replies(c['response'],c['protocol']))
    def test_source_capabilities(self):
        caps={};m.add_capabilities(caps,[m.ObdReply(0x7e8,bytes.fromhex('410008180001')),m.ObdReply(0x7e9,bytes.fromhex('410000000000'))])
        self.assertIn(12,caps[0x7e8]['supported']);self.assertNotIn(12,caps[0x7e9]['supported']);self.assertNotIn(47,caps[0x7e8]['known'])
    def test_defaults_and_conflicts(self):
        a=m.parse_args([]);self.assertEqual((a.profile,a.protocol),('generic','auto'))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(m.parse_args(['--brake-pressure']).profile,'mercedes-c260-2021')
        with self.assertRaises(SystemExit),contextlib.redirect_stderr(io.StringIO()):m.parse_args(['--profile','generic','--brake-pressure'])
        for p in range(1,10):self.assertEqual(m.parse_args(['--protocol',str(p)]).protocol,str(p))
    def test_no_can_config_on_legacy(self):
        for p in range(1,6):self.assertEqual(m.format_commands(p),['ATH1'])
    def test_rounding(self):
        metric=next(x for x in m.METRICS if x['name']=='odometer')
        self.assertEqual(m.show_value(metric,123.5),'124 km');self.assertEqual(m.show_value(metric,123.49),'123 km')
    def test_pressure_source(self):
        data=bytes.fromhex('622002')+bytes(12)+(6536).to_bytes(2,'big')
        self.assertIsNone(m.decode(m.METRICS[-1],data,0x7e8));self.assertAlmostEqual(m.decode(m.METRICS[-1],data,0x7ea),100.0018,places=2)
    def test_partial_and_wrong_sequences(self):
        answer=wire(b'\x49\x02\x01'+VIN.encode(),6)
        self.assertEqual(m.parse_replies(answer.split('\r')[0],6),[])
        self.assertEqual(m.parse_replies(answer.replace('7E821','7E823'),6),[])

class Async(unittest.IsolatedAsyncioTestCase):
    async def test_all_protocol_initialization(self):
        for p in range(1,10):
            fake=Fake(p);c=m.ObdController(fake,m.parse_args(['--extended']))
            await c.initialize();await c.read(1);self.assertEqual(c.protocol,p);self.assertEqual(c.readings[0].value,0);self.assertEqual(c.readings[1].value,738)
            for _ in range(3):await c.identity_job()
            self.assertEqual(c.identity.vin,VIN)
            self.assertFalse(any(x.startswith('22') for x in fake.commands))
            if p<6:self.assertFalse(any(x in fake.commands for x in ['ATCAF1','ATCFC1','ATD0','ATCRA']))
    async def test_honda_profile_defaults_and_override(self):
        args=m.parse_args(['--profile','honda-cl300-2023']);self.assertEqual(args.protocol,'5')
        fake=Fake(5);controller=m.ObdController(fake,args);await controller.initialize()
        self.assertEqual(controller.protocol,5);self.assertEqual(controller.readings[-1].status,'不支持')
        self.assertIn('ATTP5',fake.commands);self.assertNotIn('ATSP0',fake.commands)
        self.assertFalse(any(cmd.startswith(('22','ATSH','ATCRA','ATCAF','ATCFC')) for cmd in fake.commands))
        self.assertEqual(m.parse_args(['--profile','honda-cl300-2023','--protocol','auto']).protocol,'auto')
        self.assertEqual(m.parse_args(['--profile','honda-cl300-2023','--protocol','3']).protocol,'3')
    async def test_invalid_cache_is_ignored(self):
        fake=Fake();await m.ProtocolStrategy.discover(fake,cached='corrupt')
        self.assertIn('ATSP0',fake.commands)
        self.assertFalse(any(x.startswith('ATTP') for x in fake.commands))
    async def test_pressure_does_not_refresh_standard_heartbeat(self):
        fake=Fake();c=m.ObdController(fake,m.parse_args(['--profile','mercedes-c260-2021']))
        await c.initialize();before=c.last_standard;original=fake.command
        async def command(cmd,timeout=1.8):
            if cmd==m.PRESSURE['command']:
                return wire(bytes.fromhex('622002')+bytes(12)+(1675).to_bytes(2,'big'),6,0x7ea)
            return await original(cmd,timeout)
        fake.command=command
        await c.read(7)
        self.assertAlmostEqual(c.readings[7].value,25.62775,places=3)
        self.assertEqual(c.last_standard,before);self.assertFalse(c.route.active)
    async def test_cached_fallback(self):
        fake=Fake(7);await m.ProtocolStrategy.discover(fake,cached=6)
        self.assertLess(fake.commands.index('ATTP6'),fake.commands.index('ATSP0'))
    async def test_forced_never_falls_back(self):
        fake=Fake(7)
        with self.assertRaises(ValueError):await m.ProtocolStrategy.discover(fake,'6')
        self.assertNotIn('ATSP0',fake.commands)
    async def test_no_false_protocol_confirmation(self):
        fake=Fake();fake.asleep=True
        with self.assertRaises(ValueError):await m.ProtocolStrategy.discover(fake)
        self.assertNotIn('ATDPN',fake.commands)
    async def test_sleep_heartbeat_failure(self):
        fake=Fake();c=m.ObdController(fake,m.parse_args([]));await c.initialize();fake.asleep=True;c.last_standard-=16
        for i in range(3):
            c.last_heartbeat-=6
            if i<2:await c.cycle()
            else:
                with self.assertRaises(ConnectionError):await c.cycle()
        c.identity.vin=VIN;c.disconnected()
        self.assertEqual(c.identity.vin,VIN);self.assertIn('已断开',c.identity.detail)
        self.assertTrue(all(r.value is None for r in c.readings))
    async def test_route_does_not_cache_partial_failure(self):
        fake=Fake();route=m.RouteManager(fake,7)
        await route.select(True);await route.select(True);await route.select(False)
        self.assertEqual(fake.commands,m.route_commands(7,True)+m.route_commands(7,False))
    async def test_session_missing_prompt(self):
        class Client:
            async def write_gatt_char(self,*args,**kwargs):pass
        class Tx:properties=['write']
        session=m.ElmSession(Client(),Tx(),lambda *a,**k:None)
        with self.assertRaises(TimeoutError):await session.command('0100',.01)
        self.assertFalse(session.synchronized)
        with self.assertRaises(ConnectionError):await session.command('010D')
    async def test_fragmented_notifications(self):
        class Client:
            async def write_gatt_char(self,*args,**kwargs):
                session.receive(None,b'41 0');session.receive(None,b'D 00\r>')
        class Tx:properties=['write']
        session=m.ElmSession(Client(),Tx(),lambda *a,**k:None)
        self.assertEqual(await session.command('010D'),'41 0D 00\r')


class AdapterDiscovery(unittest.IsolatedAsyncioTestCase):
    async def test_default_name_discovery(self):
        from unittest.mock import AsyncMock
        scanner=type('Scanner',(),{'find_device_by_name':AsyncMock(return_value='adapter'),'find_device_by_address':AsyncMock()})
        self.assertEqual(await m.find_adapter(scanner,m.DEFAULT_DEVICE),'adapter')
        scanner.find_device_by_name.assert_awaited_once_with('IOS-Vlink',timeout=10)
        scanner.find_device_by_address.assert_not_awaited()
    async def test_explicit_address_discovery(self):
        from unittest.mock import AsyncMock
        scanner=type('Scanner',(),{'find_device_by_name':AsyncMock(),'find_device_by_address':AsyncMock(return_value='adapter')})
        self.assertEqual(await m.find_adapter(scanner,'AA:BB:CC:DD:EE:FF'),'adapter')
        scanner.find_device_by_address.assert_awaited_once_with('AA:BB:CC:DD:EE:FF',timeout=10)
        scanner.find_device_by_name.assert_not_awaited()
    async def test_missing_name_is_not_an_address_fallback(self):
        from unittest.mock import AsyncMock
        scanner=type('Scanner',(),{'find_device_by_name':AsyncMock(return_value=None),'find_device_by_address':AsyncMock()})
        self.assertIsNone(await m.find_adapter(scanner,m.DEFAULT_DEVICE))
        scanner.find_device_by_address.assert_not_awaited()

if __name__=='__main__':unittest.main()
