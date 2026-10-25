#!/usr/bin/env python3
"""Passive Mercedes CAN visibility test. No OBD/UDS requests or CAN injection."""
from __future__ import annotations
import argparse
import asyncio
from collections import Counter,deque
import csv
from dataclasses import dataclass,field
from datetime import datetime,timezone,timedelta
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import sys
import time
import unicodedata

sys.path.insert(0,str(Path(__file__).resolve().parent))
import elm327_ble as obd

VERSION='1.0.0'
TZ=timezone(timedelta(hours=8))

def utc8():return datetime.now(TZ).isoformat(timespec='milliseconds')
def clip(text,width):
    result=[];used=0
    for c in text:
        n=0 if unicodedata.combining(c) else 2 if unicodedata.east_asian_width(c) in ('W','F') else 1
        if used+n>width:break
        result.append(c);used+=n
    return ''.join(result)

class SafetyError(RuntimeError):pass

def allowed_command(command):
    fixed={'ATI','ATZ','ATE0','ATL0','ATS1','ATM0','ATH1','ATCSM1','ATCAF0','ATCFC0','ATD0','ATD1','ATDPN','ATDP','ATCS','ATRV','ATMA'}
    if command in fixed:return True
    if command in ('ATTP6','ATTP7'):return True
    return bool(re.fullmatch(r'ATC[FM](?:[0-9A-F]{3}|[0-9A-F]{8})',command) or re.fullmatch(r'ATCRA(?:[0-9A-F]{3}|[0-9A-F]{8})',command))

@dataclass(frozen=True)
class CanFrame:
    can_id:int
    bits:int
    data:bytes
    format_note:str=''
    @property
    def key(self):return f'{self.can_id:03X}' if self.bits==11 else f'{self.can_id:08X}'

def parse_frame(line,bits,dlc_expected=True):
    text=''.join(line.split()).upper()
    if not re.fullmatch('[0-9A-F]+',text):return None
    h=3 if bits==11 else 8
    if len(text)<h:return None
    can_id=int(text[:h],16)
    if can_id>(0x7FF if bits==11 else 0x1FFFFFFF):return None
    rest=text[h:];note=''
    if dlc_expected and not rest:return None
    if dlc_expected and len(rest)%2==1:
        dlc=int(rest[0],16)
        if dlc>8 or len(rest[1:])!=dlc*2:return None
        raw=rest[1:]
    else:
        if len(rest)%2 or len(rest)>16:return None
        raw=rest
        if dlc_expected:note='DLC missing despite ATD1; interpreted as header + raw bytes'
    return CanFrame(can_id,bits,bytes.fromhex(raw),note)

def diagnostic_shape(frame):
    return (frame.bits==11 and 0x7E0<=frame.can_id<=0x7EF) or (frame.bits==29 and frame.can_id>>16 in (0x18DA,0x18DB))

class LineFramer:
    def __init__(self,limit=4096):self.buffer=bytearray();self.limit=limit;self.discarding=False
    def feed(self,data):
        result=[]
        for byte in data:
            if byte in (10,13,62):
                if self.buffer:result.append(('line',self.buffer.decode('ascii',errors='replace')));self.buffer.clear()
                if self.discarding:self.discarding=False
                if byte==62:result.append(('prompt','>'))
            elif not self.discarding:
                self.buffer.append(byte)
                if len(self.buffer)>self.limit:self.buffer.clear();self.discarding=True;result.append(('oversize','line exceeded limit'))
        return result

