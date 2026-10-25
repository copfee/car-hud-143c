#!/usr/bin/env python3
"""IOS-Vlink: standard OBD protocols 1..9, source-aware telemetry and vehicle identity."""
from __future__ import annotations
import argparse
import asyncio
from dataclasses import dataclass, field
from datetime import datetime
import json
import math
from pathlib import Path
import re
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
DEFINITION=json.loads((ROOT/'config/obd_profiles.json').read_text())
METRICS=DEFINITION['metrics']
PROFILES={p['id']:p for p in DEFINITION['profiles']}
PRESSURE=DEFINITION['pressure']
TIMING=DEFINITION['timing']
DEFAULT_DEVICE='IOS-Vlink'
SERVICE='000018f0-0000-1000-8000-00805f9b34fb'
RX='00002af0-0000-1000-8000-00805f9b34fb'
TX='00002af1-0000-1000-8000-00805f9b34fb'
LABELS=['车速','转速','水温','油量','总里程','油门踏板','节气门','制动主缸压力']

@dataclass(frozen=True)
class ObdReply:
    source:int
    data:bytes

class Logger:
    def __init__(self,path):
        path.parent.mkdir(parents=True,exist_ok=True)
        self.file=path.open('a',encoding='utf8');self.path=path
    def __call__(self,event,**fields):
        self.file.write(json.dumps({'time':datetime.now().astimezone().isoformat(),'event':event,**fields},ensure_ascii=False)+'\n');self.file.flush()
    def close(self):self.file.close()

class ElmSession:
    def __init__(self,client,tx,log):
        self.client,self.tx,self.log=client,tx,log
        self.queue=asyncio.Queue(maxsize=128);self.synchronized=True;self.deadline=None
    def receive(self,sender,data):
        try:self.queue.put_nowait(bytes(data))
        except asyncio.QueueFull:self.synchronized=False
    async def command(self,cmd,timeout=1.8):
        if not self.synchronized:raise ConnectionError('ELM 响应不同步，需重建连接')
        if self.deadline is not None:
            timeout=min(timeout,self.deadline-time.monotonic())
            if timeout<=0:raise TimeoutError(f"本轮检测超过{TIMING['round_ms']/1000:g}秒")
        while not self.queue.empty():self.queue.get_nowait()
        self.log('command',command=cmd);start=time.monotonic()
        async def exchange():
            await self.client.write_gatt_char(self.tx,(cmd+'\r').encode(),response='write' in self.tx.properties)
            data=bytearray()
            while b'>' not in data:
                if not self.synchronized:raise ConnectionError('BLE 接收溢出')
                data.extend(await self.queue.get())
                if len(data)>8192:raise ConnectionError('ELM 响应超出限制')
            return data.split(b'>',1)[0].decode('ascii',errors='replace')
        try:answer=await asyncio.wait_for(exchange(),timeout)
        except BaseException:
            self.synchronized=False;self.log('session_error',command=cmd);raise
        self.log('response',command=cmd,response=answer,elapsed_ms=round((time.monotonic()-start)*1000))
        return answer
    async def at(self,cmd):
        r=await self.command(cmd)
        if clean_answer(r,cmd)!='OK':raise ConnectionError(f'{cmd} 返回非 OK: {r.strip()}')

def clean_answer(answer,echo=''):
    return next((s.strip(' >') for s in reversed(re.split('[\r\n]+',answer)) if s.strip(' >') and s.strip(' >')!=echo),'')

def protocol_number(answer):
    s=clean_answer(answer,'ATDPN').replace(' ','').upper()
    return int(s[-1]) if re.fullmatch('A?[1-9]',s) else 0

def checksum(data,protocol):
    if protocol>2:return sum(data)&255
    crc=255
    for b in data:
        crc^=b
        for _ in range(8):crc=((crc<<1)^0x1d)&255 if crc&128 else (crc<<1)&255
    return crc^255

