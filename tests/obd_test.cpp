#include "obd.hpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
using namespace hud;
static std::string hexByte(unsigned n){char b[4];snprintf(b,sizeof(b),"%02X",n);return b;}
static std::string frame(uint32_t source,const std::vector<uint8_t>& data){
    char h[16];snprintf(h,sizeof(h),source>0x7FF?"%08X":"%03X",unsigned(source));std::string out;
    if(data.size()<=7){out=h;out+=hexByte(data.size());for(auto c:data)out+=hexByte(c);for(unsigned i=data.size()+1;i<8;++i)out+="00";return out+"\r";}
    out=std::string(h)+hexByte(0x10|(data.size()>>8))+hexByte(data.size()&255);size_t i=0;
    for(;i<6;++i)out+=hexByte(data[i]);out+='\r';unsigned seq=1;
    while(i<data.size()){out+=std::string(h)+hexByte(0x20|(seq++&15));for(unsigned j=0;j<7;++j)out+=hexByte(i<data.size()?data[i++]:0);out+='\r';}return out;
}
struct Fake:ElmChannel{
    uint32_t clock=1000;uint8_t protocol=7,vehicleProtocol=7;bool headers=false,working=true,sleep=false,pressureReplies=false;std::string reject;std::vector<std::string> commands;
    uint32_t now()const override{return clock;}
    bool alive()const override{return working;}
    bool command(const std::string& cmd,std::string& answer,uint32_t)override{
        commands.push_back(cmd);clock+=20;answer="OK\r";
        if(cmd==reject){answer="?";return true;}
        if(cmd=="222002"&&protocol==6&&pressureReplies){std::vector<uint8_t> data(44);data[0]=0x62;data[1]=0x20;data[2]=2;data[15]=6;data[16]=0x8B;answer=frame(0x7EA,data);return true;}
        if(cmd=="ATZ"){answer="ELM327 test\r";headers=false;protocol=vehicleProtocol;}
        else if(cmd=="ATH0")headers=false;else if(cmd=="ATH1")headers=true;
        else if(cmd=="ATSP0")protocol=vehicleProtocol;
        else if(cmd.rfind("ATTP",0)==0)protocol=cmd.back()-'0';
        else if(cmd=="ATDPN")answer="A"+std::to_string(protocol)+"\r";
        else if(cmd=="ATDP")answer="ISO 15765-4 CAN\r";
        else if(cmd[0]!='A'){
            if(sleep||protocol!=vehicleProtocol){answer="NO DATA\r";return true;}
            std::vector<uint8_t> data;
            if(cmd=="0100")data={0x41,0,0x08,0x18,0,0};
            else if(cmd=="010D")data={0x41,0x0D,0};
            else if(cmd=="010C")data={0x41,0x0C,0x0B,0x88};
            else if(cmd=="0105")data={0x41,5,0x89};
            else if(cmd=="0900")data={0x49,0,0x40,0,0,0};
            else if(cmd=="0902"){data={0x49,2,1};std::string vin="1D4GP00R55B123456";data.insert(data.end(),vin.begin(),vin.end());}
            else {answer="NO DATA\r";return true;}
            if(headers&&protocol==5){std::vector<uint8_t> raw{uint8_t(0xC0|data.size()),0xF1,0x10};raw.insert(raw.end(),data.begin(),data.end());uint8_t sum=0;for(auto v:raw)sum+=v;raw.push_back(sum);answer.clear();for(auto v:raw)answer+=hexByte(v);answer+='\r';}
            else if(headers)answer=frame(0x18DAF110,data);else{answer.clear();for(auto b:data)answer+=hexByte(b);answer+='\r';}
        }return true;
    }
};
int main(int argc,char** argv){
    assert(argc==2);std::ifstream in(argv[1]);assert(in);std::string line;unsigned count=0;
    while(std::getline(in,line)){
        if(line.empty()||line[0]=='#')continue;std::istringstream row(line);std::string p,raw,source,data;
        std::getline(row,p,'|');std::getline(row,raw,'|');std::getline(row,source,'|');std::getline(row,data);
        for(size_t i=0;(i=raw.find("\\r",i))!=std::string::npos;)raw.replace(i,2,"\r");
        auto rs=parseReplies(raw,std::stoi(p));if(source=="-")assert(rs.empty());else{assert(rs.size()==1);assert(rs[0].source==std::stoul(source,nullptr,16));std::vector<uint8_t> bytes;assert(parseHex(data,bytes)&&bytes==rs[0].data);}++count;
    }
    assert(count>=12);
    for(uint8_t p=1;p<=9;++p){auto commands=protocolFormat(p);assert((commands.size()>1)==(p>=6));}
    Fake f;Snapshot s;ObdController c(f);assert(c.initialize(s,6));assert(s.mode==Mode::Live&&s.connection.info.detected==7);assert(c.cycle(s));
    assert(s.samples[Speed].valid&&s.samples[Speed].raw==0&&s.samples[Rpm].raw==738);
    assert(s.capabilities.status[Pressure]==MetricStatus::Unsupported);
    assert(std::find(f.commands.begin(),f.commands.end(),"ATSP0")!=f.commands.end());
    for(unsigned i=0;i<70;++i){f.clock+=100;assert(c.cycle(s));}
    assert(s.identity.verified&&s.identity.vin=="1D4GP00R55B123456"&&s.identity.model.empty());
    for(auto& cmd:f.commands)assert(cmd.rfind("22",0)!=0);
    c.refreshIdentity(s);assert(s.identity.vin.empty());
    auto before=s.connection.lastReply;f.sleep=true;f.clock+=16000;assert(c.cycle(s));assert(s.connection.state==ConnectionState::WaitingEcu&&!s.connection.ecu);
    assert(s.connection.lastReply==before);f.clock+=5000;assert(c.cycle(s));f.clock+=5000;assert(!c.cycle(s));
    Fake forced;Snapshot fixed;fixed.requestedProtocol=6;ObdController bad(forced);assert(!bad.initialize(fixed));assert(std::find(forced.commands.begin(),forced.commands.end(),"ATSP0")==forced.commands.end());
    Fake honda;honda.vehicleProtocol=5;Snapshot hs;selectProfile(hs,VehicleProfile::HondaCl300);ObdController hc(honda);assert(hc.initialize(hs));assert(hs.connection.info.detected==5&&hs.mode==Mode::Live&&hs.samples[Speed].raw==0);assert(hc.cycle(hs)&&hs.samples[Rpm].raw==738);
    assert(hs.capabilities.status[Pressure]==MetricStatus::Unsupported);assert(std::find(honda.commands.begin(),honda.commands.end(),"ATTP5")!=honda.commands.end());
    for(auto& cmd:honda.commands)assert(cmd!="ATSP0"&&cmd.rfind("22",0)!=0&&cmd.rfind("ATCRA",0)!=0&&cmd.rfind("ATCAF",0)!=0&&cmd.rfind("ATCFC",0)!=0&&cmd.rfind("ATSH",0)!=0);
    Fake mercedes;mercedes.pressureReplies=true;Snapshot ms;ms.profile=VehicleProfile::Mercedes;ms.page=PageId::Pedals;ObdController mc(mercedes);assert(mc.initialize(ms));
    for(unsigned i=0;i<120;++i){mercedes.clock+=100;assert(mc.cycle(ms));}
    assert(ms.samples[Pressure].valid&&ms.samples[Pressure].raw>25&&ms.samples[Pressure].raw<26);
    bool batch=false;for(size_t i=1;i<mercedes.commands.size();++i)batch|=mercedes.commands[i]=="222002"&&mercedes.commands[i-1]=="222002";assert(batch&&mercedes.protocol==7);
    auto standardStamp=ms.connection.lastReply;mercedes.sleep=true;mercedes.clock+=16000;assert(mc.cycle(ms));assert(!ms.connection.ecu&&ms.connection.lastReply==standardStamp);
    Fake transition;RouteManager route(7);route.reset(7);transition.reject="ATTP6";assert(!route.select(transition,true)&&route.protocol()==0);transition.reject.clear();assert(route.select(transition,false)&&route.protocol()==7);
    Fake cancelled;Snapshot cs;ObdController cc(cancelled);assert(cc.initialize(cs));cancelled.working=false;auto commandCount=cancelled.commands.size();assert(!cc.cycle(cs)&&cancelled.commands.size()==commandCount);
    VehicleIdentity identity;std::vector<uint8_t> d={0x49,2,1};std::string v="1D4GP00R55B123456";d.insert(d.end(),v.begin(),v.end());
    addIdentity(identity,{{0x7E8,d},{0x7E9,d}},2,6);assert(identity.verified&&!identity.conflict&&identity.records.size()==2);
    d.back()='8';addIdentity(identity,{{0x7E9,d}},2,6);assert(identity.conflict&&!identity.verified&&identity.vin=="身份信息冲突");
    VehicleIdentity invalid;d.pop_back();addIdentity(invalid,{{0x7E8,d}},2,6);assert(invalid.vin.empty());
    VehicleCapabilities caps;addCapabilities(caps,{{0x7E8,{0x41,0,0x08,0x18,0,1}},{0x7E9,{0x41,0,0,0,0,0}}},0);
    assert(declares(caps,0x0C,0x7E8)&&!declares(caps,0x0C,0x7E9));assert(!caps.ecus[0].known[0x2F]);
    PollScheduler scheduler;s.page=PageId::Pedals;auto scale=scheduler.scale(s);scheduler.result(Speed,true,f.clock,4000);assert(scheduler.scale(s)>scale);
    std::cout<<"PASS: "<<count<<" common wire fixtures, discovery/cache/forced protocol, source capabilities, identity conflict, sleep recovery\n";
}
