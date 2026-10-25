#include "core.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <map>
#include <sstream>

namespace hud {
uint8_t splashOpacity(uint32_t elapsed){
    if(elapsed>=SplashDurationMs)return 0;
    if(elapsed<SplashFadeMs)return uint8_t((elapsed*255+SplashFadeMs/2)/SplashFadeMs);
    if(elapsed>SplashDurationMs-SplashFadeMs)
        return uint8_t(((SplashDurationMs-elapsed)*255+SplashFadeMs/2)/SplashFadeMs);
    return 255;
}
uint32_t SplashTimeline::elapsed(uint32_t now) const{
    return started_?std::min(SplashDurationMs,uint32_t(now-start_)):0;
}
bool SplashTimeline::takeCompletion(uint32_t now){
    if(!started_||completed_||elapsed(now)<SplashDurationMs)return false;
    completed_=true;return true;
}
int effectiveBrightness(Mode mode,int saved){return mode==Mode::Splash?SplashBrightnessPercent:std::max(10,std::min(100,saved));}
uint32_t period(Metric m, PageId page) {
    return MetricDefinitions[m].periods[page==PageId::Pedals?1:0];
}
uint32_t pressureBurstPeriod(PageId page){return period(Pressure,page)*pressureBurstReads(page);}
unsigned pressureBurstReads(PageId page){return page==PageId::Pedals?2:1;}
uint32_t expiry(Metric m, PageId page) {
    if (m == Coolant) return 15000;
    if (m == Fuel) return 30000;
    if (m == Odometer) return 90000;
    return std::max(3000u, period(m, page) * 3);
}
int batteryPercent(double volts){
    // Approximate resting curve for a conventional 4.2 V single-cell Li-ion pack.
    static constexpr double v[]={3.20,3.40,3.60,3.70,3.75,3.80,3.85,3.90,4.00,4.10,4.20};
    static constexpr int pct[]={0,5,10,20,30,40,50,60,75,90,100};
    if(!std::isfinite(volts)||volts<=v[0])return 0;
    for(unsigned i=1;i<sizeof(v)/sizeof(v[0]);++i)
        if(volts<v[i])return int(std::lround(pct[i-1]+(pct[i]-pct[i-1])*(volts-v[i-1])/(v[i]-v[i-1])));
    return 100;
}
void updateBattery(BatteryInfo& b,double volts,bool charging,uint32_t now){
    if(!std::isfinite(volts)||volts<2.5||volts>4.35){b=BatteryInfo{};b.stamp=now;return;}
    double dt=double(uint32_t(now-b.stamp))/1000;
    b.volts=b.valid?b.volts+(volts-b.volts)*(1-std::exp(-dt/10.0)):volts;
    b.percent=batteryPercent(b.volts);b.charging=charging;b.valid=true;b.stamp=now;
}
uint64_t roundedKm(double km) { return km >= 0 && std::isfinite(km) ? uint64_t(std::floor(km + 0.5)) : 0; }
void update(Sample& s, double value, uint32_t now, uint32_t source) {
    if (!std::isfinite(value)) return;
    if (!s.valid) s.shown = value;
    s.raw = value; s.stamp = now; s.source = source; s.valid = true;
}
void invalidate(Snapshot& s) { for (auto& v : s.samples) v.valid = false; }
void animate(Snapshot& s, uint32_t now, uint32_t dt) {
    constexpr double tau[Count] = {120,100,2000,20000,0,80,120,40};
    for (unsigned i=0; i<Count; ++i) {
        auto& v=s.samples[i];
        if (!v.valid) continue;
        // A worker can publish a sample a few milliseconds after the UI captured `now`.
        if (!s.demo && int32_t(now-v.stamp)>int32_t(expiry(Metric(i),s.page)*s.samplingScale)) { v.valid=false; continue; }
        if (i==Odometer || ((i==Speed || i==Rpm) && v.raw==0)) { v.shown=v.raw; continue; }
        double t=(i==Pressure && v.raw<v.shown) ? 80 : tau[i];
        v.shown += (v.raw-v.shown)*(1-std::exp(-double(dt)/t));
    }
}
double DemoGenerator::noise() {
    seed_^=seed_<<13; seed_^=seed_>>17; seed_^=seed_<<5;
    return double(seed_&65535)/32767.5-1;
}
DemoGenerator::DemoGenerator(uint32_t seed):seed_(seed?seed:2020){
    km_=40000+std::floor((noise()+1)*45000);
    fuel_=25+(noise()+1)*30;
    cruise_=70+(noise()+1)*12;
    coolant_=92+(noise()+1)*3;
    brake_=16+(noise()+1)*4;
    elapsed_=8+(noise()+1)*6;
}
void DemoGenerator::tick(Snapshot& s, uint32_t now, double dt) {
    elapsed_+=dt;
    double p=std::fmod(elapsed_,60), speed=0, pedal=0, pressure=0;
    if(p<8) { speed=0; pedal=0; pressure=18+noise(); }
    else if(p<25) { double u=(p-8)/17; speed=cruise_*(u*u*(3-2*u)); pedal=25+12*std::sin(u*3.14159265); }
    else if(p<40) { speed=cruise_+3*std::sin((p-25)/3); pedal=19+3*std::sin(p/2); }
    else if(p<53) { double u=(p-40)/13; speed=cruise_*(1-u*u*(3-2*u)); pressure=brake_*std::sin(u*3.14159265); }
    else { speed=0; pressure=15; }
    double rpm=speed<1 ? 735+noise()*4 : 1150+std::fmod(speed,18)*65+pedal*11;
    km_+=speed*dt/3600; fuel_=std::max(0.0,fuel_-pedal*dt*0.000015);
    double vals[Count]={speed,rpm,coolant_+2*std::sin(elapsed_/35),fuel_,km_,pedal,13+pedal*.7,pressure};
    s.demo=true;
    for(unsigned i=0;i<Count;++i) {if(i==Pressure&&!profileHasPressure(s.profile)){s.samples[i].valid=false;s.capabilities.status[i]=MetricStatus::Unsupported;continue;}update(s.samples[i],vals[i],now);s.capabilities.status[i]=MetricStatus::Valid;}
}
static int hex(char c) { if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
bool parseHex(const std::string& s, std::vector<uint8_t>& out) {
    out.clear(); if(s.size()%2) return false;
    for(size_t i=0;i<s.size();i+=2) { int a=hex(s[i]),b=hex(s[i+1]); if(a<0||b<0)return false; out.push_back((a<<4)|b); }
    return true;
}
std::vector<Reply> parseReplies(const std::string& text,uint8_t protocol) {
    struct Assembly { size_t size=0; unsigned sequence=1; bool active=false; std::vector<uint8_t> bytes; };
    std::map<uint32_t,Assembly> assemblies;
    std::vector<Reply> out;
    std::string line;
    std::string normalized=text;
    std::replace(normalized.begin(),normalized.end(),'\n','\r');
    std::istringstream lines(normalized);
    while(std::getline(lines,line,'\r')) {
        std::string s;
        for(unsigned char c:line) if(!std::isspace(c)&&c!='>') s+=char(std::toupper(c));
        if(s.empty()) continue;
        std::vector<uint8_t> bytes;
        if(protocol==0&&(s.rfind("41",0)==0||s.rfind("49",0)==0) && parseHex(s,bytes)) { out.push_back({0,bytes}); continue; }
        if(protocol>=1&&protocol<=5){
            if(!parseHex(s,bytes)||bytes.size()<6)continue;
            unsigned header=3;
            if(protocol==4||protocol==5){
                if(!(bytes[0]&0xC0))continue;
                if(!(bytes[0]&0x3F))header=4;
                if(bytes.size()<header+2)continue;
                unsigned length=header==4?bytes[3]:(bytes[0]&0x3F);
                if(length!=bytes.size()-header-1)continue;
            }
            uint8_t sum=0;
            if(protocol<=2){sum=0xFF;for(size_t i=0;i+1<bytes.size();++i){sum^=bytes[i];for(int bit=0;bit<8;++bit)sum=(sum&0x80)?uint8_t((sum<<1)^0x1D):uint8_t(sum<<1);}sum^=0xFF;}
            else for(size_t i=0;i+1<bytes.size();++i)sum=uint8_t(sum+bytes[i]);
            if(sum!=bytes.back())continue;
            out.push_back({bytes[2],{bytes.begin()+header,bytes.end()-1}});continue;
        }
        size_t h=(protocol==7||protocol==9||s.rfind("18DA",0)==0||s.rfind("18DB",0)==0)?8:3;
        if(s.size()<=h+1 || (s.size()-h)%2)continue;
        uint32_t id=0; bool ok=true;
        for(size_t i=0;i<h;++i) { int n=hex(s[i]); if(n<0){ok=false;break;} id=(id<<4)|n; }
        if((h==3&&id>0x7FF)||(h==8&&id>0x1FFFFFFF))continue;
        if(!ok || !parseHex(s.substr(h),bytes) || bytes.empty() || bytes.size()>8)continue;
        unsigned type=bytes[0]>>4;
        auto& a=assemblies[id];
        if(type==0) {
            size_t n=bytes[0]&15;
            if(n>0&&n<=7&&bytes.size()>=n+1)out.push_back({id,{bytes.begin()+1,bytes.begin()+1+n}});
        } else if(type==1&&bytes.size()==8) {
            a=Assembly{}; a.size=((bytes[0]&15)<<8)|bytes[1];
            a.active=a.size>7&&a.size<=512;
            if(a.active)a.bytes.assign(bytes.begin()+2,bytes.end());
        } else if(type==2 && a.active) {
            if((bytes[0]&15)!=(a.sequence&15)){a.active=false;continue;}
            ++a.sequence; a.bytes.insert(a.bytes.end(),bytes.begin()+1,bytes.end());
            if(a.bytes.size()>=a.size) { a.bytes.resize(a.size);out.push_back({id,a.bytes});a.active=false; }
        }
    }
    return out;
}
std::vector<uint8_t> supported(const std::vector<Reply>& replies,uint8_t base) {
    uint32_t bits=0;
    for(auto& r:replies)if(r.data.size()>=6&&r.data[0]==0x41&&r.data[1]==base)
        bits|=uint32_t(r.data[2])<<24|uint32_t(r.data[3])<<16|uint32_t(r.data[4])<<8|r.data[5];
    std::vector<uint8_t> out;
    for(unsigned bit=0;bit<32;++bit)if(bits&(1u<<(31-bit)))out.push_back(base+bit+1);
    return out;
}
bool decode(const Reply& r, Metric m,uint8_t pid,double& value) {
    auto& d=r.data;
    if(m==Pressure) {
        if(r.source!=PressureRx||d.size()<3+PressureOffset+2||!std::equal(std::begin(PressurePrefix),std::end(PressurePrefix),d.begin()))return false;
        int n=(int(d[3+PressureOffset])<<8)|d[4+PressureOffset]; if(n&0x8000)n-=65536;
        value=n/PressureDivisor;return true;
    }
    size_t size=m==Rpm?4:m==Odometer?6:3;
    if(d.size()<size||d[0]!=0x41||d[1]!=pid)return false;
    if(m==Rpm)value=((d[2]<<8)|d[3])/4.0;
    else if(m==Speed)value=d[2];
    else if(m==Coolant)value=int(d[2])-40;
    else if(m==Odometer)value=(uint32_t(d[2])<<24|uint32_t(d[3])<<16|uint32_t(d[4])<<8|d[5])/10.0;
    else value=d[2]*100.0/255;
    return true;
}
}