def parse_replies(answer,protocol=0):
    result=[];assemblies={}
    for line in re.split('[\r\n]+',answer):
        s=re.sub(r'\s+','',line).rstrip('>').upper()
        if not s or not re.fullmatch('[0-9A-F]+',s):continue
        if protocol==0 and s.startswith(('41','49')) and len(s)%2==0:
            result.append(ObdReply(0,bytes.fromhex(s)));continue
        if 1<=protocol<=5:
            if len(s)%2:continue
            raw=bytes.fromhex(s)
            if len(raw)<6:continue
            header=3
            if protocol in (4,5):
                if not raw[0]&0xc0:continue
                header=3 if raw[0]&63 else 4
                if len(raw)<header+2:continue
                size=raw[0]&63 if header==3 else raw[3]
                if size!=len(raw)-header-1:continue
            if checksum(raw[:-1],protocol)!=raw[-1]:continue
            result.append(ObdReply(raw[2],raw[header:-1]));continue
        h=8 if protocol in (7,9) or s.startswith(('18DA','18DB')) else 3
        if len(s)<=h or (len(s)-h)%2:continue
        source=int(s[:h],16)
        if source>(0x7ff if h==3 else 0x1fffffff):continue
        raw=bytes.fromhex(s[h:])
        if not 1<=len(raw)<=8:continue
        kind=raw[0]>>4
        if kind==0 and 0<raw[0]<8 and len(raw)>=1+raw[0]:result.append(ObdReply(source,raw[1:1+raw[0]]))
        elif kind==1 and len(raw)==8:
            size=((raw[0]&15)<<8)|raw[1]
            if 7<size<=512:assemblies[source]=(size,1,bytearray(raw[2:]))
            else:assemblies.pop(source,None)
        elif kind==2 and source in assemblies:
            size,seq,data=assemblies[source]
            if raw[0]&15!=seq&15:assemblies.pop(source);continue
            data.extend(raw[1:]);assemblies[source]=(size,seq+1,data)
            if len(data)>=size:result.append(ObdReply(source,bytes(data[:size])));assemblies.pop(source)
    return result

def bitmap(replies,mode=0x41,base=0):
    found={}
    for r in replies:
        if len(r.data)==6 and r.data[:2]==bytes((mode,base)):
            bits=int.from_bytes(r.data[2:],'big');found[r.source]={base+i for i in range(1,33) if bits&(1<<(32-i))}
    return found

def add_capabilities(caps,replies,base=0):
    for source,pids in bitmap(replies,base=base).items():
        e=caps.setdefault(source,{'supported':set(),'known':set()});e['supported'].update(pids);e['known'].update(range(base+1,min(base+33,256)))

def format_commands(protocol):
    return ['ATCAF1','ATCFC1','ATD0','ATCRA','ATH1'] if protocol>=6 else ['ATH1']

def route_commands(protocol,pressure=False):
    if pressure:return ['ATPC',f'ATTP{PRESSURE["protocol"]}',f'ATSH{PRESSURE["tx"]}',f'ATCRA{PRESSURE["rx"]}']
    return ['ATPC',f'ATTP{protocol}',*(['ATSH18DB33F1' if protocol in (7,9) else 'ATSH7DF','ATCRA'] if protocol>=6 else [])]

@dataclass
class VehicleIdentity:
    records:dict=field(default_factory=dict)
    vin:str=''
    ecu:str=''
    model:str='未识别'
    detail:str='等待读取'
    conflict:bool=False
    def add(self,replies,pid,protocol):
        grouped={}
        for r in replies:
            if len(r.data)>3 and r.data[:2]==bytes((0x49,pid)):grouped.setdefault(r.source,[]).append(r.data)
        for source,records in grouped.items():
            if protocol<=5:
                records.sort(key=lambda x:x[2])
                if [r[2] for r in records]!=list(range(1,len(records)+1)):continue
                raw=b''.join(r[3:] for r in records)
            else:
                if len(records)!=1 or records[0][2]!=1:continue
                raw=records[0][3:]
            raw=raw.lstrip(b'\0').rstrip(b'\0\xff ')
            text=''.join(chr(b) if 32<=b<127 else '?' for b in raw)
            if pid==2 and not re.fullmatch('[A-HJ-NPR-Z0-9]{17}',text):continue
            if pid==10 and not 1<=len(text)<=80:continue
            self.records.setdefault(source,{})['vin' if pid==2 else 'ecu']=text
        vins={v['vin'] for v in self.records.values() if v.get('vin')};self.conflict=len(vins)>1
        self.vin='身份信息冲突' if self.conflict else next(iter(vins),'')
        self.ecu='\n'.join(f'ECU {k:X} / {v["ecu"]}' for k,v in sorted(self.records.items()) if v.get('ecu'))
        self.detail='\n'.join(f'ECU {k:X} / {v.get("vin","")}' for k,v in sorted(self.records.items())) or '未获得有效身份信息'

