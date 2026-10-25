#pragma once
#include "core.hpp"
#include <lvgl.h>
namespace hud {
class Interface {
public:
    void create();
    void refresh(const Snapshot& s, uint32_t now, uint32_t sinceAttempt);
    Action takeAction();
    PageId page() const;
    int brightness() const;
    bool mirror() const;
    VehicleProfile profile()const;
    uint8_t protocol()const;
    void showPage(PageId page, bool animated=true);
    bool scrolling() const;
    BlePeer selectedBluetoothPeer() const{return chosenPeer_;}
private:
    lv_obj_t *splash_=nullptr,*logo_=nullptr,*boot_=nullptr,*dash_=nullptr,*settingsScroll_=nullptr,*tiles_=nullptr;
    lv_obj_t* pages_[PageCount]{};
    lv_obj_t *status_=nullptr,*countdown_=nullptr,*spinner_=nullptr,*retry_=nullptr,*demoButton_=nullptr;
    lv_obj_t *bootPeer_=nullptr,*bootBluetooth_=nullptr,*bluetoothScreen_=nullptr,*bluetoothList_=nullptr,*bluetoothStatus_=nullptr,*scanButton_=nullptr,*cancelBluetooth_=nullptr;
    lv_obj_t *savedPeer_=nullptr,*peerState_=nullptr;
    std::vector<BlePeer> renderedPeers_;
    BlePeer chosenPeer_;
    uint32_t bluetoothRevision_=0xffffffffu;
    lv_obj_t *number_[Count]{},*gauge_=nullptr,*pedalBar_=nullptr,*throttleBar_=nullptr,*brakeBar_=nullptr;
    lv_obj_t *demoBadge_[PageCount]{},*dots_[PageCount][PageCount]{},*context_=nullptr,*slider_=nullptr,*mirror_=nullptr;
    lv_obj_t *batteryValue_=nullptr,*batteryState_=nullptr,*batteryFill_=nullptr,*brightnessValue_=nullptr;
    lv_obj_t *connectionState_=nullptr,*address_=nullptr,*signal_=nullptr,*elm_=nullptr,*protocol_=nullptr,*lastReply_=nullptr;
    lv_obj_t *chip_=nullptr,*capacity_=nullptr,*heap_=nullptr,*uptime_=nullptr;
    lv_obj_t *profileSelect_=nullptr,*protocolSelect_=nullptr,*model_=nullptr,*vin_=nullptr,*ecuName_=nullptr,*identityState_=nullptr,*availability_=nullptr;
    Mode displayed_=Mode::Timeout;
    Action action_=Action::None;
    uint32_t lastNumbers_=0,lastInfo_=0;
    bool demo_=false;
    bool scrolling_=false, settingsScrolling_=false, visualStateKnown_=false, previousDemo_=false;
    int previousPage_=-1;
    static void clicked(lv_event_t* e);
    static void control(lv_event_t* e);
    static void scrollEvent(lv_event_t* e);
    void createSettings(lv_obj_t* page);
    void refreshSettings(const Snapshot& s,uint32_t now);
    void createBluetooth();
    void refreshBluetooth(const Snapshot& s);
};
}