@dataclass
class IdStats:
    count:int=0
    first_ns:int=0
    last_ns:int=0
    changes:int=0
    last:bytes=b''
    dlcs:Counter=field(default_factory=Counter)
    samples:list=field(default_factory=list)
    gaps:deque=field(default_factory=lambda:deque(maxlen=256))
    zero_gaps:int=0
    gap_sum_ns:int=0
    positive_gaps:int=0
    minimum_gap_ns:int=0
    maximum_gap_ns:int=0
    byte_min:list=field(default_factory=lambda:[256]*8)
    byte_max:list=field(default_factory=lambda:[-1]*8)
    byte_values:list=field(default_factory=lambda:[0]*8)
    variable_bits:list=field(default_factory=lambda:[0]*8)
    transitions:list=field(default_factory=lambda:[0]*8)
    increment_one:list=field(default_factory=lambda:[0]*8)
    def add(self,data,received_ns):
        if self.count:
            gap=max(0,received_ns-self.last_ns);self.gap_sum_ns+=gap
            if gap:
                self.positive_gaps+=1;self.gaps.append(gap/1e6)
                self.minimum_gap_ns=gap if not self.minimum_gap_ns else min(self.minimum_gap_ns,gap)
                self.maximum_gap_ns=max(self.maximum_gap_ns,gap)
            else:self.zero_gaps+=1
            self.changes+=data!=self.last
            for i,(old,new) in enumerate(zip(self.last,data)):
                self.variable_bits[i]|=old^new;self.transitions[i]+=1;self.increment_one[i]+=((new-old)&255)==1
        else:self.first_ns=received_ns
        self.count+=1;self.last_ns=received_ns;self.dlcs[len(data)]+=1
        text=data.hex(' ').upper()
        if text not in self.samples and len(self.samples)<8:self.samples.append(text)
        for i,b in enumerate(data):self.byte_min[i]=min(self.byte_min[i],b);self.byte_max[i]=max(self.byte_max[i],b);self.byte_values[i]|=1<<b
        self.last=data
    def report(self,seconds):
        gaps=sorted(self.gaps)
        return {'count':self.count,'observed_hz_over_phase':round(self.count/max(.001,seconds),3),
                'payload_changes':self.changes,'dlc_counts':dict(self.dlcs),'payload_examples_first_8':self.samples,'last_data':self.last.hex(' ').upper(),
                'host_timing':{'first_monotonic_ns':self.first_ns,'last_monotonic_ns':self.last_ns,'zero_interval_pairs':self.zero_gaps,
                 'mean_interval_ms':self.gap_sum_ns/max(1,self.count-1)/1e6,'minimum_positive_ms':self.minimum_gap_ns/1e6,
                 'maximum_ms':self.maximum_gap_ns/1e6,'positive_interval_samples_recent':len(gaps),
                 'p50_positive_recent_ms':statistics.median(gaps) if gaps else None,
                 'p95_positive_recent_ms':gaps[int(.95*(len(gaps)-1))] if gaps else None},
                'bytes':[{'index':i,'min':self.byte_min[i],'max':self.byte_max[i],'distinct_values':self.byte_values[i].bit_count(),
                          'variable_bit_mask':f'{self.variable_bits[i]:02X}',
                          'incrementing_counter_candidate':self.transitions[i]>=10 and self.increment_one[i]/self.transitions[i]>.8}
                         for i in range(8) if self.byte_max[i]>=0]}

class RunFiles:
    def __init__(self,directory):
        directory.mkdir(parents=True,exist_ok=False);self.path=directory;self.start_ns=time.monotonic_ns();self.phase=None;self.error=None;self.last_flush=time.monotonic()
        self.raw=(directory/'raw.jsonl').open('w',encoding='utf8',buffering=65536)
        self.frame_file=(directory/'frames.csv').open('w',encoding='utf8',newline='');self.frames=csv.writer(self.frame_file)
        self.frames.writerow(['time_utc8','elapsed_ms','phase','protocol','id_bits','can_id','dlc','data_hex','diagnostic_shape','last_marker'])
        self.mark_file=(directory/'markers.csv').open('w',encoding='utf8',newline='');self.marks=csv.writer(self.mark_file);self.marks.writerow(['time_utc8','elapsed_ms','phase','label'])
        self.markers=[]
    def log(self,event,**fields):
        try:self.raw.write(json.dumps({'time':utc8(),'elapsed_ms':round((time.monotonic_ns()-self.start_ns)/1e6,3),'phase':self.phase,'event':event,**fields},ensure_ascii=False)+'\n')
        except OSError as exc:self.error=str(exc)
    def flush(self):
        if time.monotonic()-self.last_flush>=.5:
            for file in (self.raw,self.frame_file,self.mark_file):file.flush()
            self.last_flush=time.monotonic()
    def marker(self,label):
        event={'time':utc8(),'elapsed_ms':round((time.monotonic_ns()-self.start_ns)/1e6,3),'phase':self.phase,'label':label}
        self.markers.append(event);self.marks.writerow(event.values());self.log('marker',label=label)
    def frame(self,frame,phase,received_ns):
        marker=self.markers[-1]['label'] if self.markers else ''
        self.frames.writerow([utc8(),round((received_ns-self.start_ns)/1e6,3),phase['index'],phase['protocol'],frame.bits,frame.key,len(frame.data),frame.data.hex(' ').upper(),int(diagnostic_shape(frame)),marker])
    def close(self):
        for file in (self.raw,self.frame_file,self.mark_file):file.close()