@dataclass
class Reading:
    pid:int=0
    source:int|None=None
    status:str='未知'
    value:float|None=None
    stamp:float=0
    sent:float=0
    due:float=0
    failures:int=0
    cost:float=.12

def decode(metric,data,source):
    if metric['name']=='pressure':
        prefix=bytes.fromhex(PRESSURE['prefix']);offset=len(prefix)+PRESSURE['offset']
        if source!=int(PRESSURE['rx'],16) or not data.startswith(prefix) or len(data)<offset+2:return None
        return int.from_bytes(data[offset:offset+2],'big',signed=True)/PRESSURE['divisor']
    if len(data)<2+metric['bytes'] or data[:2]!=bytes((0x41,metric['pid'])):return None
    v=int.from_bytes(data[2:2+metric['bytes']],'big');kind=metric['kind']
    return v/4 if kind=='rpm' else v-40 if kind=='temperature' else v/10 if kind=='odometer' else v*100/255 if kind=='percent' else v

def show_value(metric,value):
    if value is None:return '--'
    kind=metric['kind']
    if kind=='odometer':return f'{math.floor(value+.5):,} km'
    return f'{value:.2f} bar' if kind=='pressure' else f'{value:.1f}%' if kind=='percent' else f'{value:g} '+{'speed':'km/h','rpm':'rpm','temperature':'°C'}[kind]

class ProtocolStrategy:
    @staticmethod
    async def discover(session,requested='auto',cached=0):
        cached=cached if isinstance(cached,int) and 1<=cached<=9 else 0
        elm=clean_answer(await session.command('ATZ'),'ATZ')
        for cmd in ['ATE0','ATL0','ATS1','ATM0','ATH0']:await session.at(cmd)
        valid=False
        if requested!='auto':
            await session.at('ATTP'+requested);valid=bool(bitmap(parse_replies(await session.command('0100',10))))
        else:
            if cached:
                start=time.monotonic();await session.at(f'ATTP{cached}')
                remaining=TIMING['cached_ms']/1000-(time.monotonic()-start)
                if remaining<=0:raise TimeoutError('缓存协议验证超时')
                valid=bool(bitmap(parse_replies(await session.command('0100',remaining))))
            if not valid:
                await session.at('ATSP0');valid=bool(bitmap(parse_replies(await session.command('0100',TIMING['round_ms']/1000))))
        if not valid:raise ValueError('未获得有效 41 00 ECU 响应，请检查点火与诊断连接')
        number=protocol_number(await session.command('ATDPN'))
        description=clean_answer(await session.command('ATDP'),'ATDP')
        if not number or (requested!='auto' and number!=int(requested)):raise ValueError('实际协议与请求不符')
        for cmd in format_commands(number):await session.at(cmd)
        replies=parse_replies(await session.command('0100',3),number)
        if not bitmap(replies):raise ValueError('带来源的 0100 验证失败')
        return number,description,elm,replies

class RouteManager:
    def __init__(self,session,protocol):self.session,self.protocol,self.active=session,protocol,False
    async def select(self,pressure=False):
        if self.active is pressure:return
        self.active=None
        for cmd in route_commands(self.protocol,pressure):await self.session.at(cmd)
        self.active=pressure

class PollScheduler:
    def __init__(self,controller):self.controller=controller
    def scale(self):
        c=self.controller
        return max(1,sum(r.cost/(m['periods'][0]/1000) for m,r in zip(METRICS,c.readings) if r.status not in ('未知','不支持'))/.6)
    def select(self,now):
        scale=self.scale();candidates=[]
        for i,(m,r) in enumerate(zip(METRICS,self.controller.readings)):
            if r.status in ('未知','不支持') or now<r.due:continue
            score=(now-r.sent)/(m['periods'][0]/1000*scale) if r.sent else 100
            if score>=1:candidates.append((score,-i))
        return -max(candidates)[1] if candidates else None

