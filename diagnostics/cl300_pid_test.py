#!/usr/bin/env python3
"""Bounded, read-only Honda CL300 2023 standard OBD PID test via IOS-Vlink."""
from __future__ import annotations
import argparse
import asyncio
from dataclasses import dataclass,field
from datetime import datetime
import json
from pathlib import Path
import shutil
import sys
import time
import unicodedata

# Works both as a direct script and through the workspace compatibility entry.
sys.path.insert(0,str(Path(__file__).resolve().parent))
import elm327_ble as obd

DEFAULT_PIDS=(0x0C,0x0D,0x05,0x11,0x45,0x2F,0xA6,0x42,0x0F,0x1F,0x04,0x49,0x4A,0x4B,0x5A)
SPECS={}
for definition,label in zip(obd.METRICS[:-1],obd.LABELS[:-1]):
    for pid in definition['pids']:SPECS[pid]=dict(definition,pid=pid,label=label)
SPECS.update({
    0x11:dict(SPECS[0x11],label='节气门绝对位置'),
    0x45:dict(SPECS[0x45],label='节气门相对位置'),
    0x42:dict(name='ecu_voltage',pid=0x42,bytes=2,kind='voltage',label='ECU 供电电压'),
    0x0F:dict(name='intake_temperature',pid=0x0F,bytes=1,kind='temperature',label='进气温度'),
    0x1F:dict(name='engine_runtime',pid=0x1F,bytes=2,kind='seconds',label='发动机运行时间'),
    0x04:dict(name='engine_load',pid=0x04,bytes=1,kind='percent',label='发动机负荷'),
})

@dataclass
class ProbeState:
    phase:str='等待连接'
    command:str='--'
    protocol:int=0
    protocol_description:str='--'
    elm:str='--'
    caps:dict=field(default_factory=dict)
    rows:dict=field(default_factory=dict)
    identity:object=field(default_factory=obd.VehicleIdentity)
    round:int=0
    complete:bool=False
    confirmed:bool=False
    error:str=''

class PidProbe:
    """Discover per-ECU capabilities and sample only explicitly advertised Mode 01 PIDs."""
    def __init__(self,session,args,state):self.session,self.args,self.state=session,args,state
    async def discover(self):
        s=self.state;s.phase='自动识别协议' if self.args.protocol=='auto' else '验证指定协议'
        s.protocol,s.protocol_description,s.elm,replies=await obd.ProtocolStrategy.discover(self.session,self.args.protocol)
        s.confirmed=True;obd.add_capabilities(s.caps,replies)
        for base in range(0x20,0xE1,0x20):
            if not any(base in ecu['supported'] for ecu in s.caps.values()):break
            s.phase=f'读取支持位图 01{base:02X}'
            response=await self.session.command(f'01{base:02X}',3)
            obd.add_capabilities(s.caps,obd.parse_replies(response,s.protocol),base)
        supported={p for ecu in s.caps.values() for p in ecu['supported'] if p<=255 and p%32}
        selected=sorted(supported) if self.args.all_supported else self.args.pids or DEFAULT_PIDS
        for pid in selected:
            declared=pid in supported
            known=all(pid in ecu['known'] for ecu in s.caps.values())
            s.rows[pid]={'label':SPECS.get(pid,{}).get('label','原始数据'),
                         'declared':declared,'status':'等待采样' if declared else '未声明支持' if known else '支持情况未知',
                         'requests':0,'successes':0,'sources':{},'last_responses':[]}
    async def read(self,pid):
        s=self.state;row=s.rows[pid]
        if not row['declared']:return
        row['requests']+=1;s.command=f'01{pid:02X}'
        answer=await self.session.command(s.command,3)
        replies=obd.parse_replies(answer,s.protocol)
        spec=SPECS.get(pid);minimum=2+spec['bytes'] if spec else 3
        valid=[]
        for reply in replies:
            if pid not in s.caps.get(reply.source,{}).get('supported',set()):continue
            if len(reply.data)<minimum or reply.data[:2]!=bytes((0x41,pid)):continue
            raw=reply.data[2:];value=None;unit='raw'
            if spec:
                if spec['kind']=='voltage':value=int.from_bytes(raw[:2],'big')/1000;unit='V'
                elif spec['kind']=='seconds':value=int.from_bytes(raw[:2],'big');unit='s'
                else:
                    value=obd.decode(spec,reply.data,reply.source)
                    unit={'rpm':'rpm','speed':'km/h','temperature':'°C','percent':'%','odometer':'km'}[spec['kind']]
            record={'source':f'{reply.source:X}','raw':raw.hex(' ').upper(),'payload':reply.data.hex(' ').upper(),
                    'value':value,'unit':unit,'round':s.round,'time':datetime.now().astimezone().isoformat()}
            valid.append(record);row['sources'][record['source']]=record
        row['last_responses']=valid
        row['status']='有效响应' if valid else '无有效响应'
        if valid:row['successes']+=1
    async def identity(self):
        s=self.state;s.phase='读取车辆身份'
        replies=obd.parse_replies(await self.session.command('0900',3),s.protocol)
        support=obd.bitmap(replies,mode=0x49)
        if not support or any(2 in pids for pids in support.values()):
            s.identity.add(obd.parse_replies(await self.session.command('0902',3),s.protocol),2,s.protocol)
        else:s.identity.detail='车辆未声明支持 VIN'
        if any(10 in pids for pids in support.values()):
            s.identity.add(obd.parse_replies(await self.session.command('090A',3),s.protocol),10,s.protocol)
    async def sample(self):
        s=self.state
        for n in range(1,self.args.samples+1):
            s.round=n;s.phase=f'采样 {n}/{self.args.samples}';started=time.monotonic()
            for pid,row in s.rows.items():
                if not row['declared']:continue
                await self.read(pid)
                # Conservative pacing for older J1850/K-Line implementations.
                await asyncio.sleep(.1)
            if n<self.args.samples:await asyncio.sleep(max(0,self.args.interval-(time.monotonic()-started)))
        if not self.args.no_identity:await self.identity()
        s.complete=True;s.phase='测试完成'