class TimedQueue(asyncio.Queue):
    def __init__(self,size,files):super().__init__(maxsize=size);self.arrivals=deque();self.last_received_ns=0;self.dropped=0;self.chunks=0;self.bytes=0;self.files=files
    def put_nowait(self,item):
        received=time.monotonic_ns();self.chunks+=1;self.bytes+=len(item)
        self.files.log('ble_rx',received_monotonic_ns=received,hex=item.hex(' ').upper(),ascii=item.decode('ascii',errors='replace'))
        try:super().put_nowait(item)
        except asyncio.QueueFull:self.dropped+=1;self.files.log('queue_overflow',dropped=self.dropped);raise
        self.arrivals.append(received)
    def get_nowait(self):
        item=super().get_nowait();self.last_received_ns=self.arrivals.popleft();return item

class MonitorSession:
    def __init__(self,base,files,args):
        self.base,self.files,self.args=base,files,args;self.queue=TimedQueue(args.queue_size,files);base.queue=self.queue
        self.streaming=False;self.prompt_seen=False;self.configured=False;self.command_results=[]
    async def write(self,data):
        cmd=data.decode('ascii',errors='replace').rstrip('\r')
        if not ((data==b'x' and self.streaming) or (data.endswith(b'\r') and allowed_command(cmd))):raise SafetyError('已阻止非ELM配置或监听数据写入')
        self.files.log('ble_tx',hex=data.hex(' ').upper(),ascii=data.decode('ascii',errors='replace'))
        await asyncio.wait_for(self.base.client.write_gatt_char(self.base.tx,data,response='write' in self.base.tx.properties),3)
    async def synchronize(self):
        # ATI is local. If an old monitor consumes its first letter, the remaining TI is invalid hex, never an OBD request.
        while not self.queue.empty():self.queue.get_nowait()
        self.base.synchronized=False;await self.write(b'ATI\r');deadline=time.monotonic()+3;prompt=False;quiet=False
        while time.monotonic()<deadline:
            try:data=await asyncio.wait_for(self.queue.get(),.15 if prompt else max(.001,deadline-time.monotonic()))
            except TimeoutError:
                if prompt:quiet=True;break
                raise ConnectionError('无法取得ELM提示符，停止测试，未发送车辆请求')
            prompt|=b'>' in data
        if not prompt or not quiet:raise ConnectionError('ELM同步失败：未观察到提示符后的安静窗口')
        self.base.synchronized=True;self.streaming=False
    async def command(self,cmd,required=True,timeout=2):
        if not allowed_command(cmd) or cmd=='ATMA':raise SafetyError(f'非监听配置命令已阻止：{cmd}')
        if self.streaming:raise SafetyError('监听中禁止发送AT命令，请先停止并同步提示符')
        answer=await self.base.command(cmd,timeout)
        result={'command':cmd,'response':answer,'required':required};self.command_results.append(result)
        if required and obd.clean_answer(answer,cmd)!='OK':raise SafetyError(f'{cmd} 未返回OK，停止该阶段：{answer.strip()}')
        return answer
    async def configure(self,protocol,phase):
        self.configured=False
        self.base.deadline=time.monotonic()+self.args.connection_timeout
        await self.synchronize()
        phase['elm']=obd.clean_answer(await self.command('ATZ',False,3),'ATZ')
        for cmd in ('ATE0','ATL0','ATS1','ATM0','ATH1','ATCSM1','ATCAF0','ATCFC0','ATD1' if self.args.dlc else 'ATD0',f'ATTP{protocol}'):
            await self.command(cmd)
        h=3 if protocol==6 else 8
        if self.args.filter_id is None:
            await self.command('ATCF'+'0'*h);await self.command('ATCM'+'0'*h)
        else:await self.command(f'ATCRA{self.args.filter_id:0{h}X}')
        phase['reported_protocol']=obd.clean_answer(await self.command('ATDPN',False),'ATDPN')
        if obd.protocol_number(phase['reported_protocol'])!=protocol:raise SafetyError('适配器返回的协议编号与固定配置不符')
        phase['protocol_description']=obd.clean_answer(await self.command('ATDP',False),'ATDP')
        phase['status_before']=await self.command('ATCS',False)
        phase['input_voltage']=obd.clean_answer(await self.command('ATRV',False),'ATRV')
        phase['configuration_verified']=True;self.configured=True;self.base.deadline=None
        phase['can_status_before']=parse_can_status(phase['status_before'])
    async def start(self):
        if not self.configured:raise SafetyError('静默配置尚未确认，禁止启动监听')
        if not self.base.synchronized:raise ConnectionError('ELM不同步，不能开始监听')
        self.prompt_seen=False;self.base.synchronized=False;self.streaming=True
        await self.write(b'ATMA\r')
    async def stop(self,on_data):
        if not self.streaming:return self.base.synchronized
        if self.prompt_seen:self.streaming=False;self.base.synchronized=True;return True
        while not self.queue.empty():
            pending=self.queue.get_nowait();on_data(pending,self.queue.last_received_ns,False)
            self.prompt_seen|=b'>' in pending
        if self.prompt_seen:self.streaming=False;self.base.synchronized=True;return True
        self.files.log('monitor_interrupt',reason='end of bounded capture');await self.write(b'x')
        deadline=time.monotonic()+self.args.stop_timeout
        while time.monotonic()<deadline:
            try:data=await asyncio.wait_for(self.queue.get(),max(.001,deadline-time.monotonic()))
            except TimeoutError:break
            on_data(data,self.queue.last_received_ns,False);self.prompt_seen|=b'>' in data
            if self.prompt_seen:self.streaming=False;self.base.synchronized=True;return True
        self.streaming=False;self.base.synchronized=False;self.files.log('stop_failed',reason='no prompt after interrupt');return False

