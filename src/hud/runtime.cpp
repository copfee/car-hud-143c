#include "board.hpp"
#include "ui.hpp"
#include "obd.hpp"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <atomic>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace hud {
Board* Board::instance_=nullptr;
static Board board;
static Interface ui;
static Preferences prefs;
static Snapshot shared;
static SplashTimeline splash;
static SemaphoreHandle_t stateMutex;
static std::atomic<uint32_t> epoch{1},attemptStart{0};
static std::atomic<uint16_t> activeHandle{BLE_HS_CONN_HANDLE_NONE};
static std::atomic<bool> rxOverflow{false},identityRequested{false},scanRequested{false};
static bool pickerWasDemo=false;
static SemaphoreHandle_t preferencesMutex;
struct PreferencesLock { PreferencesLock(){xSemaphoreTake(preferencesMutex,portMAX_DELAY);}~PreferencesLock(){xSemaphoreGive(preferencesMutex);} };
struct Packet { uint16_t size;uint8_t data[192]; };
static QueueHandle_t packets;
struct Lock { Lock(){xSemaphoreTake(stateMutex,portMAX_DELAY);}~Lock(){xSemaphoreGive(stateMutex);} };
static Snapshot snapshot(){Lock guard;return shared;}
static void cancelTransport(){auto h=activeHandle.exchange(BLE_HS_CONN_HANDLE_NONE);if(h!=BLE_HS_CONN_HANDLE_NONE)ble_gap_terminate(h,BLE_ERR_REM_USER_CONN_TERM);}
static void attempt(){Lock guard;epoch.fetch_add(1);cancelTransport();attemptStart=millis();invalidate(shared);shared.connection=ConnectionInfo{};shared.connection.state=ConnectionState::Connecting;shared.identity.verified=false;shared.identity.detail="已断开 / 待读取复核";shared.demo=false;shared.mode=Mode::Loading;shared.status="正在连接蓝牙";}
static bool current(uint32_t token){return token==epoch.load();}
static void openBluetoothPicker(){
    Lock guard;epoch.fetch_add(1);cancelTransport();pickerWasDemo=shared.demo;shared.demo=false;shared.mode=Mode::Bluetooth;
    shared.bluetooth.scanning=true;shared.bluetooth.devices.clear();shared.bluetooth.detail="正在扫描附近设备";++shared.bluetooth.revision;
    scanRequested=true;
}
static void stage(uint32_t token,const char* text){Lock guard;if(current(token)&&(shared.mode==Mode::Loading||shared.mode==Mode::Timeout))shared.status=text;}
static void notify(NimBLERemoteCharacteristic*,uint8_t* data,size_t n,bool) {
    while(n) {Packet p{};p.size=std::min(n,sizeof(p.data));memcpy(p.data,data,p.size);if(xQueueSend(packets,&p,0)!=pdTRUE)rxOverflow=true;data+=p.size;n-=p.size;}
}
class BleTransport {
public:
    NimBLEClient* client=nullptr;
    NimBLERemoteCharacteristic* tx=nullptr;
    NimBLERemoteCharacteristic* rx=nullptr;
    std::string error;
    uint32_t token=0;
    bool connected() const{return client&&client->isConnected();}
    void close(){
        activeHandle=BLE_HS_CONN_HANDLE_NONE;
        tx=nullptr;rx=nullptr;
        if(client){if(client->isConnected())client->disconnect();NimBLEDevice::deleteClient(client);client=nullptr;}
        xQueueReset(packets);rxOverflow=false;

    }
    bool open(uint32_t t,const BlePeer& peer){
        close();token=t;error="蓝牙连接失败，请检查设备供电";
        if(!validPeer(peer)){error="请选择蓝牙设备";return false;}
        {Lock guard;if(!current(token))return false;shared.connection.state=ConnectionState::Connecting;}
        stage(token,"正在连接蓝牙");client=NimBLEDevice::createClient();if(!client)return false;
        client->setConnectTimeout(3000);
        if(!client->connect(NimBLEAddress(peer.address,peer.addressType)))return false;
        activeHandle=client->getConnHandle();
        if(!current(token))return false;
        error="设备不支持所需 OBD 服务，请重新选择";
        auto* s=client->getService(NimBLEUUID(uint16_t(0x18F0)));if(!s)return false;
        rx=s->getCharacteristic(NimBLEUUID(uint16_t(0x2AF0)));tx=s->getCharacteristic(NimBLEUUID(uint16_t(0x2AF1)));
        if(!rx||!tx||(!tx->canWrite()&&!tx->canWriteNoResponse()))return false;
        if(!rx->canNotify()&&!rx->canIndicate())return false;
        if(!rx->subscribe(rx->canNotify(),notify))return false;
        int rssi=client->getRssi();
        {Lock guard;if(!current(token))return false;
            shared.connection.ble=true;shared.connection.rssi=rssi;
            shared.connection.rssiValid=shared.connection.rssi<0&&shared.connection.rssi>=-127;
            snprintf(shared.connection.address,sizeof(shared.connection.address),"%s",client->getPeerAddress().toString().c_str());
        }
        stage(token,"蓝牙已连接");return true;
    }
    bool write(const std::string& line){return tx&&tx->writeValue(reinterpret_cast<const uint8_t*>(line.data()),line.size(),tx->canWrite());}
};
static void scanBluetooth(uint32_t token){
    auto* scan=NimBLEDevice::getScan();scan->clearResults();scan->setMaxResults(64);
    auto results=scan->getResults(4000,false);BluetoothInfo found;
    for(int i=0;i<results.getCount();++i){
        auto* device=results.getDevice(i);BlePeer peer;
        peer.address=device->getAddress().toString();peer.addressType=device->getAddress().getType();peer.name=device->getName();peer.rssi=device->getRSSI();
        peer.obdService=device->isAdvertisingService(NimBLEUUID(uint16_t(0x18F0)));addBluetoothDevice(found,peer);
    }
    scan->clearResults();
    Lock guard;if(!current(token)||shared.mode!=Mode::Bluetooth)return;
    shared.bluetooth.devices=std::move(found.devices);shared.bluetooth.scanning=false;++shared.bluetooth.revision;
    shared.bluetooth.detail=shared.bluetooth.devices.empty()?"未发现设备，请重新扫描":"请选择设备，连接后验证 OBD 通道";
}
static void saveConnectedPeer(uint32_t token,const BlePeer& peer){
    // One versioned NVS value prevents partially written address/type/name records.
    PreferencesLock preferences;Lock guard;
    if(!current(token)||!samePeer(shared.bluetooth.target,peer))return;
    auto encoded=encodePeer(peer);
    if(encoded.empty())return;
    if(encodePeer(shared.bluetooth.saved)==encoded||prefs.putString("ble_peer",encoded.c_str())==encoded.size()){
        confirmBluetoothPeer(shared.bluetooth,peer);shared.bluetooth.detail="已保存 / 下次开机自动连接";
    }else shared.bluetooth.detail="蓝牙已连接，但保存失败";
}
class ElmSession final : public ElmChannel {
    BleTransport& transport_;
public:
    bool synchronized=false,discovering=true;
    uint32_t roundStart=0;
    Snapshot* progress=nullptr;
    explicit ElmSession(BleTransport& transport):transport_(transport){}
    void reset(uint32_t start,Snapshot* state){roundStart=start;progress=state;discovering=true;synchronized=true;}
    uint32_t now()const override{return millis();}
    bool connected()const{return transport_.connected();}
    bool alive()const override{return current(transport_.token)&&connected()&&synchronized&&(!discovering||uint32_t(millis()-roundStart)<DiscoveryTimeoutMs);}
    bool command(const std::string& cmd,std::string& answer,uint32_t timeout=1800) override {
        auto token=transport_.token;auto* tx=transport_.tx;
        if(discovering){auto used=uint32_t(millis()-roundStart);if(used>=DiscoveryTimeoutMs)return false;timeout=std::min(timeout,DiscoveryTimeoutMs-used);}
        if(progress){Lock guard;if(current(token)){shared.connection.state=progress->connection.state;shared.status=progress->status;}}
        answer.clear();if(!current(token)||!connected()||!tx||!synchronized)return false;
        xQueueReset(packets);rxOverflow=false;
        std::string line=cmd+"\r";
        if(Serial)Serial.printf("ELM > %s\n",cmd.c_str());
        if(!transport_.write(line)){synchronized=false;return false;}
        uint32_t start=millis();
        while(uint32_t(millis()-start)<timeout && current(token)&&connected()&&!rxOverflow.load()){
            Packet p;if(xQueueReceive(packets,&p,pdMS_TO_TICKS(20))!=pdTRUE)continue;
            answer.append(reinterpret_cast<char*>(p.data),p.size);
            if(answer.size()>4096)break;
            auto end=answer.find('>');
            if(end!=std::string::npos){answer.resize(end);if(Serial)Serial.printf("ELM < %s\n",answer.c_str());return true;}
        }
        synchronized=false;return false;
    }
};
static void publish(uint32_t token,const Snapshot& local){
    Lock guard;if(!current(token))return;
    shared.mode=local.mode;shared.connection=local.connection;shared.status=local.status;
    shared.identity=local.identity;shared.capabilities=local.capabilities;shared.samplingScale=local.samplingScale;
    for(unsigned i=0;i<Count;++i){const auto& v=local.samples[i];auto& dest=shared.samples[i];
        if(!v.valid)dest.valid=false;
        else if(!dest.valid||v.stamp!=dest.stamp)update(dest,v.raw,v.stamp,v.source);
    }
}
static std::string cacheKey(const std::string& address,VehicleProfile profile){
    uint32_t hash=2166136261u;for(unsigned char c:address){hash^=c;hash*=16777619u;}
    char key[16];snprintf(key,sizeof(key),"p%u%08x",unsigned(profile),unsigned(hash));return key;
}
static void worker(void*){
    while(snapshot().mode==Mode::Splash)vTaskDelay(pdMS_TO_TICKS(10));
    NimBLEDevice::init("Universal HUD");NimBLEDevice::setPower(ESP_PWR_LVL_P6);NimBLEDevice::getScan()->setActiveScan(true);
    BleTransport link;ElmSession session(link);unsigned failures=0;uint32_t backoffStart=0,backoff=0,roundStart=millis(),seen=epoch.load();
    while(true){
        // UI changes epoch and state under the same mutex. Read the epoch first,
        // then the snapshot, rejecting a concurrent transition before doing I/O.
        auto token=epoch.load();auto s=snapshot();if(!current(token))continue;
        if(token!=seen){seen=token;failures=0;backoff=0;roundStart=millis();}
        if(s.mode==Mode::Bluetooth){
            link.close();bool requested=false;
            {Lock guard;if(current(token)&&shared.mode==Mode::Bluetooth)requested=scanRequested.exchange(false);}
            if(requested)scanBluetooth(token);vTaskDelay(pdMS_TO_TICKS(40));continue;
        }
        if(!connectionAllowed(s.mode)){link.close();vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(!validPeer(s.bluetooth.target)){link.close();vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(backoff&&uint32_t(millis()-backoffStart)<backoff){vTaskDelay(pdMS_TO_TICKS(40));continue;}
        if(backoff){backoff=0;roundStart=millis();}
        if(uint32_t(millis()-roundStart)>=DiscoveryTimeoutMs){
            backoff=RetryBackoff[std::min(failures++,3u)];backoffStart=millis();
            {Lock guard;if(current(token)){shared.mode=Mode::Timeout;shared.connection.state=ConnectionState::Recovering;shared.status="等待自动重试";}}
            continue;
        }
        if(!link.open(token,s.bluetooth.target)){
            link.close();if(current(token)){backoff=RetryBackoff[std::min(failures++,3u)];backoffStart=millis();Lock guard;shared.status=link.error;shared.connection.ble=false;shared.connection.ecu=false;shared.connection.state=ConnectionState::Recovering;}
            vTaskDelay(pdMS_TO_TICKS(40));continue;
        }
        saveConnectedPeer(token,s.bluetooth.target);
        Snapshot local=snapshot();local.identity={};local.capabilities={};local.connection.info={};
        session.reset(roundStart,&local);auto key=cacheKey(local.connection.address,local.profile);uint8_t cached;
        {PreferencesLock guard;cached=prefs.getUChar(key.c_str(),0);}
        ObdController controller(session);
        if(controller.initialize(local,cached)&&current(token)){
            session.discovering=false;failures=0;
            {PreferencesLock guard;if(cached!=local.connection.info.detected)prefs.putUChar(key.c_str(),local.connection.info.detected);}
            publish(token,local);uint32_t lastRssi=millis();
            while(session.alive()){
                {Lock guard;local.page=shared.page;}
                if(identityRequested.exchange(false))controller.refreshIdentity(local);
                if(!controller.cycle(local))break;
                if(local.page==PageId::Settings&&uint32_t(millis()-lastRssi)>=2000){local.connection.rssi=link.client->getRssi();local.connection.rssiValid=local.connection.rssi<0&&local.connection.rssi>=-127;lastRssi=millis();}
                publish(token,local);vTaskDelay(pdMS_TO_TICKS(10));
            }
            if(current(token)){
                link.close();local.connection.ble=false;local.connection.ecu=false;local.connection.state=ConnectionState::Recovering;
                local.identity.verified=false;local.identity.detail="已断开 / 等待复核";invalidate(local);publish(token,local);
                // Keep the dashboard accessible while recovery is pending, then use the connection page.
                {Lock guard;shared.mode=Mode::Loading;shared.status="连接断开正在恢复";}
                attemptStart=millis();roundStart=millis();backoff=RetryBackoff[0];backoffStart=millis();
            }
        }else{
            if(cached&&current(token)){PreferencesLock guard;prefs.putUChar(key.c_str(),0);}
            if(current(token)){
                backoff=RetryBackoff[std::min(failures++,3u)];backoffStart=millis();
                Lock guard;shared.status="等待车辆点火 / 自动重试";shared.connection.ecu=false;shared.connection.ble=false;shared.connection.state=ConnectionState::Recovering;
            }
        }
        session.progress=nullptr;link.close();NimBLEDevice::getScan()->clearResults();vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static void telemetry(void*){
    BatteryInfo battery;unsigned tick=0;TickType_t wake=xTaskGetTickCount();
    while(true){
        if(tick%2==0)updateBattery(battery,board.readBatteryVoltage(),board.batteryCharging(),millis());
        SystemInfo system;
        snprintf(system.chip,sizeof(system.chip),"%s rev.%u",ESP.getChipModel(),unsigned(ESP.getChipRevision()));
        system.flashBytes=ESP.getFlashChipSize();system.psramBytes=ESP.getPsramSize();system.freeHeap=ESP.getFreeHeap();
        system.uptimeSeconds=uint64_t(esp_timer_get_time())/1000000;
        {Lock guard;shared.battery=battery;shared.system=system;}
        ++tick;vTaskDelayUntil(&wake,pdMS_TO_TICKS(1000));
    }
}
}
void setup(){
    using namespace hud;
    Serial.begin(115200);stateMutex=xSemaphoreCreateMutex();preferencesMutex=xSemaphoreCreateMutex();packets=xQueueCreate(24,sizeof(Packet));
    if(!stateMutex||!preferencesMutex||!packets)abort();
    if(!board.begin()){if(Serial)Serial.println("Display initialization failed");while(true)delay(100);}
    prefs.begin("c260-hud",false);shared.brightness=std::max(10,std::min(100,prefs.getInt("brightness",50)));shared.mirror=prefs.getBool("mirror",false);
    shared.profile=prefs.getUChar("schema",0)>=1?normalizeProfile(prefs.getUChar("profile",0)):VehicleProfile::Generic;
    shared.requestedProtocol=prefs.getUChar("schema",0)>=1?(prefs.getUChar("protocol",defaultProtocol(shared.profile))<=9?prefs.getUChar("protocol",defaultProtocol(shared.profile)):defaultProtocol(shared.profile)):0;
    shared.bluetooth.saved=decodePeer(prefs.getString("ble_peer","").c_str());shared.bluetooth.target=shared.bluetooth.saved;
    if(validPeer(shared.bluetooth.saved))shared.bluetooth.detail="已保存 / 下次开机自动连接";
    board.setBrightness(effectiveBrightness(Mode::Splash,shared.brightness));board.setMirror(shared.mirror);ui.create();
    lv_refr_now(lv_display_get_default());board.waitDisplayIdle(); // Start timing only after the first frame reaches the panel.
    splash.start(millis());
    if(xTaskCreatePinnedToCore(worker,"obd-owner",12288,nullptr,1,nullptr,0)!=pdPASS||
       xTaskCreatePinnedToCore(telemetry,"device-info",4096,nullptr,1,nullptr,0)!=pdPASS)abort();
}
void loop(){
    using namespace hud;
    static uint32_t last=millis(),lastDemo=0,lastSettingChange=0;static bool settingsDirty=false;
    static bool demoActive=false;static DemoGenerator demo(esp_random());
    uint32_t now=millis();
    if(splash.takeCompletion(now)){
        auto black=snapshot();black.splashElapsed=SplashDurationMs;ui.refresh(black,now,0);
        lv_refr_now(lv_display_get_default());board.waitDisplayIdle();
        board.setBrightness(effectiveBrightness(Mode::Loading,black.brightness));
        if(validPeer(black.bluetooth.saved))attempt();else openBluetoothPicker();
    }
    auto action=ui.takeAction();
    if(action==Action::Retry){if(validPeer(snapshot().bluetooth.target))attempt();else openBluetoothPicker();demoActive=false;}
    else if(action==Action::Bluetooth){openBluetoothPicker();}
    else if(action==Action::ScanBluetooth){
        Lock guard;epoch.fetch_add(1);shared.bluetooth.scanning=true;shared.bluetooth.devices.clear();shared.bluetooth.detail="正在扫描附近设备";++shared.bluetooth.revision;scanRequested=true;
    }
    else if(action==Action::SelectBluetooth){
        auto peer=ui.selectedBluetoothPeer();bool selected;
        {Lock guard;selected=chooseBluetoothPeer(shared.bluetooth,peer);}
        if(selected){scanRequested=false;attempt();demoActive=false;}
    }
    else if(action==Action::CancelBluetooth){
        bool saved;{Lock guard;epoch.fetch_add(1);scanRequested=false;shared.bluetooth.scanning=false;shared.bluetooth.devices.clear();++shared.bluetooth.revision;shared.bluetooth.target=shared.bluetooth.saved;saved=validPeer(shared.bluetooth.saved);
            if(pickerWasDemo){shared.demo=true;shared.mode=Mode::Demo;}else if(!saved){shared.mode=Mode::Timeout;shared.status="请选择蓝牙设备或进入演示";}}
        if(pickerWasDemo){lastDemo=now;demoActive=true;}else if(saved)attempt();
    }
    else if(action==Action::Demo){
        Lock guard;epoch.fetch_add(1);cancelTransport();invalidate(shared);shared.connection=ConnectionInfo{};shared.identity={};shared.identity.detail="演示模式";shared.connection.state=ConnectionState::Demo;shared.demo=true;shared.mode=Mode::Demo;
        demo=DemoGenerator(esp_random());demoActive=true;lastDemo=now;
        demo.tick(shared,now,0);
    }
    else if(action==Action::Brightness){int p=ui.brightness();board.setBrightness(p);{Lock guard;shared.brightness=p;}settingsDirty=true;lastSettingChange=now;}
    else if(action==Action::Mirror){bool m=ui.mirror();board.setMirror(m);{Lock guard;shared.mirror=m;}settingsDirty=true;lastSettingChange=now;}
    else if(action==Action::Profile||action==Action::Protocol){
        {Lock guard;if(action==Action::Profile)selectProfile(shared,ui.profile());else shared.requestedProtocol=ui.protocol();}settingsDirty=true;lastSettingChange=now;
        if(validPeer(snapshot().bluetooth.target))attempt();else openBluetoothPicker();demoActive=false;
    }else if(action==Action::Identity)identityRequested=true;
    if(settingsDirty&&uint32_t(now-lastSettingChange)>=500){auto s=snapshot();PreferencesLock guard;prefs.putInt("brightness",s.brightness);prefs.putBool("mirror",s.mirror);prefs.putUChar("profile",unsigned(s.profile));prefs.putUChar("protocol",s.requestedProtocol);prefs.putUChar("schema",1);settingsDirty=false;}
    if(uint32_t(now-last)>=16){
        Snapshot s;
        {Lock guard;
            shared.splashElapsed=splash.elapsed(now);
            if(connectionTimedOut(shared.mode,now,attemptStart.load())){shared.mode=Mode::Timeout;shared.status="仍在检测 / 后台自动重试";}
            shared.page=ui.page();
            if(demoActive&&shared.demo&&uint32_t(now-lastDemo)>=100){demo.tick(shared,now,double(uint32_t(now-lastDemo))/1000);lastDemo=now;}
            animate(shared,now,uint32_t(now-last));s=shared;
        }
        board.setNumericPage((s.mode==Mode::Live||s.mode==Mode::Demo)&&unsigned(s.page)<2?int(s.page):-1);
        ui.refresh(s,now,uint32_t(now-attemptStart.load()));last=now;
    }
    uint32_t handlerStarted=micros();lv_timer_handler();board.waitDisplayIdle();
    board.profileHandler(uint32_t(micros()-handlerStarted));delay(3);
}