def value_text(record):
    if record['value'] is None:return record['raw']
    value=record['value'];unit=record['unit']
    if unit=='km':return f'{int(value+.5)} km'
    return f'{value:g} {unit}'

def clip(text,width):
    result=[];used=0
    for char in text:
        size=0 if unicodedata.combining(char) else 2 if unicodedata.east_asian_width(char) in ('W','F') else 1
        if used+size>width:break
        result.append(char);used+=size
    return ''.join(result)

class TerminalView:
    def __init__(self,enabled=True):self.tty=enabled and sys.stdout.isatty()
    def render(self,s,args,started,final=False):
        if not self.tty and not final:return
        width,height=shutil.get_terminal_size((120,40))
        lines=['本田 CL300 2023 / 标准 PID 只读测试（车型为手动指定）',
               f'状态：{s.phase} / 已用 {time.monotonic()-started:.1f}s / 连接检测上限 {args.connection_timeout:g}s',
               f'协议：{s.protocol or "--"} / {s.protocol_description} / ECU已验证：{"是" if s.confirmed else "否"}',
               f'适配器：{s.elm} / 当前命令：{s.command}',
               f'VIN：{s.identity.vin or "--"} / 实读车型：{s.identity.model}',
               'PID   项目                  当前响应（保留不同 ECU 来源）', '-'*min(width-1,100)]
        limit=max(1,height-13) if self.tty else len(s.rows)
        for pid,row in list(s.rows.items())[:limit]:
            values='; '.join(f'{r["source"]}: {value_text(r)}' for r in row['last_responses']) or '--'
            lines.append(f'{pid:02X}    {row["label"]}  {values} / {row["status"]} ({row["successes"]}/{row["requests"]})')
        if len(s.rows)>limit:lines.append(f'其余 {len(s.rows)-limit} 项保存在 JSON 汇总中。')
        if s.error:lines.append('原因：'+s.error)
        lines += [f'日志：{args.log}',f'汇总：{args.report}','不发送奔驰专用命令；Ctrl+C 退出并保存当前结果。']
        if self.tty:sys.stdout.write('\033[H\033[2J')
        print('\n'.join(clip(line,max(1,width-1)) if self.tty else line for line in lines),flush=True)

def make_report(s,args):
    return {'target_vehicle':'Honda CL300 2023 (user supplied, not auto identified)',
            'created_at':datetime.now().astimezone().isoformat(),'completed':s.complete,'error':s.error,
            'device':args.device,'requested_protocol':args.protocol,'connection_timeout_seconds':args.connection_timeout,
            'confirmed_protocol':s.protocol if s.confirmed else None,'protocol_description':s.protocol_description,
            'elm':s.elm,'mode':'all_advertised_pids' if args.all_supported else 'selected_advertised_pids',
            'rounds_finished':s.round if s.complete else max(0,s.round-1),'rounds_requested':args.samples,
            'capabilities':{f'{source:X}':{'supported':[f'{p:02X}' for p in sorted(ecu['supported']) if p<=255],
                                        'known':[f'{p:02X}' for p in sorted(ecu['known'])]} for source,ecu in sorted(s.caps.items())},
            'pids':{f'{pid:02X}':row for pid,row in s.rows.items()},
            'valid_pid_responses':sum(row['successes'] for row in s.rows.values()),
            'identity':{'model':s.identity.model,'vin':s.identity.vin,'ecu_name':s.identity.ecu,
                        'conflict':s.identity.conflict,'detail':s.identity.detail},
            'log':str(args.log)}

