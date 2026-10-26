#pragma once
#include "core.hpp"
#include "display_policy.hpp"
#include "display_transport.hpp"
#include <Arduino.h>
#include <TouchDrvCSTXXX.hpp>
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <lvgl.h>

namespace hud {

class Board {
    static constexpr unsigned Width=466,Height=466;
    static constexpr size_t FrameBytes=size_t(Width)*Height*2;
    DisplayTransport transport_;
    TouchDrvCSTXXX touch_;
    lv_display_t* display_=nullptr;
    SemaphoreHandle_t teSignal_=nullptr;
    DirtyRegions dirty_;
    uint8_t* frame_=nullptr;
    bool mirror_=false,touchReady_=false,direct_=false,teReady_=false,pendingFlush_=false;
    int numericPage_=-1;
    volatile uint32_t teCount_=0;
    uint32_t lastFullTe_=0,lastFullMs_=0,consecutiveTeTimeouts_=0;
    uint64_t transferBytes_=0,transferUs_=0,teWaitUs_=0;
    uint32_t regions_=0,fullFrames_=0,numericFrames_=0,teTimeouts_=0,maxHandlerUs_=0,statsAt_=0;
    uint32_t pendingStartedUs_=0,pendingBytes_=0;

    static Board* instance_;
    static void teInterrupt(){
        if(!instance_)return;
        ++instance_->teCount_;BaseType_t wake=pdFALSE;
        if(instance_->teSignal_)xSemaphoreGiveFromISR(instance_->teSignal_,&wake);
        if(wake)portYIELD_FROM_ISR();
    }
    static DisplayRect rect(const lv_area_t* area){return {int16_t(area->x1),int16_t(area->y1),int16_t(area->x2),int16_t(area->y2)};}
    static void alignArea(lv_event_t* event){
        auto* area=lv_event_get_invalidated_area(event);auto adjusted=aligned(rect(area));
        if(instance_->direct_&&!numericOnly(adjusted,instance_->numericPage_))adjusted=FullDisplay;
        area->x1=adjusted.x1;area->y1=adjusted.y1;area->x2=adjusted.x2;area->y2=adjusted.y2;
    }
    static void flush(lv_display_t* display,const lv_area_t* area,uint8_t* pixels){instance_->flushArea(display,area,pixels);}
    static void flushWait(lv_display_t*){instance_->waitDisplayIdle();}
    static void read(lv_indev_t*,lv_indev_data_t* data){
        int16_t x=0,y=0;
        if(instance_->touchReady_&&instance_->touch_.getPoint(&x,&y,1)){
            int tx=465-x,ty=465-y;if(instance_->mirror_)tx=465-tx;
            data->point.x=constrain(tx,0,465);data->point.y=constrain(ty,0,465);data->state=LV_INDEV_STATE_PRESSED;
        }else data->state=LV_INDEV_STATE_RELEASED;
    }
    bool waitForTearing(bool full){
        if(!teReady_){
            if(full){uint32_t elapsed=millis()-lastFullMs_;if(elapsed<33)delay(33-elapsed);lastFullMs_=millis();}
            return false;
        }
        uint32_t started=micros(),current=teCount_,target=current+1;
        if(full&&int32_t(lastFullTe_+2-target)>0)target=lastFullTe_+2;
        while(int32_t(teCount_-target)<0){
            while(xSemaphoreTake(teSignal_,0)==pdTRUE){}
            if(int32_t(teCount_-target)>=0)break;
            if(xSemaphoreTake(teSignal_,pdMS_TO_TICKS(22))!=pdTRUE){
                ++teTimeouts_;++consecutiveTeTimeouts_;teWaitUs_+=uint32_t(micros()-started);
                if(consecutiveTeTimeouts_>=3)teReady_=false;
                return false;
            }
        }
        consecutiveTeTimeouts_=0;if(full){lastFullTe_=teCount_;lastFullMs_=millis();}
        teWaitUs_+=uint32_t(micros()-started);return true;
    }
    static uint32_t areaBytes(const DisplayRect& area){return uint32_t(area.x2-area.x1+1)*uint32_t(area.y2-area.y1+1)*2;}
    void finishPending(){
        if(!pendingFlush_)return;
        transport_.wait();transferUs_+=uint32_t(micros()-pendingStartedUs_);transferBytes_+=pendingBytes_;
        pendingBytes_=0;pendingFlush_=false;if(display_)lv_display_flush_ready(display_);
    }
    void flushArea(lv_display_t* display,const lv_area_t* area,uint8_t* pixels){
        if(!direct_){
            auto region=rect(area);uint32_t started=micros();
            bool ok=transport_.startPixels(pixels,unsigned(area->x2-area->x1+1),area->x1,area->y1,region)&&transport_.wait();
            transferUs_+=uint32_t(micros()-started);transferBytes_+=areaBytes(region);++regions_;
            if(!ok&&Serial)Serial.println("[HUD-DISPLAY] fallback transfer failed");
            lv_display_flush_ready(display);return;
        }
        dirty_.add(rect(area),numericPage_);frame_=pixels;
        if(!lv_display_flush_is_last(display)){lv_display_flush_ready(display);return;}
        pendingBytes_=0;bool ok=true;
        if(dirty_.full()){
            waitForTearing(true);pendingStartedUs_=micros();pendingBytes_=areaBytes(FullDisplay);++regions_;++fullFrames_;
            ok=transport_.startPixels(frame_,Width,0,0,FullDisplay);
        }else if(dirty_.count()){
            waitForTearing(false);pendingStartedUs_=micros();++numericFrames_;
            for(size_t i=0;i<dirty_.count()&&ok;++i){
                const auto& region=dirty_[i];pendingBytes_+=areaBytes(region);++regions_;
                ok=transport_.startPixels(frame_,Width,0,0,region);
                if(i+1<dirty_.count())ok=transport_.wait();
            }
        }
        dirty_.clear();
        if(!ok||!pendingBytes_){
            transport_.wait();if(!ok&&Serial)Serial.println("[HUD-DISPLAY] DMA transfer failed");
            lv_display_flush_ready(display);return;
        }
        display_=display;pendingFlush_=true;
    }
public:
    bool begin(){
        instance_=this;
        if(!transport_.begin(80000000)||!transport_.initializePanel())return false;
        teSignal_=xSemaphoreCreateBinary();if(!teSignal_)return false;
        pinMode(8,INPUT);attachInterrupt(digitalPinToInterrupt(8),teInterrupt,RISING);delay(55);
        teReady_=teCount_>=2;lastFullTe_=teCount_;lastFullMs_=millis();
        void* first=nullptr;void* second=nullptr;
        if(psramFound()){
            first=heap_caps_aligned_alloc(64,FrameBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            second=heap_caps_aligned_alloc(64,FrameBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        }
        direct_=first&&second&&teReady_;
        if(!direct_){
            if(first)heap_caps_free(first);if(second)heap_caps_free(second);
            if(!transport_.setFrequency(40000000))return false;constexpr size_t fallbackBytes=Width*16*2;
            first=heap_caps_aligned_alloc(16,fallbackBytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
            second=heap_caps_aligned_alloc(16,fallbackBytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
            if(!first||!second)return false;
        }
        size_t bufferBytes=direct_?FrameBytes:Width*16*2;
        std::memset(first,0,bufferBytes);std::memset(second,0,bufferBytes);
        if(!transport_.startSolid(0,FullDisplay)||!transport_.wait())return false;
        writeBrightness(0);setMirror(false);writeBrightness(SplashBrightnessPercent);
        touch_.setPins(16,17);touchReady_=touch_.begin(Wire,0x15,47,48);Wire.setClock(400000);
        analogReadResolution(12);analogSetPinAttenuation(4,ADC_11db);pinMode(7,INPUT_PULLUP);
        lv_init();lv_tick_set_cb([]()->uint32_t{return millis();});
        display_=lv_display_create(Width,Height);lv_display_set_color_format(display_,LV_COLOR_FORMAT_RGB565_SWAPPED);
        lv_display_set_buffers(display_,first,second,bufferBytes,direct_?LV_DISPLAY_RENDER_MODE_DIRECT:LV_DISPLAY_RENDER_MODE_PARTIAL);
        if(!direct_)lv_timer_set_period(lv_display_get_refr_timer(display_),33);
        lv_display_set_flush_cb(display_,flush);lv_display_set_flush_wait_cb(display_,flushWait);
        lv_display_add_event_cb(display_,alignArea,LV_EVENT_INVALIDATE_AREA,nullptr);
        auto* input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,read);
        lv_timer_set_period(lv_indev_get_read_timer(input),10);statsAt_=millis();
        if(Serial)Serial.printf("[HUD-DISPLAY] mode=%s qspi_mhz=%u te_edges=%u buffers=%u\n",direct_?"direct-psram":"partial-fallback",direct_?80u:40u,unsigned(teCount_),unsigned(bufferBytes));
        return true;
    }
    void waitDisplayIdle(){finishPending();transport_.wait();}
    void writeBrightness(int pct){waitDisplayIdle();transport_.writeRegister(0x51,uint8_t(constrain(pct,0,100)*255/100));}
    void setBrightness(int pct){writeBrightness(constrain(pct,10,100));}
    double readBatteryVoltage(){uint32_t mv=0;for(unsigned i=0;i<8;++i)mv+=analogReadMilliVolts(4);return double(mv)*2/(8*1000);}
    bool batteryCharging()const{return digitalRead(7)==LOW;}
    void setMirror(bool enabled){waitDisplayIdle();mirror_=enabled;transport_.writeRegister(0x36,uint8_t(0xC0^(enabled?0x40:0)));}
    bool directMode()const{return direct_;}
    void setNumericPage(int page){numericPage_=page==0||page==1?page:-1;}
    void profileHandler(uint32_t elapsedUs){
        maxHandlerUs_=std::max(maxHandlerUs_,elapsedUs);uint32_t now=millis(),elapsed=now-statsAt_;if(elapsed<5000)return;
        if(Serial&&Serial.availableForWrite()>=240)Serial.printf(
            "[HUD-PERF] mode=%s te=%u full_hz=%.1f numeric_hz=%.1f regions=%u bytes_kb=%.1f dma_ms=%.1f te_wait_ms=%.1f te_timeouts=%u handler_max_ms=%.1f free_heap=%u\n",
            direct_?"direct":"fallback",teReady_?1u:0u,fullFrames_*1000.0/elapsed,numericFrames_*1000.0/elapsed,unsigned(regions_),transferBytes_/1024.0,
            transferUs_/1000.0,teWaitUs_/1000.0,unsigned(teTimeouts_),maxHandlerUs_/1000.0,unsigned(ESP.getFreeHeap()));
        statsAt_=now;transferBytes_=transferUs_=teWaitUs_=0;regions_=fullFrames_=numericFrames_=teTimeouts_=maxHandlerUs_=0;
    }
};

}
