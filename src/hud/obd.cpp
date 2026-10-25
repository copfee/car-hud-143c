#include "obd.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <map>

namespace hud {
std::string cleanAnswer(const std::string& answer,const std::string& echo){
    std::string a=answer;std::replace(a.begin(),a.end(),'\r','\n');std::istringstream in(a);std::string line,result;
    while(std::getline(in,line)){
        line.erase(std::remove_if(line.begin(),line.end(),[](unsigned char c){return c<32||c==127||c=='>'; }),line.end());
        auto b=line.find_first_not_of(' '),e=line.find_last_not_of(' ');if(b==std::string::npos)continue;
        line=line.substr(b,e-b+1);if(line!=echo)result=line;
    }return result;
}
uint8_t protocolNumber(const std::string& answer){
    auto s=cleanAnswer(answer,"ATDPN");s.erase(std::remove_if(s.begin(),s.end(),[](unsigned char c){return std::isspace(c);}),s.end());
    if(s.size()==2&&(s[0]=='A'||s[0]=='a'))s.erase(0,1);
    return s.size()==1&&s[0]>='1'&&s[0]<='9'?s[0]-'0':0;
}
static bool at(ElmChannel& c,const std::string& cmd){std::string r;return c.command(cmd,r,1800)&&cleanAnswer(r,cmd)=="OK";}
std::vector<std::string> protocolFormat(uint8_t p){
    if(p>=6&&p<=9)return {"ATCAF1","ATCFC1","ATD0","ATCRA","ATH1"};
    return {"ATH1"};
}
std::vector<std::string> routeCommands(uint8_t standard,bool pressure){
    if(pressure)return {"ATPC","ATTP"+std::to_string(PressureProtocol),std::string("ATSH")+PressureTx,PressureReceiveCommand};
    if(standard>=6)return {"ATPC","ATTP"+std::to_string(standard),(standard==7||standard==9)?"ATSH18DB33F1":"ATSH7DF","ATCRA"};
    return {"ATPC","ATTP"+std::to_string(standard)};
}
bool ProtocolStrategy::configure(ElmChannel& c,uint8_t p){for(auto& cmd:protocolFormat(p))if(!at(c,cmd))return false;return true;}
bool ProtocolStrategy::discover(ElmChannel& c,Snapshot& s,uint8_t cached){
    if(cached>9)cached=0;
    std::string r;
    if(!c.command("ATZ",r,1800))return false;
    snprintf(s.connection.elm,sizeof(s.connection.elm),"%s",cleanAnswer(r,"ATZ").c_str());
    for(auto cmd:{"ATE0","ATL0","ATS1","ATM0","ATH0"})if(!at(c,cmd))return false;
    s.connection.state=ConnectionState::Discovering;s.status="正在识别车辆协议";
    auto valid=[](const std::string& a){auto rs=parseReplies(a);return std::any_of(rs.begin(),rs.end(),[](const Reply& r){return r.data.size()==6&&r.data[0]==0x41&&r.data[1]==0;});};
    bool found=false;
    if(s.requestedProtocol){
        if(!at(c,"ATTP"+std::to_string(s.requestedProtocol))||!c.command("0100",r,10000))return false;
        found=valid(r);
    }else{
        if(cached){
            auto start=c.now();if(!at(c,"ATTP"+std::to_string(cached)))return false;auto used=uint32_t(c.now()-start);
            if(used>=CachedTimeoutMs||!c.command("0100",r,CachedTimeoutMs-used))return false;
            found=valid(r);
        }
        if(!found){if(!at(c,"ATSP0")||!c.command("0100",r,DiscoveryTimeoutMs))return false;found=valid(r);}
    }
    if(!found)return false;
    if(!c.command("ATDPN",r,1800))return false;
    auto p=protocolNumber(r);if(!p||(s.requestedProtocol&&p!=s.requestedProtocol))return false;
    s.connection.info.detected=p;s.connection.info.configured=s.requestedProtocol;
    snprintf(s.connection.protocol,sizeof(s.connection.protocol),"%s",cleanAnswer(r,"ATDPN").c_str());
    if(!c.command("ATDP",r,1800))return false;
    snprintf(s.connection.info.description,sizeof(s.connection.info.description),"%s",cleanAnswer(r,"ATDP").c_str());
    if(!configure(c,p)||!c.command("0100",r,3000))return false;
    auto replies=parseReplies(r,p);addCapabilities(s.capabilities,replies,0);
    if(s.capabilities.ecus.empty())return false;
    s.connection.info.verified=true;s.connection.info.active=p;s.connection.activeProtocol=p;
    s.connection.ecu=true;s.connection.lastReply=c.now();return true;
}
bool RouteManager::select(ElmChannel& c,bool pressure){
    if(valid_&&pressure_==pressure)return true;
    valid_=false;
    for(auto& cmd:routeCommands(standard_,pressure))if(!at(c,cmd))return false;
    valid_=true;pressure_=pressure;active_=pressure?PressureProtocol:standard_;return true;
}
void addCapabilities(VehicleCapabilities& caps,const std::vector<Reply>& replies,uint8_t base){
    for(auto& r:replies){
        if(r.data.size()!=6||r.data[0]!=0x41||r.data[1]!=base)continue;
        auto it=std::find_if(caps.ecus.begin(),caps.ecus.end(),[&](const EcuCapabilities& e){return e.source==r.source;});
        if(it==caps.ecus.end()){if(caps.ecus.size()>=8)continue;caps.ecus.push_back({});it=caps.ecus.end()-1;it->source=r.source;}
        uint32_t bits=(uint32_t(r.data[2])<<24)|(uint32_t(r.data[3])<<16)|(uint32_t(r.data[4])<<8)|r.data[5];
        for(unsigned i=1;i<=32&&unsigned(base)+i<256;++i){it->known[base+i]=true;it->supported[base+i]=bool(bits&(1u<<(32-i)));}
    }
}
bool declares(const VehicleCapabilities& caps,uint8_t pid,uint32_t source){
    for(auto& e:caps.ecus)if((!source||e.source==source)&&e.supported[pid])return true;return false;
}
static bool validVin(const std::string& vin){
    if(vin.size()!=17)return false;
    for(char c:vin)if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z'&&c!='I'&&c!='O'&&c!='Q')))return false;
    return true;
}
void addIdentity(VehicleIdentity& identity,const std::vector<Reply>& replies,uint8_t pid,uint8_t protocol){
    std::map<uint32_t,std::vector<Reply>> bySource;
    for(auto& r:replies)if(r.data.size()>3&&r.data[0]==0x49&&r.data[1]==pid)bySource[r.source].push_back(r);
    for(auto& entry:bySource){
        auto rs=entry.second;std::vector<uint8_t> bytes;
        if(protocol<=5){
            std::sort(rs.begin(),rs.end(),[](const Reply& a,const Reply& b){return a.data[2]<b.data[2];});
            unsigned expected=1;bool bad=false;
            for(auto& r:rs){if(r.data[2]!=expected++){bad=true;break;}bytes.insert(bytes.end(),r.data.begin()+3,r.data.end());}
            if(bad)continue;
        }else{if(rs.size()!=1||rs[0].data[2]!=1)continue;bytes.assign(rs[0].data.begin()+3,rs[0].data.end());}
        while(!bytes.empty()&&bytes.front()==0)bytes.erase(bytes.begin());
        while(!bytes.empty()&&(bytes.back()==0||bytes.back()==0xFF||bytes.back()==' '))bytes.pop_back();
        std::string text;for(auto c:bytes)text+=c>=32&&c<127?char(c):'?';
        if(pid==2&&!validVin(text))continue;
        if(pid==0xA&&(text.empty()||text.size()>80))continue;
        auto it=std::find_if(identity.records.begin(),identity.records.end(),[&](const IdentityRecord& r){return r.source==entry.first;});
        if(it==identity.records.end()){if(identity.records.size()>=8)continue;identity.records.push_back({});it=identity.records.end()-1;it->source=entry.first;}
        if(pid==2)it->vin=text;else it->ecu=text;
    }
    identity.vin.clear();identity.ecu.clear();identity.detail.clear();identity.conflict=false;
    for(auto& r:identity.records){
        char source[20];snprintf(source,sizeof(source),"ECU %X",unsigned(r.source));
        if(!r.vin.empty()){if(identity.vin.empty())identity.vin=r.vin;else if(identity.vin!=r.vin)identity.conflict=true;}
        if(!r.ecu.empty()){if(!identity.ecu.empty())identity.ecu+='\n';identity.ecu+=std::string(source)+" / "+r.ecu;}
        if(!identity.detail.empty())identity.detail+='\n';identity.detail+=source;
        if(!r.vin.empty())identity.detail+=" / "+r.vin;
    }
    identity.verified=!identity.vin.empty()&&!identity.conflict;
    if(identity.conflict){identity.vin="身份信息冲突";identity.detail="不同 ECU 返回不同 VIN\n"+identity.detail;}
    if(identity.detail.empty())identity.detail="未获得有效身份信息";
}
double PollScheduler::scale(const Snapshot& s)const{
    double duty=0;for(unsigned i=0;i<Count;++i)if(s.capabilities.status[i]!=MetricStatus::Unsupported&&s.capabilities.status[i]!=MetricStatus::Unknown)duty+=cost_[i]/period(Metric(i),s.page);
    return std::max(1.0,duty/.6);
}
int PollScheduler::select(const Snapshot& s,uint32_t now,bool pressureAllowed)const{
    int selected=-1;double highest=0,stretch=scale(s);
    for(unsigned i=0;i<Count;++i){
        if(i==Pressure&&!pressureAllowed)continue;
        auto st=s.capabilities.status[i];if(st==MetricStatus::Unknown||st==MetricStatus::Unsupported)continue;
        if(next_[i]&&int32_t(now-next_[i])<0)continue;
        double score=sent_[i]?double(uint32_t(now-sent_[i]))/(period(Metric(i),s.page)*stretch):100;
        if(score>=1&&score>highest){highest=score;selected=i;}
    }return selected;
}
void PollScheduler::result(Metric m,bool ok,uint32_t now,uint32_t elapsed){
    sent_[m]=now;if(elapsed)recordCost(m,elapsed);
    if(ok){failures_[m]=0;next_[m]=0;}else{unsigned n=++failures_[m];next_[m]=now+(n==1?5000:n==2?10000:30000);}
}
bool ObdController::read(Snapshot& s,Metric m,bool restore){
    uint32_t start=channel_.now();bool pressure=m==Pressure;std::string answer;
    s.connection.activeProtocol=0;
    if(!route_.select(channel_,pressure)){
        s.samples[m].valid=false;s.capabilities.status[m]=MetricStatus::NoData;
        scheduler_.result(m,false,channel_.now(),pressure?0:uint32_t(channel_.now()-start));
        if(pressure&&channel_.alive()&&route_.select(channel_,false)){s.connection.activeProtocol=route_.protocol();return false;}
        return false;
    }
    s.connection.activeProtocol=route_.protocol();s.connection.info.active=route_.protocol();
    char cmd[12];if(pressure)snprintf(cmd,sizeof(cmd),"%s",PressureCommand);else snprintf(cmd,sizeof(cmd),"01%02X",pids_[m]);
    if(!channel_.command(cmd,answer,1800))return false;
    auto rs=parseReplies(answer,route_.protocol());std::sort(rs.begin(),rs.end(),[](const Reply& a,const Reply& b){return a.source<b.source;});
    if(scheduler_.failures(m)>=3)selected_[m]=false;
    bool ok=false;double value=0;
    for(auto& r:rs){
        if(!pressure&&!declares(s.capabilities,pids_[m],r.source))continue;
        if(selected_[m]&&r.source!=source_[m])continue;
        if(!decode(r,m,pids_[m],value))continue;
        source_[m]=r.source;selected_[m]=true;ok=true;update(s.samples[m],value,channel_.now(),r.source);break;
    }
    if(!ok)s.samples[m].valid=false;
    s.capabilities.status[m]=ok?MetricStatus::Valid:MetricStatus::NoData;
    if(ok&&!pressure){lastStandard_=channel_.now();everData_=true;s.connection.ecu=true;s.connection.lastReply=lastStandard_;}
    if(pressure&&restore&&!route_.select(channel_,false))return false;
    s.connection.activeProtocol=route_.protocol();s.connection.info.active=route_.protocol();
    scheduler_.result(m,ok,channel_.now(),pressure?0:uint32_t(channel_.now()-start));return ok;
}
bool ObdController::initialize(Snapshot& s,uint8_t cached){
    s.capabilities={};s.identity={};invalidate(s);s.connection.state=ConnectionState::Initializing;
    if(!ProtocolStrategy::discover(channel_,s,cached))return false;
    route_.reset(s.connection.info.detected);s.connection.state=ConnectionState::Capabilities;s.status="正在读取车辆数据";
    for(unsigned base=0x20;base<=0xA0&&declares(s.capabilities,base);base+=0x20){
        std::string answer;char cmd[8];snprintf(cmd,sizeof(cmd),"01%02X",base);
        if(!channel_.command(cmd,answer,2500))return false;addCapabilities(s.capabilities,parseReplies(answer,route_.protocol()),base);
    }
    for(unsigned i=0;i<Pressure;++i){
        bool known=true;for(unsigned j=0;j<MetricDefinitions[i].count;++j){auto pid=MetricDefinitions[i].pids[j];if(declares(s.capabilities,pid)){pids_[i]=pid;break;}for(auto& e:s.capabilities.ecus)known&=e.known[pid];}
        s.capabilities.status[i]=pids_[i]?MetricStatus::NoData:known?MetricStatus::Unsupported:MetricStatus::Unknown;
    }
    s.capabilities.status[Pressure]=profileHasPressure(s.profile)&&route_.protocol()==PressureStandardProtocol?MetricStatus::NoData:MetricStatus::Unsupported;
    for(unsigned i=0;i<Pressure;++i)if(pids_[i]){read(s,Metric(i));if(!channel_.alive())return false;if(everData_)break;}
    if(!everData_)return false;
    s.mode=Mode::Live;s.connection.state=ConnectionState::Running;s.status.clear();lastHeartbeat_=channel_.now();return true;
}
bool ObdController::identity(Snapshot& s){
    if(!identityWanted_||(identityDue_&&int32_t(channel_.now()-identityDue_)<0))return true;
    if(!route_.select(channel_,false))return false;
    std::string answer;uint8_t pid=identityStep_==0?0:identityStep_==1?2:0xA;
    bool send=pid==0||(pid==2&&identityVin_)||(pid==0xA&&identityName_);
    if(send){char cmd[8];snprintf(cmd,sizeof(cmd),"09%02X",pid);if(!channel_.command(cmd,answer,3000))return false;
        auto rs=parseReplies(answer,route_.protocol());
        if(pid==0){bool known=false;identityVin_=identityName_=false;for(auto& r:rs)if(r.data.size()==6&&r.data[0]==0x49&&r.data[1]==0){known=true;uint32_t bits=(uint32_t(r.data[2])<<24)|(uint32_t(r.data[3])<<16)|(uint32_t(r.data[4])<<8)|r.data[5];identityVin_|=bits&(1u<<30);identityName_|=bits&(1u<<22);}if(!known)identityVin_=true;if(known&&!identityVin_)s.identity.detail="车辆未声明支持 VIN";}
        else addIdentity(s.identity,rs,pid,route_.protocol());
    }
    if(++identityStep_>=3){identityStep_=0;++identityRound_;if(s.identity.verified||identityRound_>=2||!identityVin_)identityWanted_=false;else identityDue_=channel_.now()+IdentityRetryMs;}
    return true;
}
bool ObdController::cycle(Snapshot& s){
    auto now=channel_.now();if(!channel_.alive())return false;
    if(uint32_t(now-lastStandard_)>=SilenceTimeoutMs){
        s.connection.ecu=false;s.connection.state=ConnectionState::WaitingEcu;s.status="等待车辆唤醒";
        if(uint32_t(now-lastHeartbeat_)<HeartbeatIntervalMs)return true;
        lastHeartbeat_=now;std::string answer;if(!route_.select(channel_,false)||!channel_.command("0100",answer,2500))return false;
        auto rs=parseReplies(answer,route_.protocol());bool valid=false;for(auto& r:rs)valid|=r.data.size()==6&&r.data[0]==0x41&&r.data[1]==0;
        if(!valid)return ++heartbeatFailures_<3;
        lastStandard_=now;heartbeatFailures_=0;scheduler_.wake();s.connection.ecu=true;s.connection.lastReply=now;
    }
    s.connection.state=ConnectionState::Running;s.samplingScale=scheduler_.scale(s);
    int job=scheduler_.select(s,now,uint32_t(now-lastPressure_)>=uint32_t(pressureBurstPeriod(s.page)*s.samplingScale));
    // Alternate identity jobs with normal polling. Identity never gates the first live frame.
    if(identityWanted_&&(!identityDue_||int32_t(now-identityDue_)>=0)&&uint32_t(now-lastHeartbeat_)>=1000){lastHeartbeat_=now;if(!identity(s))return false;}
    else if(job>=0){
        auto m=Metric(job);auto groupStart=channel_.now();unsigned reads=1;
        read(s,m,m!=Pressure);if(!channel_.alive())return false;
        if(m==Pressure){lastPressure_=now;if(scheduler_.failures(m)==0&&pressureBurstReads(s.page)>1){++reads;read(s,m,false);if(!channel_.alive())return false;}if(!route_.select(channel_,false))return false;s.connection.activeProtocol=route_.protocol();s.connection.info.active=route_.protocol();scheduler_.recordCost(Pressure,uint32_t(channel_.now()-groupStart)/reads);}
    }
    return channel_.alive();
}
}
