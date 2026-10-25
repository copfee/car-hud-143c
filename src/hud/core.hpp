#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "profiles_generated.hpp"

namespace hud {
enum Metric : uint8_t { Speed, Rpm, Coolant, Fuel, Odometer, Pedal, Throttle, Pressure, Count };
enum class Mode : uint8_t { Loading, Timeout, Live, Demo, Splash };
enum class PageId : uint8_t { Overview, Pedals, Settings, Count };
constexpr unsigned PageCount=unsigned(PageId::Count);
inline bool connectionTimedOut(Mode mode,uint32_t now,uint32_t start){return mode==Mode::Loading&&int32_t(now-start)>=int32_t(ConnectionTimeoutMs);}
inline bool connectionAllowed(Mode mode){return mode==Mode::Loading||mode==Mode::Timeout;}
uint8_t splashOpacity(uint32_t elapsed);
// Started only after the first splash frame was flushed; emits completion once per boot.
class SplashTimeline {
    uint32_t start_=0;
    bool started_=false,completed_=false;
public:
    void start(uint32_t now){if(!started_){start_=now;started_=true;}}
    uint32_t elapsed(uint32_t now) const;
    bool takeCompletion(uint32_t now);
};
enum class Action : uint8_t { None, Retry, Demo, Brightness, Mirror, Profile, Protocol, Identity };
enum class VehicleProfile : uint8_t { Generic, Mercedes, HondaCl300, Count };
static_assert(unsigned(VehicleProfile::Count)==ProfileCount,"Profile definitions must match stable persisted IDs");
inline VehicleProfile normalizeProfile(unsigned value){return value<ProfileCount?VehicleProfile(value):VehicleProfile::Generic;}
inline uint8_t defaultProtocol(VehicleProfile profile){return ProfileDefaultProtocols[unsigned(normalizeProfile(unsigned(profile)))];}
inline bool profileHasPressure(VehicleProfile profile){return ProfilePressureEnabled[unsigned(normalizeProfile(unsigned(profile)))];}
enum class ConnectionState : uint8_t { Idle, Scanning, Initializing, Discovering, Capabilities, Running, WaitingEcu, Recovering, Demo };
enum class MetricStatus : uint8_t { Unknown, Unsupported, NoData, Valid, Stale };
struct ProtocolInfo { uint8_t configured=0, detected=0, active=0; bool verified=false; char description[80]{}; };
struct EcuCapabilities { uint32_t source=0; std::array<bool,256> supported{},known{}; };
struct VehicleCapabilities { std::vector<EcuCapabilities> ecus; std::array<MetricStatus,Count> status{}; };
struct IdentityRecord { uint32_t source=0; std::string vin,ecu; };
struct VehicleIdentity { std::vector<IdentityRecord> records; std::string model,vin,ecu,detail="等待读取"; bool conflict=false,verified=false; };
int effectiveBrightness(Mode mode,int saved);
struct Sample {
    double raw = 0, shown = 0;
    uint32_t stamp = 0;
    uint32_t source = 0;
    bool valid = false;
};
struct BatteryInfo {
    double volts=0;
    uint32_t stamp=0;
    int percent=0;
    bool valid=false, charging=false;
    // The board has no battery-presence or fuel-gauge signal. Never infer full/present.
};
int batteryPercent(double volts);
void updateBattery(BatteryInfo& info,double volts,bool charging,uint32_t now);
struct ConnectionInfo {
    bool ble=false, ecu=false, rssiValid=false;
    uint8_t activeProtocol=0;
    int rssi=0;
    uint32_t lastReply=0;
    ConnectionState state=ConnectionState::Idle;
    ProtocolInfo info;
    char address[24]{}, elm[64]{}, protocol[16]{};
};
struct SystemInfo {
    char chip[24]{};
    uint32_t flashBytes=0, psramBytes=0, freeHeap=0;
    uint64_t uptimeSeconds=0;
};
struct Snapshot {
    std::array<Sample, Count> samples{};
    Mode mode = Mode::Splash;
    uint32_t splashElapsed=0;
    std::string status = "正在扫描蓝牙";
    bool demo = false;
    PageId page = PageId::Overview;
    int brightness = 50;
    bool mirror = false;
    VehicleProfile profile=VehicleProfile::Generic;
    uint8_t requestedProtocol=0;
    double samplingScale=1;
    VehicleCapabilities capabilities;
    VehicleIdentity identity;
    BatteryInfo battery;
    ConnectionInfo connection;
    SystemInfo system;
};
inline void selectProfile(Snapshot& s,VehicleProfile profile){s.profile=normalizeProfile(unsigned(profile));s.requestedProtocol=defaultProtocol(s.profile);}
uint32_t period(Metric m, PageId page);
uint32_t pressureBurstPeriod(PageId page);
unsigned pressureBurstReads(PageId page);
uint32_t expiry(Metric m, PageId page);
uint64_t roundedKm(double km);
void update(Sample& s, double value, uint32_t now, uint32_t source = 0);
void animate(Snapshot& s, uint32_t now, uint32_t dt);
void invalidate(Snapshot& s);
struct DemoGenerator {
    explicit DemoGenerator(uint32_t seed = 2020);
    void tick(Snapshot& s, uint32_t now, double dt);
private:
    uint32_t seed_;
    double elapsed_ = 0, km_ = 0, fuel_ = 0, cruise_ = 82, coolant_ = 94, brake_ = 22;
    double noise();
};
struct Reply { uint32_t source; std::vector<uint8_t> data; };
// ATH1 + CAF1 + D0, classic CAN PCI. Also accepts ATH0 single-line replies for log replay.
std::vector<Reply> parseReplies(const std::string& text,uint8_t protocol=0);
std::vector<uint8_t> supported(const std::vector<Reply>& replies, uint8_t base);
bool decode(const Reply& r, Metric m, uint8_t pid, double& value);
bool parseHex(const std::string& s, std::vector<uint8_t>& out);
}