ERROR_WORDS=('BUFFER FULL','CAN ERROR','BUS ERROR','RX ERROR','DATA ERROR','UNABLE TO CONNECT','BUS BUSY')

def parse_can_status(text):
    result={'raw':text.strip()}
    for key,label in [('T','tx_errors'),('R','rx_errors')]:
        match=re.search(rf'\b{key}:(OFF|[0-9A-F]+)',text.upper())
        if match:result[label]=None if match[1]=='OFF' else int(match[1],16);result[label+'_off']=match[1]=='OFF'
    frequency=re.search(r'\bF:(<250|500|250|0)',text.upper())
    if frequency:result['adapter_reported_frequency_kbit']=frequency[1]
    return result

def classify_phase(phase):
    if not phase['configuration_verified']:return '配置未完成，无法判定总线是否可监听'
    if phase['data_loss_possible']:return '采集不完整：存在适配器或接收队列丢帧迹象'
    if phase['frames']:
        if phase['diagnostic_shaped_frames']==phase['frames']:return '可见报文，但仅见诊断形态ID，尚不能确认所需传感器广播'
        return '可见被动CAN报文；信号含义与HUD适用性仍需解码验证'
    if phase['errors']:return '未见有效帧且适配器报错，无法据此判断车辆网关'
    if phase['unparsed_lines']:return '有模块输出但未解析成CAN帧，请检查原始格式，不能据此判定总线无帧'
    if phase['end_reason'] not in ('duration_elapsed','max_frames'):return '监听未完整结束，无法判断总线可见性'
    return '当前接口和车辆状态未观察到CAN帧；不能推断整车无广播'

