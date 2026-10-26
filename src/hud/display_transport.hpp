#pragma once
#include "display_policy.hpp"
#include <Arduino.h>
#include <driver/spi_master.h>

namespace hud {

class DisplayTransport {
    static constexpr int Cs=15,Clock=14,Data0=9,Data1=10,Data2=11,Data3=12;
    static constexpr size_t StageBytes=16*1024;
    struct Slot {
        spi_transaction_ext_t transaction{};
        uint8_t* data=nullptr;
        bool queued=false;
    } slots_[2];
    spi_device_handle_t device_=nullptr;
    bool busReady_=false,csLow_=false;
    unsigned pending_=0,nextSlot_=0;

    bool addDevice(uint32_t frequency);
    bool commandData(uint8_t command,const uint8_t* data,size_t length);
    bool setWindow(const DisplayRect& area);
    Slot* writableSlot();
    bool queueSlot(Slot& slot,size_t length,bool first);
    bool reapOne();
    void select();
    void deselect();
public:
    bool begin(uint32_t frequency);
    bool setFrequency(uint32_t frequency);
    bool initializePanel();
    bool command(uint8_t command){return commandData(command,nullptr,0);}
    bool writeRegister(uint8_t command,uint8_t value){return commandData(command,&value,1);}
    bool startPixels(const uint8_t* source,unsigned stridePixels,int sourceX,int sourceY,const DisplayRect& area);
    bool startSolid(uint16_t value,const DisplayRect& area);
    bool wait();
    bool busy()const{return pending_!=0;}
};

}