class ObdController:
    def __init__(self,session,args):
        self.session,self.args=session,args;self.protocol=0;self.description='';self.elm='';self.caps={}
        self.readings=[Reading() for _ in METRICS];self.identity=VehicleIdentity();self.route=None;self.scheduler=PollScheduler(self)
        self.last_standard=0;self.heartbeat_failures=0;self.last_heartbeat=0;self.identity_round=0;self.identity_step=0;self.identity_due=0;self.identity_wanted=True
        self.identity_vin=False;self.identity_name=False;self.status='识别协议'
    async def initialize(self,cached=0):
        self.protocol,self.description,self.elm,replies=await ProtocolStrategy.discover(self.session,self.args.protocol,cached)
        self.route=RouteManager(self.session,self.protocol);add_capabilities(self.caps,replies)
        for base in range(32,161,32):
            if not any(base in e['supported'] for e in self.caps.values()):break
            add_capabilities(self.caps,parse_replies(await self.session.command(f'01{base:02X}',2.5),self.protocol),base)
        for m,r in zip(METRICS,self.readings):
            r.pid=next((p for p in m['pids'] if any(p in e['supported'] for e in self.caps.values())),0)
            known=all(p in e['known'] for p in m['pids'] for e in self.caps.values())
            r.status='无响应' if r.pid else '不支持' if known else '未知'
        self.readings[-1].status='无响应' if PROFILES[self.args.profile]['pressure'] and self.protocol==PRESSURE['required_standard_protocol'] else '不支持'
        if not (self.args.extended or self.args.live):
            for r in self.readings[5:7]:r.pid=0;r.status="未知"
        for i,r in enumerate(self.readings[:-1]):
            if r.pid:await self.read(i)
            if self.last_standard:break
        if not self.last_standard:raise ValueError('ECU 已响应，但未获得有效 HUD 数据')
        self.status='运行中';self.last_heartbeat=time.monotonic()
    async def read(self,i):
        m=METRICS[i];r=self.readings[i];pressure=m['name']=='pressure';start=time.monotonic()
        await self.route.select(pressure)
        answer=await self.session.command(PRESSURE['command'] if pressure else f'01{r.pid:02X}')
        replies=sorted(parse_replies(answer,PRESSURE['protocol'] if pressure else self.protocol),key=lambda x:x.source)
        if r.failures>=3:r.source=None
        value=None
        for reply in replies:
            if r.source is not None and reply.source!=r.source:continue
            if not pressure and r.pid not in self.caps.get(reply.source,{}).get('supported',set()):continue
            value=decode(dict(m,pid=r.pid),reply.data,reply.source)
            if value is not None:r.source=reply.source;break
        now=time.monotonic();r.value=value;r.sent=now;r.cost=.8*r.cost+.2*(now-start)
        if value is None:r.failures+=1;r.due=now+([5,10,30][min(r.failures-1,2)]);r.status='无响应'
        else:
            r.failures=0;r.due=0;r.stamp=now;r.status='有效'
            if not pressure:self.last_standard=now
        if pressure:
            await self.route.select(False)
            r.cost=.8*r.cost+.2*(time.monotonic()-start)
    async def identity_job(self):
        pid=[0,2,10][self.identity_step]
        if pid==0 or (pid==2 and self.identity_vin) or (pid==10 and self.identity_name):
            await self.route.select(False)
            replies=parse_replies(await self.session.command(f'09{pid:02X}',3),self.protocol)
            if pid==0:
                supported=bitmap(replies,0x49);self.identity_vin=not supported or any(2 in v for v in supported.values());self.identity_name=any(10 in v for v in supported.values())
                if supported and not self.identity_vin:self.identity.detail='车辆未声明支持 VIN'
            else:self.identity.add(replies,pid,self.protocol)
        self.identity_step+=1
        if self.identity_step>=3:
            self.identity_step=0;self.identity_round+=1
            self.identity_wanted=(not self.identity.vin or self.identity.conflict) and self.identity_round<2 and self.identity_vin
            self.identity_due=time.monotonic()+TIMING['identity_retry_ms']/1000
    def disconnected(self):
        self.status='后台恢复';self.identity.detail='已断开 / 等待复核'
        for r in self.readings:
            r.value=None
            if r.status not in ('未知','不支持'):r.status='无响应'
    async def cycle(self):
        now=time.monotonic()
        if now-self.last_standard>=TIMING['silence_ms']/1000:
            self.status='等待 ECU 唤醒'
            if now-self.last_heartbeat<TIMING['heartbeat_ms']/1000:return
            self.last_heartbeat=now;await self.route.select(False)
            if not bitmap(parse_replies(await self.session.command('0100',2.5),self.protocol)):
                self.heartbeat_failures+=1
                if self.heartbeat_failures>=3:raise ConnectionError('连续三次标准 ECU 心跳失败')
                return
            self.last_standard=now;self.heartbeat_failures=0
            for r in self.readings:r.sent=r.due=0;r.failures=0
        self.status='运行中'
        if self.identity_wanted and now>=self.identity_due and now-self.last_heartbeat>=1:
            self.last_heartbeat=now;await self.identity_job();return
        i=self.scheduler.select(now)
        if i is not None:await self.read(i)