async def capture_phase(session,phase,args,files,stop_event):
    framer=LineFramer();stats={};queue=session.queue;drop_start=queue.dropped;phase['start_time']=utc8();start=time.monotonic();phase['capture_start_ns']=time.monotonic_ns()
    def consume(data,received_ns,record=True):
        for kind,line in framer.feed(data):
            if kind=='prompt':session.prompt_seen=True;continue
            if kind=='oversize':phase['oversize_lines']+=1;phase['data_loss_possible']=True;continue
            normalized=line.strip().upper();phase['lines']+=1
            frame=parse_frame(line,phase['id_bits'],args.dlc) if record else None
            if frame:
                if args.max_frames and phase['frames']>=args.max_frames:continue
                phase['frames']+=1;phase['diagnostic_shaped_frames']+=int(diagnostic_shape(frame));phase['missing_dlc_frames']+=bool(frame.format_note)
                files.frame(frame,phase,received_ns)
                key=frame.key
                if key in stats:stats[key].add(frame.data,received_ns)
                elif len(stats)<args.max_ids:stats[key]=IdStats();stats[key].add(frame.data,received_ns)
                else:phase['untracked_id_frames']+=1
            else:
                if normalized in ('ATMA','OK','STOPPED',''):continue
                message={'line':line,'elapsed_ms':round((time.monotonic()-start)*1000,3)}
                if normalized=='?' or any(word in normalized for word in ERROR_WORDS):
                    phase['errors'].append(message);phase['data_loss_possible']|='BUFFER FULL' in normalized or 'RX ERROR' in normalized
                elif record:
                    phase['unparsed_lines']+=1
                    if len(phase['unparsed_examples'])<20:phase['unparsed_examples'].append(message)
                files.log('adapter_line',line=line)
    phase['_live_ids']=stats
    try:
        await session.start()
        while time.monotonic()-start<args.seconds and not stop_event.is_set():
            if files.error:raise OSError(files.error)
            if queue.dropped>drop_start:phase['data_loss_possible']=True;phase['end_reason']='queue_overflow';break
            try:data=await asyncio.wait_for(queue.get(),min(.25,max(.001,args.seconds-(time.monotonic()-start))))
            except TimeoutError:
                if not session.base.client.is_connected:phase['end_reason']='ble_disconnected';phase['errors'].append({'line':'BLE disconnected'});break
                continue
            consume(data,queue.last_received_ns)
            if session.prompt_seen:phase['end_reason']='adapter_returned_prompt';break
            if phase['errors']:phase['end_reason']='adapter_error';break
            if args.max_frames and phase['frames']>=args.max_frames:phase['end_reason']='max_frames';break
        else:phase['end_reason']='user_stop' if stop_event.is_set() else 'duration_elapsed'
    finally:
        phase['capture_seconds']=round(time.monotonic()-start,6)
        try:phase['stop_synchronized']=await session.stop(consume)
        except Exception as exc:phase['stop_synchronized']=False;phase['errors'].append({'line':f'Stop: {exc}'})
        if not phase.get('stop_synchronized'):phase['errors'].append({'line':'停止监听后未取得ELM提示符'})
        phase['queue_dropped']=queue.dropped-drop_start;phase['data_loss_possible']|=phase['queue_dropped']>0
        phase['unterminated_fragment']=framer.buffer.decode('ascii',errors='replace')[:200]
        phase['ids']={key:value.report(phase['capture_seconds']) for key,value in sorted(stats.items())}
        phase.pop('_live_ids',None)
        phase['completed']=phase['end_reason'] in ('duration_elapsed','max_frames') and phase['stop_synchronized'] and not phase['data_loss_possible'] and not phase['errors']
        phase['conclusion']=classify_phase(phase)
        if phase['stop_synchronized']:
            try:
                phase['status_after']=await session.command('ATCS',False);phase['can_status_after']=parse_can_status(phase['status_after'])
            except Exception as exc:phase.setdefault('warnings',[]).append(f'ATCS after capture unavailable: {exc}')

class KeyboardMarkers:
    LABELS={'1':'怠速或静止标记','2':'轻给油标记','3':'制动踏板标记','4':'松开踏板标记','m':'手动标记'}
    def __init__(self,files,event,enabled):self.files,self.event,self.enabled=files,event,enabled;self.old=None;self.fd=None;self.loop=None
    def start(self):
        if not self.enabled or not sys.stdin.isatty():return
        try:
            import termios,tty
            self.fd=sys.stdin.fileno();self.old=termios.tcgetattr(self.fd);tty.setcbreak(self.fd)
            self.loop=asyncio.get_running_loop();self.loop.add_reader(self.fd,self.read)
        except (ImportError,OSError,NotImplementedError):self.close()
    def read(self):
        for key in os.read(self.fd,64).decode('ascii',errors='ignore').lower():
            if key=='q':self.event.set();self.files.log('user_stop',key='q')
            elif key in self.LABELS:self.files.marker(self.LABELS[key])
    def close(self):
        if self.loop and self.fd is not None:self.loop.remove_reader(self.fd)
        if self.old is not None:
            import termios
            termios.tcsetattr(self.fd,termios.TCSADRAIN,self.old);self.old=None

class View:
    def __init__(self,enabled):self.tty=enabled and sys.stdout.isatty()
    def render(self,report,files):
        if not self.tty:return
        width,height=shutil.get_terminal_size((120,35));phases=report['phases'];phase=phases[-1] if phases else None
        lines=['奔驰 C260 2021 / CAN 被动监听测试',f'状态：{report["state"]} / 已用 {(time.monotonic_ns()-files.start_ns)/1e9:.1f}s',
               '固定500 kbit/s；ATCSM1静默；不发送OBD/UDS查询',f'保存：{files.path}']
        if phase:
            elapsed=(time.monotonic_ns()-phase.get('capture_start_ns',time.monotonic_ns()))/1e9
            lines += [f'阶段 {phase["index"]} / 协议 {phase["protocol"]} / {phase["id_bits"]}位 / {elapsed:.1f}s',
                      f'帧 {phase["frames"]} / 行 {phase["lines"]} / 错误 {len(phase["errors"])} / 队列丢包 {phase.get("queue_dropped",0)}',
                      'CAN ID       帧数     变化次数   最近数据']
            live=phase.get('_live_ids',{})
            for key,value in sorted(live.items(),key=lambda x:x[1].count,reverse=True)[:max(1,height-12)]:lines.append(f'{key:<12} {value.count:<8} {value.changes:<10} {value.last.hex(" ").upper()}')
        lines+=['按1静止、2给油、3制动、4松开、m标记、q结束；Ctrl+C也会保存',f'最近标记：{files.markers[-1]["label"] if files.markers else "--"}']
        sys.stdout.write('\033[H\033[2J'+'\n'.join(clip(line,max(1,width-1)) for line in lines[:max(1,height-1)])+'\n');sys.stdout.flush()

