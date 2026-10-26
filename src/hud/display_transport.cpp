#include "display_transport.hpp"
#include <cstring>
#include <esp_heap_caps.h>

namespace hud {

void DisplayTransport::select(){digitalWrite(Cs,LOW);csLow_=true;}
void DisplayTransport::deselect(){digitalWrite(Cs,HIGH);csLow_=false;}

bool DisplayTransport::addDevice(uint32_t frequency){
    spi_device_interface_config_t config{};
    config.command_bits=8;config.address_bits=24;config.mode=0;config.clock_speed_hz=frequency;
    config.spics_io_num=-1;config.flags=SPI_DEVICE_HALFDUPLEX;config.queue_size=2;
    return spi_bus_add_device(SPI2_HOST,&config,&device_)==ESP_OK;
}

bool DisplayTransport::begin(uint32_t frequency){
    pinMode(Cs,OUTPUT);deselect();
    spi_bus_config_t config{};
    config.mosi_io_num=Data0;config.miso_io_num=Data1;config.sclk_io_num=Clock;
    config.quadwp_io_num=Data2;config.quadhd_io_num=Data3;config.data4_io_num=-1;
    config.data5_io_num=-1;config.data6_io_num=-1;config.data7_io_num=-1;
    config.max_transfer_sz=StageBytes+8;config.flags=SPICOMMON_BUSFLAG_MASTER|SPICOMMON_BUSFLAG_GPIO_PINS;
    if(spi_bus_initialize(SPI2_HOST,&config,SPI_DMA_CH_AUTO)!=ESP_OK)return false;
    busReady_=true;
    if(!addDevice(frequency))return false;
    for(auto& slot:slots_){
        slot.data=static_cast<uint8_t*>(heap_caps_aligned_alloc(16,StageBytes,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL));
        if(!slot.data)return false;
    }
    return true;
}

bool DisplayTransport::setFrequency(uint32_t frequency){
    if(!wait()||!device_)return false;
    if(spi_bus_remove_device(device_)!=ESP_OK)return false;
    device_=nullptr;return addDevice(frequency);
}

bool DisplayTransport::commandData(uint8_t command,const uint8_t* data,size_t length){
    if(!device_||length>4||!wait())return false;
    spi_transaction_ext_t transaction{};
    transaction.base.flags=SPI_TRANS_MULTILINE_CMD|SPI_TRANS_MULTILINE_ADDR;
    transaction.base.cmd=0x02;transaction.base.addr=uint32_t(command)<<8;
    if(length){
        transaction.base.flags|=SPI_TRANS_USE_TXDATA;
        std::memcpy(transaction.base.tx_data,data,length);
        transaction.base.length=length*8;
    }
    select();auto result=spi_device_polling_transmit(device_,reinterpret_cast<spi_transaction_t*>(&transaction));deselect();
    return result==ESP_OK;
}

bool DisplayTransport::setWindow(const DisplayRect& area){
    uint16_t x1=uint16_t(area.x1+8),x2=uint16_t(area.x2+8),y1=uint16_t(area.y1),y2=uint16_t(area.y2);
    uint8_t x[]={uint8_t(x1>>8),uint8_t(x1),uint8_t(x2>>8),uint8_t(x2)};
    uint8_t y[]={uint8_t(y1>>8),uint8_t(y1),uint8_t(y2>>8),uint8_t(y2)};
    return commandData(0x2A,x,sizeof(x))&&commandData(0x2B,y,sizeof(y))&&command(0x2C);
}

DisplayTransport::Slot* DisplayTransport::writableSlot(){
    if(pending_==2&&!reapOne())return nullptr;
    unsigned index=nextSlot_;
    if(slots_[index].queued)index^=1;
    if(slots_[index].queued)return nullptr;
    nextSlot_=index^1;return &slots_[index];
}

bool DisplayTransport::queueSlot(Slot& slot,size_t length,bool first){
    slot.transaction={};
    auto& base=slot.transaction.base;
    base.flags=SPI_TRANS_MODE_QIO;base.tx_buffer=slot.data;base.length=length*8;
    if(first){base.cmd=0x32;base.addr=0x003C00;}
    else{
        base.flags|=SPI_TRANS_VARIABLE_CMD|SPI_TRANS_VARIABLE_ADDR|SPI_TRANS_VARIABLE_DUMMY;
        slot.transaction.command_bits=0;slot.transaction.address_bits=0;slot.transaction.dummy_bits=0;
    }
    if(spi_device_queue_trans(device_,reinterpret_cast<spi_transaction_t*>(&slot.transaction),portMAX_DELAY)!=ESP_OK)return false;
    slot.queued=true;++pending_;return true;
}

bool DisplayTransport::reapOne(){
    if(!pending_)return true;
    spi_transaction_t* completed=nullptr;
    if(spi_device_get_trans_result(device_,&completed,portMAX_DELAY)!=ESP_OK)return false;
    for(auto& slot:slots_)if(completed==reinterpret_cast<spi_transaction_t*>(&slot.transaction)){slot.queued=false;break;}
    --pending_;return true;
}

bool DisplayTransport::startPixels(const uint8_t* source,unsigned stridePixels,int sourceX,int sourceY,const DisplayRect& area){
    if(!source||!valid(area)||!wait()||!setWindow(area))return false;
    const unsigned width=unsigned(area.x2-area.x1+1),height=unsigned(area.y2-area.y1+1);
    size_t cursor=0,total=size_t(width)*height,staged=0;bool first=true;
    select();
    while(cursor<total){
        auto* slot=writableSlot();if(!slot){wait();return false;}
        staged=0;
        while(cursor<total&&staged<StageBytes){
            unsigned row=unsigned(cursor/width),column=unsigned(cursor%width);
            size_t pixels=std::min<size_t>(width-column,(StageBytes-staged)/2);
            auto* rowSource=source+(size_t(area.y1-sourceY+int(row))*stridePixels+area.x1-sourceX+int(column))*2;
            std::memcpy(slot->data+staged,rowSource,pixels*2);
            staged+=pixels*2;cursor+=pixels;
        }
        if(!queueSlot(*slot,staged,first)){wait();return false;}
        first=false;
    }
    return true;
}

bool DisplayTransport::startSolid(uint16_t value,const DisplayRect& area){
    if(!valid(area)||!wait()||!setWindow(area))return false;
    size_t remaining=size_t(area.x2-area.x1+1)*size_t(area.y2-area.y1+1)*2;bool first=true;
    select();
    while(remaining){
        size_t length=std::min(remaining,StageBytes);auto* slot=writableSlot();if(!slot){wait();return false;}
        auto* pixels=reinterpret_cast<uint16_t*>(slot->data);for(size_t i=0;i<length/2;++i)pixels[i]=value;
        if(!queueSlot(*slot,length,first)){wait();return false;}
        first=false;remaining-=length;
    }
    return true;
}

bool DisplayTransport::wait(){
    bool ok=true;while(pending_)ok=reapOne()&&ok;
    if(csLow_)deselect();return ok;
}

bool DisplayTransport::initializePanel(){
    pinMode(13,OUTPUT);digitalWrite(13,HIGH);delay(10);digitalWrite(13,LOW);delay(10);digitalWrite(13,HIGH);delay(120);
    if(!writeRegister(0xFE,0x00)||!writeRegister(0xC4,0x80)||!writeRegister(0x3A,0x55)||
       !writeRegister(0x35,0x00)||!writeRegister(0x53,0x20)||!writeRegister(0x51,0x00)||
       !writeRegister(0x36,0xC0)||!writeRegister(0x63,0xFF))return false;
    uint8_t x[]={0x00,0x06,0x01,0xD7},y[]={0x00,0x00,0x01,0xD1};
    if(!commandData(0x2A,x,sizeof(x))||!commandData(0x2B,y,sizeof(y))||!command(0x11))return false;
    delay(100);return command(0x29);
}

}