class TerminalDashboard:
    def __init__(self):self.tty=sys.stdout.isatty()
    def render(self,c,status,log):
        now=time.monotonic();lines=[f'通用 OBD HUD / IOS-Vlink    {status}',f'日志：{log}']
        if c:
            lines += [f'配置：{PROFILES[c.args.profile]["name"]} / 协议：{c.protocol} {c.description}',f'ECU：{c.status} / 最近标准响应：{now-c.last_standard:.1f}s 前' if c.last_standard else 'ECU：等待响应',f'实读车型：{c.identity.model}',f'VIN：{c.identity.vin or "--"}',f'ECU 名称：{c.identity.ecu or "--"}',f'身份状态：{c.identity.detail}','-'*65]
            for label,m,r in zip(LABELS,METRICS,c.readings):
                age=now-r.stamp if r.stamp else 0;limit=max(3,m['periods'][0]/1000*3*c.scheduler.scale())
                value=r.value if age<=limit else None
                lines.append(f'{label:<10} {show_value(m,value):<20} {r.status if value is not None or r.value is None else "已过期"} / {age:.1f}s')
        lines += [f"{TIMING['notice_ms']/1000:g}秒后仍继续检测；Ctrl+C 退出。"]
        if self.tty:sys.stdout.write('\033[H\033[2J'+'\n'.join(lines)+'\n');sys.stdout.flush()
        elif c and status=='单轮读取完成':print('\n'.join(lines))

async def find_adapter(scanner,target):
    if target==DEFAULT_DEVICE:return await scanner.find_device_by_name(target,timeout=10)
    return await scanner.find_device_by_address(target,timeout=10)

async def connect(args,log):
    from bleak import BleakClient,BleakScanner
    device=await find_adapter(BleakScanner,args.device)
    if device is None:raise ConnectionError('未找到 IOS-Vlink，请检查供电并关闭其他客户端')
    client=BleakClient(device,timeout=10)
    try:
        await client.connect();service=client.services.get_service(SERVICE)
        rx=service.get_characteristic(RX) if service else None;tx=service.get_characteristic(TX) if service else None
        if not rx or not tx or not set(tx.properties)&{'write','write-without-response'}:raise ConnectionError('IOS-Vlink GATT 通道不匹配')
        session=ElmSession(client,tx,log);await client.start_notify(rx,session.receive)
        return client,session
    except BaseException:
        if client.is_connected:await client.disconnect()
        raise