def new_phase(index,protocol):
    return {'index':index,'created_at':utc8(),'protocol':protocol,'id_bits':11 if protocol==6 else 29,'bitrate':500000,'configuration_verified':False,
            'frames':0,'lines':0,'diagnostic_shaped_frames':0,'missing_dlc_frames':0,'unparsed_lines':0,'oversize_lines':0,'untracked_id_frames':0,
            'errors':[],'unparsed_examples':[],'data_loss_possible':False,'completed':False,'end_reason':'not_started','capture_seconds':0,'ids':{}}

def export_report(report,files):
    report['markers']=files.markers;report['finished_at']=utc8();report['raw_log_error']=files.error
    for phase in report['phases']:phase.pop('_live_ids',None);phase.setdefault('conclusion',classify_phase(phase))
    temporary=files.path/'summary.json.tmp';temporary.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');temporary.replace(files.path/'summary.json')
    with (files.path/'can_ids.csv').open('w',encoding='utf8',newline='') as f:
        writer=csv.writer(f);writer.writerow(['phase','id_bits','can_id','count','observed_hz','payload_changes','last_data','variable_bytes','counter_candidate_bytes'])
        for phase in report['phases']:
            for key,stats in phase['ids'].items():writer.writerow([phase['index'],phase['id_bits'],key,stats['count'],stats['observed_hz_over_phase'],stats['payload_changes'],stats['last_data'],','.join(str(b['index']) for b in stats['bytes'] if b['variable_bit_mask']!='00'),','.join(str(b['index']) for b in stats['bytes'] if b['incrementing_counter_candidate'])])
    lines=['# 奔驰 CAN 被动监听报告','',f'- 工具版本：{VERSION}',f'- 用户标签：{report["label"]}',f'- 完成状态：{report["completed"]}',f'- 结束原因：{report["end_reason"]}',
           '- 本工具未发送车辆诊断请求，仅发送ELM本地配置和监听指令。','- ATDPN/ATDP仅证明模块配置；CAN帧可见性与信号含义分别判断。','',
           '|阶段|格式|帧数|唯一ID|时长/s|结论|','|---|---|---:|---:|---:|---|']
    for phase in report['phases']:lines.append(f'|{phase["index"]}|{phase["id_bits"]}位/500k|{phase["frames"]}|{len(phase["ids"])}|{phase["capture_seconds"]:.2f}|{phase["conclusion"]}|')
    for phase in report['phases']:
        lines += ['',f'## 阶段 {phase["index"]}：ID与数据变化','',f'ELM状态：前 {phase.get("status_before","--").strip()}；后 {phase.get("status_after","--").strip()}',
                  '', '|CAN ID|帧数|观测频率Hz|数据变化次数|最近数据|','|---|---:|---:|---:|---|']
        for key,s in sorted(phase['ids'].items(),key=lambda x:x[1]['count'],reverse=True)[:50]:lines.append(f'|{key}|{s["count"]}|{s["observed_hz_over_phase"]}|{s["payload_changes"]}|{s["last_data"]}|')
        if phase['errors']:lines+=['','适配器/采集错误：',*[f'- {e["line"]}' for e in phase['errors']]]
        if phase['missing_dlc_frames']:lines+=['',f'格式提醒：{phase["missing_dlc_frames"]} 帧未包含 ATD1 要求的 DLC，已按头部与原始字节解析，请对照 raw.jsonl。']
        if phase['untracked_id_frames']:lines+=['',f'统计容量提醒：{phase["untracked_id_frames"]} 帧因达到唯一 ID 上限未纳入逐 ID 统计；原始数据仍保留在 frames.csv 与 raw.jsonl。']
        if phase.get('warnings'):lines+=['','附加信息：',*[f'- {warning}' for warning in phase['warnings']]]
    lines += ['','## 解读边界','',
              '- 11位与29位是接收标识符格式，不代表两条不同物理总线。顺序测试时车辆状态可能变化。',
              '- 无帧不等于整车没有广播；有帧不等于已解码转速、车速或制动压力。',
              '- 诊断形态ID可能来自其他诊断设备或网关；单凭ID外观不能确定用途。',
              '- 时间来自主机接收BLE通知；批量传输会出现0间隔，不是车辆真实时间戳。频率是观测值，不是完整总线负载。',
              '- BUFFER FULL、接收溢出或异常结束会标记采集不完整；即使没有错误，也不能证明ELM/BLE没有内部丢帧。',
              '- 递增计数器候选仅是字节变化特征，不能据此认定某字节是速度或转速。',
              '- 软件要求ATCSM1且关闭自动流控，无法单凭模块返回OK独立证明其硬件完全遵守静默模式。']
    (files.path/'report.md').write_text('\n'.join(lines)+'\n')

