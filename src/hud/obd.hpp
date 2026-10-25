#pragma once
#include "core.hpp"
#include <algorithm>
namespace hud {
uint8_t protocolNumber(const std::string& answer);
std::string cleanAnswer(const std::string& answer,const std::string& echo="");
std::vector<std::string> protocolFormat(uint8_t protocol);
std::vector<std::string> routeCommands(uint8_t standard,bool pressure);
void addIdentity(VehicleIdentity&,const std::vector<Reply>&,uint8_t pid,uint8_t protocol);
void addCapabilities(VehicleCapabilities&,const std::vector<Reply>&,uint8_t base);
bool declares(const VehicleCapabilities&,uint8_t pid,uint32_t source=0);
class ElmChannel {
public:
    virtual ~ElmChannel()=default;
    virtual uint32_t now() const=0;
    virtual bool alive() const=0;
    virtual bool command(const std::string&,std::string&,uint32_t timeout)=0;
};
class ProtocolStrategy {
public:
    static bool configure(ElmChannel&,uint8_t);
    static bool discover(ElmChannel&,Snapshot&,uint8_t cached);
};
class RouteManager {
    uint8_t standard_,active_=0;
    bool valid_=false,pressure_=false;
public:
    explicit RouteManager(uint8_t standard=0):standard_(standard){}
    void reset(uint8_t standard){standard_=standard;active_=standard;valid_=true;pressure_=false;}
    bool select(ElmChannel&,bool pressure);
    uint8_t protocol()const{return valid_?active_:0;}
};
class PollScheduler {
    std::array<uint32_t,Count> sent_{},next_{};
    std::array<unsigned,Count> failures_{};
    std::array<double,Count> cost_{};
public:
    PollScheduler(){cost_.fill(120);}
    double scale(const Snapshot&)const;
    int select(const Snapshot&,uint32_t now,bool pressureAllowed=true)const;
    void result(Metric,bool,uint32_t now,uint32_t elapsed);
    void recordCost(Metric m,uint32_t elapsed){cost_[m]=cost_[m]*.8+std::max(1u,elapsed)*.2;}
    void wake(){sent_.fill(0);next_.fill(0);failures_.fill(0);}
    unsigned failures(Metric m)const{return failures_[m];}
};
class ObdController {
    ElmChannel& channel_;
    RouteManager route_;
    PollScheduler scheduler_;
    std::array<uint8_t,Count> pids_{};
    std::array<uint32_t,Count> source_{};
    std::array<bool,Count> selected_{};
    uint32_t lastStandard_=0,lastHeartbeat_=0,lastPressure_=0,identityDue_=0;
    unsigned heartbeatFailures_=0,identityRound_=0,identityStep_=0;
    bool identityWanted_=true,identityVin_=false,identityName_=false,everData_=false;
    bool read(Snapshot&,Metric,bool restore=true);
    bool identity(Snapshot&);
public:
    explicit ObdController(ElmChannel& c):channel_(c){}
    bool initialize(Snapshot&,uint8_t cached=0);
    bool cycle(Snapshot&);
    void refreshIdentity(Snapshot& s){if(identityStep_)return;s.identity=VehicleIdentity{};identityRound_=identityStep_=0;identityDue_=0;identityWanted_=true;}
};
}