async def run(args):
    log=Logger(Path(args.log));dashboard=TerminalDashboard();cache_path=Path(args.cache)
    try:cache=json.loads(cache_path.read_text())
    except (OSError,ValueError):cache={}
    if not isinstance(cache,dict):cache={}
    retry=0
    try:
        while True:
            client=None;c=None;started=time.monotonic()
            key=args.device+'|'+args.profile
            async def operate():
                nonlocal client,c,retry
                client,session=await connect(args,log);session.deadline=started+TIMING['round_ms']/1000
                c=ObdController(session,args);await c.initialize(cache.get(key,0));session.deadline=None;retry=0
                if cache.get(key)!=c.protocol:
                    cache[key]=c.protocol;cache_path.parent.mkdir(parents=True,exist_ok=True);cache_path.write_text(json.dumps(cache))
                if not args.live:
                    for i,r in enumerate(c.readings):
                        if (r.pid and r.value is None) or (i==7 and r.status!='不支持'):await c.read(i)
                    while c.identity_step or c.identity_round==0:await c.identity_job()
                    for _ in range(args.samples-1):
                        await asyncio.sleep(args.interval)
                        for i,r in enumerate(c.readings):
                            if r.pid or (i==7 and r.status!='不支持'):await c.read(i)
                    dashboard.render(c,'单轮读取完成',args.log)
                    return
                while client.is_connected:
                    await c.cycle();await asyncio.sleep(.01)
                raise ConnectionError('BLE 已断开')
            task=asyncio.create_task(operate())
            try:
                while not task.done():
                    notice='仍在检测 / 可等待或 Ctrl+C 退出' if time.monotonic()-started>=TIMING['notice_ms']/1000 else '正在连接'
                    dashboard.render(c,c.status if c and c.last_standard else notice,args.log)
                    if (c is None or not c.last_standard) and time.monotonic()-started>=TIMING['round_ms']/1000:
                        task.cancel();raise TimeoutError(f"本轮检测超过{TIMING['round_ms']/1000:g}秒")
                    await asyncio.sleep(.25)
                await task
                if not args.live:return
            except (Exception,asyncio.CancelledError) as exc:
                if isinstance(exc,asyncio.CancelledError):raise
                log('recover',error=str(exc))
                if c:c.disconnected()
                if c is None or not c.last_standard:
                    cache.pop(key,None);cache_path.parent.mkdir(parents=True,exist_ok=True);cache_path.write_text(json.dumps(cache))
                if not args.live:raise
                delay=TIMING['backoff_ms'][min(retry,3)]/1000;retry+=1
                dashboard.render(c,f'{exc} / {delay}秒后自动重试',args.log)
            finally:
                task.cancel()
                try:await task
                except BaseException:pass
                if client and client.is_connected:await client.disconnect()
            await asyncio.sleep(delay)
    finally:log.close()

def parse_args(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device',default=DEFAULT_DEVICE)
    p.add_argument('--profile',choices=list(PROFILES),default=None)
    p.add_argument('--protocol',choices=['auto',*map(str,range(1,10))],default=None,help='省略时使用车型默认协议；可显式指定auto或1～9')
    modes=p.add_mutually_exclusive_group();modes.add_argument('--read',action='store_true');modes.add_argument('--live',action='store_true')
    p.add_argument('--extended',action='store_true',help='读取已声明支持的标准踏板与节气门数据')
    p.add_argument('--brake-pressure',action='store_true',help='兼容入口：显式选择奔驰车型配置')
    p.add_argument('--samples',type=int,default=1);p.add_argument('--interval',type=float,default=1)
    p.add_argument('--log',default=str(ROOT.parent/'elm327_live.log'))
    p.add_argument('--cache',default=str(ROOT.parent/'elm327_protocol_cache.json'))
    a=p.parse_args(argv)
    if a.brake_pressure:
        if a.profile not in (None,'mercedes-c260-2021'):p.error('--brake-pressure 与非奔驰配置冲突')
        a.profile='mercedes-c260-2021';print('迁移提示：建议使用 --profile mercedes-c260-2021',file=sys.stderr)
    a.profile=a.profile or 'generic'
    if a.protocol is None:a.protocol=str(PROFILES[a.profile].get('default_protocol',0) or 'auto')
    if not 1<=a.samples<=60 or not .2<=a.interval<=10:p.error('samples 应为1～60，interval应为0.2～10秒')
    if not a.live:a.read=True
    return a

def main(argv=None):
    try:asyncio.run(run(parse_args(argv)))
    except KeyboardInterrupt:print('\n已退出，蓝牙连接已释放。')
    except Exception as exc:print(f'读取失败：{exc}',file=sys.stderr);return 1
    return 0

if __name__=='__main__':raise SystemExit(main())