async def run(args):
    files=RunFiles(args.output_dir);stop_event=asyncio.Event();keys=KeyboardMarkers(files,stop_event,not args.no_keys);view=View(not args.no_tui)
    report={'tool_version':VERSION,'started_at':utc8(),'target_vehicle':'Mercedes C260 2021 (user specified)','device':args.device,'label':args.label,
            'passive_only':True,'requested_protocols':args.protocols,'seconds_per_phase':args.seconds,'passes':args.passes,'state':'连接蓝牙','phases':[],'completed':False,'end_reason':'running',
            'host':{'python':platform.python_version(),'platform':platform.platform()},
            'gatt':{'service':obd.SERVICE,'notify':obd.RX,'write':obd.TX},
            'capture_settings':{'filter_id':args.filter_id,'dlc_requested':args.dlc,'queue_size':args.queue_size,'max_ids':args.max_ids,
                                'max_frames':args.max_frames,'connection_timeout_s':args.connection_timeout,'stop_timeout_s':args.stop_timeout},
            'reference':'https://www.elmelectronics.com/wp-content/uploads/2017/01/ELM327DS.pdf'}
    client=None;session=None
    async def work():
        nonlocal client,session
        for _ in range(args.passes):
            for protocol in args.protocols:
                phase=new_phase(len(report['phases'])+1,protocol);report['phases'].append(phase);files.phase=phase['index']
                if not client or not client.is_connected or (session and not session.base.synchronized):
                    if client and client.is_connected:await client.disconnect()
                    base=None;report['state']='连接IOS-Vlink'
                    client,base=await asyncio.wait_for(obd.connect(args,files.log),args.connection_timeout);session=MonitorSession(base,files,args)
                begin=len(session.command_results)
                try:
                    report['state']=f'配置{phase["id_bits"]}位静默监听';await session.configure(protocol,phase)
                    report['state']=f'监听{phase["id_bits"]}位CAN';await capture_phase(session,phase,args,files,stop_event)
                except asyncio.CancelledError:phase['end_reason']='cancelled';raise
                except Exception as exc:
                    phase['end_reason']='configuration_or_capture_error';phase['errors'].append({'line':f'{type(exc).__name__}: {exc}'});files.log('phase_error',error=str(exc))
                    if isinstance(exc,SafetyError):report['end_reason']='safety_configuration_failed';return
                finally:phase['configuration_commands']=session.command_results[begin:];phase.setdefault('conclusion',classify_phase(phase))
                if stop_event.is_set():return
        report['completed']=all(p['completed'] for p in report['phases']);report['end_reason']='completed' if report['completed'] else 'partial_or_failed'
    keys.start();files.marker(args.label)
    task=asyncio.create_task(work())
    try:
        while not task.done():
            view.render(report,files);files.flush()
            if stop_event.is_set():task.cancel();break
            await asyncio.sleep(.25)
        await task
        return 0 if report['completed'] and any(p['frames'] for p in report['phases']) else 2
    except asyncio.CancelledError:
        report['end_reason']='user_stop';report['state']='已停止并保存'
        if stop_event.is_set():return 130
        raise
    except Exception as exc:report['end_reason']=f'{type(exc).__name__}: {exc}';report['state']='测试失败';return 2
    finally:
        task.cancel()
        try:await task
        except BaseException:pass
        try:
            if session and session.streaming:
                try:await session.stop(lambda data,ts,record:None)
                except Exception:pass
            if client and client.is_connected:await asyncio.wait_for(client.disconnect(),5)
        finally:
            keys.close();report['state']='已保存';
            try:export_report(report,files)
            finally:files.close()
            print(f'\n监听测试结束：{report["end_reason"]}\n详细报告：{files.path/"report.md"}\nJSON汇总：{files.path/"summary.json"}',flush=True)

