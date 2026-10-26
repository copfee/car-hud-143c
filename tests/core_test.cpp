#include "obd.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace hud;
int main(){
    assert(ConnectionTimeoutMs==60000&&DiscoveryTimeoutMs==60000);
    assert(SplashDurationMs==3000&&SplashFadeMs==300);
    assert(splashOpacity(0)==0&&splashOpacity(150)==128&&splashOpacity(300)==255);
    assert(splashOpacity(2699)==255&&splashOpacity(2700)==255&&splashOpacity(2850)==128);
    assert(splashOpacity(3000)==0&&splashOpacity(3001)==0&&splashOpacity(0xffffffff)==0);
    for(uint32_t t=1;t<=300;++t)assert(splashOpacity(t)>=splashOpacity(t-1));
    for(uint32_t t=2701;t<=3000;++t)assert(splashOpacity(t)<=splashOpacity(t-1));
    SplashTimeline startup;assert(!startup.takeCompletion(5000)&&startup.elapsed(5000)==0);
    startup.start(1000);startup.start(2000); // An accidental second start cannot extend the animation.
    assert(startup.elapsed(1000)==0&&startup.elapsed(3999)==2999&&!startup.takeCompletion(3999));
    assert(startup.takeCompletion(4000)&&!startup.takeCompletion(4001)&&startup.elapsed(9000)==3000);
    SplashTimeline wrapped;wrapped.start(0xfffffff0);
    assert(wrapped.elapsed(2983)==2999&&!wrapped.takeCompletion(2983)&&wrapped.takeCompletion(2984));
    Snapshot boot;assert(boot.mode==Mode::Splash&&!connectionAllowed(boot.mode));
    assert(!connectionTimedOut(boot.mode,100000,0));
    assert(connectionAllowed(Mode::Loading)&&connectionAllowed(Mode::Timeout)&&!connectionAllowed(Mode::Live)&&!connectionAllowed(Mode::Demo));
    assert(!connectionAllowed(Mode::Bluetooth)&&!connectionTimedOut(Mode::Bluetooth,100000,0));
    BluetoothInfo bluetooth;assert(!validPeer(bluetooth.saved)&&!validPeer(bluetooth.target));
    BlePeer peer{"AA:BB:CC:DD:EE:FF","IOS-Vlink",1,-45,true};assert(validPeer(peer));
    auto stored=encodePeer(peer);auto restored=decodePeer(stored);assert(samePeer(peer,restored)&&restored.name==peer.name&&restored.addressType==1);
    assert(encodePeer(restored)==stored&&restored.rssi==0);
    for(const auto& bad:{std::string{},std::string("garbage"),std::string("2\n1\naa:bb:cc:dd:ee:ff\nAdapter"),std::string("1\n4\naa:bb:cc:dd:ee:ff\nAdapter"),std::string("1\n1\n00:00:00:00:00:00\nAdapter")})assert(!validPeer(decodePeer(bad)));
    auto invalid=peer;invalid.address="AA:BB:CC:DD:EE";assert(!validPeer(invalid)&&encodePeer(invalid).empty()&&!chooseBluetoothPeer(bluetooth,invalid));
    assert(cleanDeviceName("Adapter\n\r\t")=="Adapter   "&&cleanDeviceName("")=="未命名设备");
    assert(cleanDeviceName(std::string(63,'A')+"中")==std::string(63,'A'));
    assert(cleanDeviceName(std::string("bad\xff",4))=="bad?");
    addBluetoothDevice(bluetooth,peer);auto duplicate=peer;duplicate.rssi=-30;duplicate.obdService=false;addBluetoothDevice(bluetooth,duplicate);
    assert(bluetooth.devices.size()==1&&bluetooth.devices[0].rssi==-30&&bluetooth.devices[0].obdService);
    auto other=peer;other.address="11:22:33:44:55:66";other.rssi=-10;other.obdService=false;addBluetoothDevice(bluetooth,other);
    assert(bluetooth.devices.size()==2&&bluetooth.devices[0].obdService);
    bluetooth.saved=restored;assert(chooseBluetoothPeer(bluetooth,other));assert(samePeer(bluetooth.saved,peer)&&samePeer(bluetooth.target,other));
    assert(!confirmBluetoothPeer(bluetooth,peer)&&samePeer(bluetooth.saved,peer)); // Late result cannot replace the current selection.
    assert(confirmBluetoothPeer(bluetooth,other)&&samePeer(bluetooth.saved,other));
    for(unsigned i=0;i<100;++i){auto candidate=peer;char address[20];snprintf(address,sizeof(address),"01:02:03:04:05:%02x",i);candidate.address=address;candidate.rssi=-int(i)-1;addBluetoothDevice(bluetooth,candidate);}
    assert(bluetooth.devices.size()==MaxBluetoothDevices);
    // The 60-second timeout starts after the three-second splash, not at power-on.
    assert(!connectionTimedOut(Mode::Loading,62999,SplashDurationMs)&&connectionTimedOut(Mode::Loading,63000,SplashDurationMs));
    for(int saved:{10,50,100}){assert(effectiveBrightness(Mode::Splash,saved)==50);assert(effectiveBrightness(Mode::Loading,saved)==saved);}
    assert(protocolNumber("ATDPN\rA7\r")==7&&protocolNumber("A6")==6&&protocolNumber("17")==0);
    assert(!connectionTimedOut(Mode::Loading,59999,0));assert(connectionTimedOut(Mode::Loading,60000,0));
    assert(!connectionTimedOut(Mode::Live,20000,0)&&!connectionTimedOut(Mode::Demo,20000,0));
    assert(connectionTimedOut(Mode::Loading,59999,0xFFFFFFFFu));
    assert(!connectionTimedOut(Mode::Loading,60999,1000));
    assert(!connectionTimedOut(Mode::Loading,100,101));
    assert(ProfileCount==2&&unsigned(VehicleProfile::Generic)==0&&unsigned(VehicleProfile::Mercedes)==1);
    assert(normalizeProfile(2)==VehicleProfile::Generic&&normalizeProfile(255)==VehicleProfile::Generic);
    Snapshot selected;
    for(unsigned profile=0;profile<ProfileCount;++profile){
        for(unsigned protocol=0;protocol<=9;++protocol){
            restoreProfileSettings(selected,profile,protocol);
            assert(unsigned(selected.profile)==profile&&selected.requestedProtocol==protocol);
        }
        restoreProfileSettings(selected,profile,255);assert(selected.requestedProtocol==defaultProtocol(selected.profile));
    }
    for(unsigned profile:{ProfileCount,255u}){
        restoreProfileSettings(selected,profile,5);
        assert(selected.profile==VehicleProfile::Generic&&selected.requestedProtocol==0&&!profileHasPressure(selected.profile));
    }
    selected.requestedProtocol=9;selectProfile(selected,VehicleProfile::Mercedes);assert(selected.requestedProtocol==0&&profileHasPressure(selected.profile));
    selected.requestedProtocol=5;selectProfile(selected,VehicleProfile::Generic);assert(selected.requestedProtocol==0&&!profileHasPressure(selected.profile));
    double v=0;
    auto r=parseReplies("18DAF11004410C0B88000000\r\n18DAF11803410D4400000000\r>");
    assert(r.size()==2);assert(decode(r[0],Rpm,0x0C,v)&&v==738);assert(decode(r[1],Speed,0x0D,v)&&v==68);
    auto odo=parseReplies("41 A6 00 0D F5 1F\r");assert(decode(odo[0],Odometer,0xA6,v));assert(std::abs(v-91471.9)<.0001);assert(roundedKm(v)==91472);
    auto can6=parseReplies("7E804410C0B88000000\r7E90641A6000DF51F00\r");
    assert(can6.size()==2&&can6[0].source==0x7E8&&can6[1].source==0x7E9);
    assert(decode(can6[0],Rpm,0x0C,v)&&v==738);
    assert(decode(can6[1],Odometer,0xA6,v)&&roundedKm(v)==91472);
    assert(roundedKm(123.49)==123&&roundedKm(123.5)==124&&roundedKm(0)==0);
    assert(roundedKm(429496729.5)==429496730);
    auto bits=supported(parseReplies("41 A0 04 00 00 00\r"),0xA0);assert(bits.size()==1&&bits[0]==0xA6);
    const std::string pressure="7EA102C622002000000\r\n7EA21000000004C00FF\r\n7EA221100068B00FFFF\r\n7EA2301000000351235\r\n7EA24333535FC5E00FF\r\n7EA25FF01FFFF01FFFF\r\n7EA260121A8AAAAAAAA\r";
    r=parseReplies(pressure);assert(r.size()==1&&r[0].data.size()==44);assert(decode(r[0],Pressure,0,v));assert(std::abs(v-25.62775)<.001);
    auto wrong=r[0];wrong.source=0x7E8;assert(!decode(wrong,Pressure,0,v));
    auto bad=pressure;bad.replace(bad.find("7EA22"),5,"7EA23");assert(parseReplies(bad).empty());
    assert(parseReplies(pressure.substr(0,pressure.find("7EA26"))).empty());
    assert(!decode({0x7EA,{0x7F,0x22,0x78}},Pressure,0,v));assert(!decode({0,{0x41,0x0C,0}},Rpm,0x0C,v));
    assert(parseReplies("NO DATA\rSTOPPED\r010C\rSEARCHING...\r>").empty());
    Snapshot s;update(s.samples[Speed],50,100);update(s.samples[Speed],100,110);animate(s,143,33);assert(s.samples[Speed].shown>50&&s.samples[Speed].shown<100);
    update(s.samples[Speed],0,200);animate(s,233,33);assert(s.samples[Speed].shown==0);
    animate(s,4000,33);assert(!s.samples[Speed].valid);
    update(s.samples[Speed],80,4001);assert(s.samples[Speed].shown==80);
    animate(s,4000,33);assert(s.samples[Speed].valid);
    assert(period(Speed,PageId::Overview)<period(Speed,PageId::Pedals)&&period(Pressure,PageId::Pedals)<period(Pressure,PageId::Overview));
    for(unsigned i=0;i<Count;++i){
        assert(period(Metric(i),PageId::Settings)==period(Metric(i),PageId::Overview));
        assert(expiry(Metric(i),PageId::Settings)==expiry(Metric(i),PageId::Overview));
    }
    assert(pressureBurstPeriod(PageId::Settings)==5000&&pressureBurstReads(PageId::Settings)==1);
    assert(pressureBurstPeriod(PageId::Pedals)==1000&&pressureBurstReads(PageId::Pedals)==2);
    assert(batteryPercent(3.2)==0&&batteryPercent(3.85)==50&&batteryPercent(4.2)==100);
    int lastPercent=0;for(int mv=2500;mv<=4350;++mv){int percent=batteryPercent(mv/1000.0);assert(percent>=lastPercent&&percent<=100);lastPercent=percent;}
    BatteryInfo battery;updateBattery(battery,3.8,false,1000);assert(battery.valid&&battery.percent==40&&!battery.charging);
    updateBattery(battery,4.2,true,3000);assert(battery.valid&&battery.charging&&battery.volts>3.8&&battery.volts<3.9);
    updateBattery(battery,5.0,true,5000);assert(!battery.valid&&!battery.charging);
    updateBattery(battery,NAN,false,7000);assert(!battery.valid);
    updateBattery(battery,2.5,false,9000);assert(battery.valid&&battery.percent==0);
    updateBattery(battery,0,false,11000);assert(!battery.valid);
    updateBattery(battery,4.35,false,0xfffffff0);updateBattery(battery,4.2,false,1984);assert(battery.valid&&battery.percent==100);
    DemoGenerator demo(1234);Snapshot a,b;a.profile=b.profile=VehicleProfile::Mercedes;DemoGenerator same(1234);
    a.battery=battery;a.system.freeHeap=123456;
    double distance=0;bool idle=false,moving=false,braking=false;
    for(uint32_t n=100;n<=180000;n+=100){
        demo.tick(a,n,.1);same.tick(b,n,.1);animate(a,n,100);
        for(auto& sample:a.samples)assert(sample.valid&&std::isfinite(sample.raw));
        assert(a.samples[Speed].raw>=0&&a.samples[Speed].raw<=100);assert(a.samples[Rpm].raw>=700&&a.samples[Rpm].raw<5000);
        for(auto m:{Pedal,Throttle,Fuel})assert(a.samples[m].raw>=0&&a.samples[m].raw<=100);
        assert(a.samples[Pressure].raw>=0&&a.samples[Pressure].raw<25);
        assert(a.samples[Odometer].raw>=distance);distance=a.samples[Odometer].raw;
        assert(a.samples[Speed].raw==b.samples[Speed].raw);
        if(a.samples[Speed].raw==0)idle=true;else moving=true;
        if(a.samples[Speed].raw>10&&a.samples[Pressure].raw>5){braking=true;assert(a.samples[Pedal].raw==0);}
    }
    assert(idle&&moving&&braking&&distance>=40000&&distance<130010&&a.demo);
    assert(a.battery.volts==battery.volts&&a.battery.stamp==battery.stamp&&a.system.freeHeap==123456);
    Snapshot generic;demo.tick(generic,100,0.1);assert(!generic.samples[Pressure].valid&&generic.capabilities.status[Pressure]==MetricStatus::Unsupported);
    invalidate(a);for(auto& x:a.samples)assert(!x.valid);
    std::cout<<"PASS: universal brightness, timing, model, CAN/ISO-TP, PID/odometer, source validation, smoothing/staleness, 1800 coherent demo samples\n";
}
