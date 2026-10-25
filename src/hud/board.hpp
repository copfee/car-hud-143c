#pragma once
#include "core.hpp"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <lvgl.h>
namespace hud {
// Board-specific initialization from Waveshare's 1.43C reference BSP.
// Keep this adapter separate from the upstream display library.
class Panel final : public Arduino_CO5300 {
    Arduino_DataBus* bus_;
protected:
    void tftInit() override {
        pinMode(13,OUTPUT);digitalWrite(13,HIGH);delay(10);digitalWrite(13,LOW);delay(10);digitalWrite(13,HIGH);delay(120);
        bus_->beginWrite();
        bus_->writeC8D8(0xFE,0x00);bus_->writeC8D8(0xC4,0x80);bus_->writeC8D8(0x3A,0x55);
        bus_->writeC8D8(0x35,0x00);bus_->writeC8D8(0x53,0x20);bus_->writeC8D8(0x51,0x00);
        bus_->writeC8D8(0x36,0xC0);bus_->writeC8D8(0x63,0xFF);
        bus_->writeC8D16D16(0x2A,0x0006,0x01D7);bus_->writeC8D16D16(0x2B,0,0x01D1);
        bus_->endWrite();bus_->sendCommand(0x11);delay(100);bus_->sendCommand(0x29);
    }
public:
    explicit Panel(Arduino_DataBus* bus):Arduino_CO5300(bus,13,0,false,466,466,8,0,0,0),bus_(bus){}
};
class Board {
    Arduino_ESP32QSPI bus_{15,14,9,10,11,12};
    Panel panel_{&bus_};
    TouchDrvCSTXXX touch_;
    bool mirror_=false;
    bool touchReady_=false;
    uint64_t flushUs_=0;
    uint32_t flushes_=0,frames_=0,maxHandlerUs_=0,statsAt_=0;
    static Board* instance_;
    static void flush(lv_display_t* d,const lv_area_t* a,uint8_t* p) {
        uint32_t started=micros();
        instance_->panel_.draw16bitBeRGBBitmap(a->x1,a->y1,reinterpret_cast<uint16_t*>(p),lv_area_get_width(a),lv_area_get_height(a));
        instance_->flushUs_+=uint32_t(micros()-started);++instance_->flushes_;
        if(lv_display_flush_is_last(d))++instance_->frames_;
        // Arduino_GFX QSPI transfer is synchronous; the buffer is reusable on return.
        lv_display_flush_ready(d);
    }
    static void read(lv_indev_t*,lv_indev_data_t* d) {
        int16_t x=0,y=0;
        if(instance_->touchReady_ && instance_->touch_.getPoint(&x,&y,1)) {
            int tx=465-x,ty=465-y;
            if(instance_->mirror_)tx=465-tx;
            d->point.x=constrain(tx,0,465);d->point.y=constrain(ty,0,465);d->state=LV_INDEV_STATE_PRESSED;
        }else d->state=LV_INDEV_STATE_RELEASED;
    }
    static void alignArea(lv_event_t* e) {
        auto* a=lv_event_get_invalidated_area(e);
        a->x1&=~1;a->y1&=~1;a->x2=std::min(465,int(a->x2|1));a->y2=std::min(465,int(a->y2|1));
    }
public:
    bool begin() {
        instance_=this;
        if(!panel_.begin(40000000))return false;
        setMirror(false);panel_.setBrightness(0);panel_.fillScreen(0);setBrightness(SplashBrightnessPercent);
        touch_.setPins(16,17);touchReady_=touch_.begin(Wire,0x15,47,48);Wire.setClock(400000);
        analogReadResolution(12);analogSetPinAttenuation(4,ADC_11db);pinMode(7,INPUT_PULLUP);
        lv_init();lv_tick_set_cb([]()->uint32_t{return millis();});
        auto* display=lv_display_create(466,466);lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565_SWAPPED);
        constexpr size_t size=466*32*2;
        auto* a=heap_caps_malloc(size,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        auto* b=heap_caps_malloc(size,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
        if(!a||!b)return false;
        lv_display_set_buffers(display,a,b,size,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(display,flush);
        lv_display_add_event_cb(display,alignArea,LV_EVENT_INVALIDATE_AREA,nullptr);
        auto* input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read);
        lv_timer_set_period(lv_indev_get_read_timer(input),10);
        return true;
    }
    void setBrightness(int pct){panel_.setBrightness(constrain(pct,10,100)*255/100);}
    // GPIO4 / ADC1_CH3, equal-value divider; GPIO7 is active-low charger STAT.
    // Only the telemetry task uses ADC. No ADC conversions in LVGL callbacks.
    double readBatteryVoltage(){uint32_t mv=0;for(unsigned i=0;i<8;++i)mv+=analogReadMilliVolts(4);return double(mv)*2/(8*1000);}
    bool batteryCharging() const{return digitalRead(7)==LOW;}
    void setMirror(bool enabled){mirror_=enabled;bus_.beginWrite();bus_.writeC8D8(CO5300_W_MADCTL,uint8_t(0xC0^(enabled?0x40:0)));bus_.endWrite();}
    void profileHandler(uint32_t elapsedUs){
        maxHandlerUs_=std::max(maxHandlerUs_,elapsedUs);uint32_t now=millis(),elapsed=now-statsAt_;
        if(elapsed<5000)return;
        if(Serial&&Serial.availableForWrite()>=192)Serial.printf("[HUD-PERF] refresh_hz=%.1f flushes=%u transfer_ms=%.1f handler_max_ms=%.1f free_heap=%u\n",
            frames_*1000.0/elapsed,unsigned(flushes_),double(flushUs_)/1000,maxHandlerUs_/1000.0,unsigned(ESP.getFreeHeap()));
        statsAt_=now;flushUs_=0;flushes_=frames_=maxHandlerUs_=0;
    }
};
}