def hex_id(text):
    try:value=int(text,16)
    except ValueError as exc:raise argparse.ArgumentTypeError('CAN ID应为十六进制') from exc
    if not 0<=value<=0x1FFFFFFF:raise argparse.ArgumentTypeError('CAN ID超出29位范围')
    return value

def parse_args(argv=None):
    parser=argparse.ArgumentParser(description='奔驰 CAN 被动监听测试：仅配置 ELM327 并接收报文，不发送 OBD/UDS 请求。',
                                   epilog='默认顺序测试11位和29位、500 kbit/s，各30秒。输出详细报告、原始BLE日志和CAN帧CSV；按q或Ctrl+C停止并保存。')
    parser.add_argument('--device',default=obd.DEFAULT_DEVICE,help='IOS-Vlink BLE设备地址/本机UUID，默认按IOS-Vlink名称查找')
    parser.add_argument('--protocol',choices=['both','6','7'],default='both',help='6=11位/500k；7=29位/500k；默认分别测试')
    parser.add_argument('--seconds',type=float,default=30,help='每阶段监听秒数，默认30')
    parser.add_argument('--passes',type=int,default=1,help='完整格式测试轮数，默认1，最多20')
    parser.add_argument('--filter-id',type=hex_id,help='可选：仅监听指定十六进制CAN ID，用于缩小范围')
    parser.add_argument('--connection-timeout',type=float,default=60,help='BLE连接及每阶段初始化各自的时间上限（秒）')
    parser.add_argument('--stop-timeout',type=float,default=5,help='停止监听后等待ELM提示符的秒数，默认5')
    parser.add_argument('--queue-size',type=int,default=4096,help='接收通知队列容量，默认4096；溢出停止并标记丢帧')
    parser.add_argument('--max-ids',type=int,default=4096,help='每阶段逐ID统计容量，默认4096；不限制原始帧写盘')
    parser.add_argument('--max-frames',type=int,default=0,help='每阶段最多记录帧数，0为只按时间结束')
    parser.add_argument('--no-dlc',dest='dlc',action='store_false',help='仅在模块不支持ATD1时使用ATD0')
    parser.add_argument('--label',default='点火状态由用户确认',help='记录本次车辆状态标签，例如“点火ON-发动机怠速”')
    parser.add_argument('--no-keys',action='store_true',help='关闭键盘时间标记，Ctrl+C仍可结束')
    parser.add_argument('--no-tui',action='store_true',help='关闭终端原地刷新，仅保留日志与结束报告')
    parser.add_argument('--output-dir',type=Path,help='不存在的新输出目录，默认在项目上一级按时间创建')
    args=parser.parse_args(argv)
    if not 1<=args.seconds<=600 or not 1<=args.passes<=20 or not 5<=args.connection_timeout<=180 or not 1<=args.stop_timeout<=15:parser.error('seconds为1～600，passes为1～20，connection-timeout为5～180，stop-timeout为1～15')
    if not 128<=args.queue_size<=65536 or not 16<=args.max_ids<=16384 or not 0<=args.max_frames<=10_000_000:parser.error('队列／ID数量／帧数量超出允许范围')
    if args.protocol=='6' and args.filter_id is not None and args.filter_id>0x7FF:parser.error('11位模式不能使用超过7FF的过滤ID')
    args.protocols=[6,7] if args.protocol=='both' else [int(args.protocol)]
    if args.filter_id is not None and args.filter_id>0x7FF:args.protocols=[7]
    args.output_dir=args.output_dir or obd.ROOT.parent/('mercedes_can_monitor_'+datetime.now(TZ).strftime('%Y%m%d_%H%M%S_%f'))
    if args.output_dir.exists():parser.error('输出目录已存在，请换一个新目录，避免覆盖旧记录')
    return args

def main(argv=None):
    try:return asyncio.run(run(parse_args(argv)))
    except KeyboardInterrupt:print('\n用户中止，已尝试停止监听并保存。');return 130
    except (OSError,ValueError) as exc:print(f'测试无法完成：{exc}',file=sys.stderr);return 2

if __name__=='__main__':raise SystemExit(main())