def save_report(s,args):
    args.report.parent.mkdir(parents=True,exist_ok=True)
    temporary=args.report.with_suffix(args.report.suffix+'.tmp')
    temporary.write_text(json.dumps(make_report(s,args),ensure_ascii=False,indent=2)+'\n')
    temporary.replace(args.report)

async def run(args):
    s=ProbeState();logger=obd.Logger(args.log);view=TerminalView(not args.no_tui);started=time.monotonic();client=None;session=None
    def log(event,**fields):
        if event=='command':s.command=fields['command']
        if event=='response' and fields['command']=='ATZ':s.elm=obd.clean_answer(fields['response'],'ATZ')
        logger(event,round=s.round,phase=s.phase,**fields)
    async def work():
        nonlocal client,session
        async def establish():
            nonlocal client,session
            s.phase='搜索并连接 IOS-Vlink';client,session=await obd.connect(args,log)
            session.deadline=started+args.connection_timeout
            await PidProbe(session,args,s).discover()
        await asyncio.wait_for(establish(),max(.001,args.connection_timeout-(time.monotonic()-started)))
        session.deadline=None
        await PidProbe(session,args,s).sample()
    task=asyncio.create_task(work());logger('start',target='Honda CL300 2023',requested_protocol=args.protocol,connection_timeout=args.connection_timeout)
    try:
        while not task.done():view.render(s,args,started);await asyncio.sleep(.2)
        await task
        if any(row['successes'] for row in s.rows.values()):return 0
        s.error='流程完成，但未读取到有效 PID 数据';return 2
    except asyncio.CancelledError:
        s.error='用户取消';raise
    except Exception as exc:
        s.error=(f'{args.connection_timeout:g}秒内未完成连接与协议验证' if not s.confirmed else f'{s.command} 等待响应超时') if isinstance(exc,TimeoutError) else str(exc) or type(exc).__name__;s.phase='测试未完成'
        # The reported adapter protocol alone never confirms a vehicle connection.
        if session and session.synchronized and not s.confirmed and time.monotonic()-started<args.connection_timeout:
            try:
                reported=obd.clean_answer(await session.command('ATDPN'),'ATDPN')
                description=obd.clean_answer(await session.command('ATDP'),'ATDP')
                s.protocol_description=f'仅适配器配置 {reported}: {description}，没有有效 ECU 证据'
            except Exception:pass
        return 2
    finally:
        task.cancel()
        try:await task
        except BaseException:pass
        try:
            if client and client.is_connected:await asyncio.wait_for(client.disconnect(),5)
        finally:
            try:save_report(s,args);logger('finish',completed=s.complete,error=s.error)
            finally:logger.close();view.render(s,args,started,final=True)

def pid_list(text):
    try:pids=tuple(dict.fromkeys(int(x.strip(),16) for x in text.split(',')))
    except ValueError as exc:raise argparse.ArgumentTypeError('PID 应为十六进制，例如 0C,0D,05,11') from exc
    if not pids or any(not 1<=p<=255 or p%32==0 for p in pids):raise argparse.ArgumentTypeError('PID 应为01～FF且不能是支持位图页')
    return pids

def parse_args(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--device',default=obd.DEFAULT_DEVICE)
    p.add_argument('--protocol',choices=['auto',*map(str,range(1,10))],default=str(obd.PROFILES['honda-cl300-2023']['default_protocol']))
    p.add_argument('--connection-timeout',type=float,default=obd.TIMING['round_ms']/1000)
    p.add_argument('--samples',type=int,default=5,help='有限采样轮数，默认5')
    p.add_argument('--interval',type=float,default=1,help='两轮起点的最小间隔秒数')
    selection=p.add_mutually_exclusive_group();selection.add_argument('--all-supported',action='store_true',help='读取所有已声明的标准PID，未知格式保留原始数据')
    selection.add_argument('--pids',type=pid_list,help='仅测试指定PID，例如0C,0D,05,11；仍需车辆声明支持')
    p.add_argument('--no-identity',action='store_true');p.add_argument('--no-tui',action='store_true')
    p.add_argument('--log',type=Path,default=obd.ROOT.parent/'cl300_pid_test.log')
    p.add_argument('--report',type=Path)
    a=p.parse_args(argv)
    if not 1<=a.samples<=60 or not .2<=a.interval<=10 or not 5<=a.connection_timeout<=180:p.error('samples为1～60，interval为0.2～10，connection-timeout为5～180秒')
    a.report=a.report or a.log.with_name(a.log.stem+'_summary.json')
    if a.report.resolve()==a.log.resolve():p.error('原始日志和JSON汇总必须使用不同文件')
    return a

def main(argv=None):
    try:return asyncio.run(run(parse_args(argv)))
    except KeyboardInterrupt:print('\n已停止，已保存日志与当前汇总。');return 130
    except (OSError,ValueError) as exc:print(f'测试无法完成：{exc}',file=sys.stderr);return 2

if __name__=='__main__':raise SystemExit(main())
