#include "ui.hpp"
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <filesystem>
using namespace hud;
LV_IMAGE_DECLARE(hud_mercedes_logo);
static std::vector<uint32_t> image(466*466);
static bool rgb565=false;
static void flush(lv_display_t* d,const lv_area_t* a,uint8_t* pixels){
    auto* p=reinterpret_cast<uint32_t*>(pixels);
    for(int y=a->y1;y<=a->y2;++y)for(int x=a->x1;x<=a->x2;++x){
        if(rgb565){
            uint16_t v=(uint16_t(pixels[0])<<8)|pixels[1];pixels+=2;
            uint8_t r=v>>11,g=(v>>5)&63,b=v&31;
            image[y*466+x]=(uint32_t((r<<3)|(r>>2))<<16)|(uint32_t((g<<2)|(g>>4))<<8)|((b<<3)|(b>>2));
        }else image[y*466+x]=*p++;
    }
    lv_display_flush_ready(d);
}
static uint32_t now=0;
static uint32_t attemptAt=0;
static int touchX=0,touchY=0;static bool pressed=false;
static void readTouch(lv_indev_t*,lv_indev_data_t* d){d->point={touchX,touchY};d->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void run(Interface& ui,const Snapshot& s,unsigned ms){for(unsigned t=0;t<ms;t+=10){now+=10;lv_tick_inc(10);ui.refresh(s,now,now-attemptAt);lv_timer_handler();}}
static void drag(Interface& ui,const Snapshot& s,int x1,int y1,int x2,int y2){
    touchX=x1;touchY=y1;pressed=true;run(ui,s,30);
    for(int n=1;n<=12;++n){touchX=x1+(x2-x1)*n/12;touchY=y1+(y2-y1)*n/12;run(ui,s,20);}
    pressed=false;run(ui,s,700);
}
static void save(const std::string& path){
    FILE* f=fopen(path.c_str(),"wb");assert(f);fprintf(f,"P6\n466 466\n255\n");
    for(int y=0;y<466;++y)for(int x=0;x<466;++x){auto c=image[y*466+x];if((x-233)*(x-233)+(y-233)*(y-233)>233*233)c=0;uint8_t b[3]={uint8_t(c>>16),uint8_t(c>>8),uint8_t(c)};fwrite(b,1,3,f);}fclose(f);
}
static lv_obj_t* text(lv_obj_t* o,const char* name){
    if(lv_obj_check_type(o,&lv_label_class)&&!strcmp(lv_label_get_text(o),name))return o;
    for(unsigned i=0;i<lv_obj_get_child_count(o);++i)if(auto* r=text(lv_obj_get_child(o,i),name))return r;
    return nullptr;
}
static lv_obj_t* object(lv_obj_t* o,const lv_obj_class_t* type){
    if(lv_obj_check_type(o,type))return o;
    for(unsigned i=0;i<lv_obj_get_child_count(o);++i)if(auto* r=object(lv_obj_get_child(o,i),type))return r;
    return nullptr;
}
int main(int argc,char** argv){
    std::string out=argc>1?argv[1]:".";
    std::filesystem::create_directories(out);rgb565=argc>2&&std::strcmp(argv[2],"--rgb565")==0;
    lv_init();auto* d=lv_display_create(466,466);static uint32_t buf[466*32];
    lv_display_set_color_format(d,rgb565?LV_COLOR_FORMAT_RGB565_SWAPPED:LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,buf,nullptr,466*32*(rgb565?2:4),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    auto* input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,readTouch);lv_timer_set_period(lv_indev_get_read_timer(input),10);
    Interface ui;ui.create();Snapshot s;
    lv_refr_now(d);auto* splash=lv_screen_active();auto* logo=object(splash,&lv_image_class);
    assert(s.mode==Mode::Splash&&logo&&lv_obj_get_child_count(splash)==1);
    assert(lv_obj_get_width(logo)==466&&lv_obj_get_height(logo)==466&&lv_obj_get_x(logo)==0&&lv_obj_get_y(logo)==0);
    assert(hud_mercedes_logo.header.cf==LV_COLOR_FORMAT_RGB565&&hud_mercedes_logo.data_size==466*466*2);
    assert(std::all_of(image.begin(),image.end(),[](uint32_t c){return (c&0xffffff)==0;}));
    save(out+"/splash-0000.ppm");SplashTimeline timeline;timeline.start(now);
    uint64_t halfBrightness=0,fullBrightness=0;
    for(unsigned elapsed=10;elapsed<=SplashDurationMs;elapsed+=10){
        now+=10;lv_tick_inc(10);s.splashElapsed=timeline.elapsed(now);
        if(timeline.takeCompletion(now)){s.mode=Mode::Loading;attemptAt=now;}
        ui.refresh(s,now,now-attemptAt);lv_timer_handler();
        if(elapsed<SplashDurationMs){
            assert(lv_screen_active()==splash&&!text(splash,"车辆连接")&&!text(splash,"正在扫描蓝牙"));
            assert(lv_obj_get_style_image_opa(logo,LV_PART_MAIN)==splashOpacity(elapsed));
            if(elapsed%50==0){
                lv_refr_now(d);char frame[40];snprintf(frame,sizeof(frame),"/splash-%04u.ppm",elapsed);save(out+frame);
                if(elapsed==150||elapsed==300){uint64_t sum=0;for(auto c:image)sum+=((c>>16)&255)+((c>>8)&255)+(c&255);
                    if(elapsed==150)halfBrightness=sum;else {
                        fullBrightness=sum;
                        if(rgb565)for(unsigned i=0;i<image.size();++i){
                            auto* source=hud_mercedes_logo.data+2*i;uint16_t v=source[0]|(uint16_t(source[1])<<8);
                            assert(((image[i]>>19)&31)==(v>>11)&&((image[i]>>10)&63)==((v>>5)&63)&&((image[i]>>3)&31)==(v&31));
                        }
                    }
                }
            }
        }
    }
    assert(fullBrightness>1000000&&halfBrightness>fullBrightness/3&&halfBrightness<fullBrightness*2/3);
    // Loading is the active screen immediately: no extra 180ms fade after the logo.
    assert(lv_screen_active()!=splash&&text(lv_screen_active(),"正在扫描蓝牙"));
    assert(text(lv_screen_active(),"等待连接 0 / 60 秒"));
    assert(!timeline.takeCompletion(now+10000));
    lv_refr_now(d);save(out+"/splash-3000.ppm");
    run(ui,s,100);save(out+"/loading.ppm");assert(text(lv_screen_active(),"正在扫描蓝牙"));
    s.mode=Mode::Timeout;s.status="连接超时";run(ui,s,200);save(out+"/timeout.ppm");
    assert(ui.takeAction()==Action::None);
    auto* retry=lv_obj_get_parent(text(lv_screen_active(),"重试"));auto* demo=lv_obj_get_parent(text(lv_screen_active(),"演示模式"));
    assert(!lv_obj_has_flag(retry,LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(demo,LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(retry,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::Retry);
    lv_obj_send_event(demo,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::Demo);
    auto* choose=lv_obj_get_parent(text(lv_screen_active(),"选择蓝牙设备"));lv_obj_send_event(choose,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::Bluetooth);
    s.mode=Mode::Bluetooth;s.bluetooth.scanning=true;s.bluetooth.revision=1;s.bluetooth.detail="正在扫描附近设备";run(ui,s,250);
    assert(text(lv_screen_active(),"选择蓝牙设备")&&text(lv_screen_active(),"正在查找设备…"));
    auto* rescan=lv_obj_get_parent(text(lv_screen_active(),"重新扫描"));assert(lv_obj_has_state(rescan,LV_STATE_DISABLED));
    s.bluetooth.scanning=false;s.bluetooth.detail="请选择设备，连接后验证 OBD 通道";
    s.bluetooth.devices={{"AA:BB:CC:DD:EE:01","IOS-Vlink",0,-42,true},{"AA:BB:CC:DD:EE:02","Another BLE adapter with a long device name",1,-64,false}};++s.bluetooth.revision;
    run(ui,s,150);assert(!lv_obj_has_state(rescan,LV_STATE_DISABLED));save(out+"/bluetooth-picker.ppm");
    auto* card=lv_obj_get_parent(text(lv_screen_active(),"IOS-Vlink"));lv_obj_send_event(card,LV_EVENT_CLICKED,nullptr);
    assert(ui.takeAction()==Action::SelectBluetooth&&ui.selectedBluetoothPeer().address=="AA:BB:CC:DD:EE:01");
    // Selection comes from the rendered row, even if the producer's next scan reorders its snapshot.
    std::reverse(s.bluetooth.devices.begin(),s.bluetooth.devices.end());
    lv_obj_send_event(card,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::SelectBluetooth&&ui.selectedBluetoothPeer().address=="AA:BB:CC:DD:EE:01");
    lv_obj_send_event(rescan,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::ScanBluetooth);
    auto* cancel=lv_obj_get_parent(text(lv_screen_active(),"返回"));lv_obj_send_event(cancel,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::CancelBluetooth);
    s.bluetooth.devices.clear();++s.bluetooth.revision;s.bluetooth.detail="未发现设备，请重新扫描";run(ui,s,100);
    assert(text(lv_screen_active(),"未发现设备，请确认适配器供电后重新扫描。"));save(out+"/bluetooth-empty.ppm");
    s.mode=Mode::Demo;s.demo=true;DemoGenerator generator;generator.tick(s,now,28);run(ui,s,400);save(out+"/overview.ppm");
    assert(text(lv_screen_active(),"演示"));assert(!text(lv_screen_active(),"BLE 已连接"));assert(!text(lv_screen_active(),"候选"));
    s.samples[Speed].shown=s.samples[Speed].raw=77;
    s.samples[Pedal].shown=s.samples[Pedal].raw=12.3;
    run(ui,s,150);auto* speedLabel=text(lv_screen_active(),"77");assert(speedLabel);
    assert(!text(lv_screen_active(),"12.3%")); // Hidden page was not redrawn.
    ui.showPage(PageId::Pedals,true);assert(ui.scrolling());
    s.samples[Speed].shown=s.samples[Speed].raw=88;
    s.samples[Pedal].shown=s.samples[Pedal].raw=66.6;
    run(ui,s,100);save(out+"/swipe.ppm");
    assert(ui.scrolling());assert(!strcmp(lv_label_get_text(speedLabel),"77"));
    assert(!text(lv_screen_active(),"66.6%"));
    run(ui,s,500);assert(ui.page()==PageId::Pedals&&!ui.scrolling());save(out+"/pedals.ppm");
    assert(text(lv_screen_active(),"66.6%")); // Catch up with the newest snapshot on release.
    assert(!strcmp(lv_label_get_text(speedLabel),"77"));
    ui.showPage(PageId::Overview,false);run(ui,s,150);assert(!strcmp(lv_label_get_text(speedLabel),"88"));
    updateBattery(s.battery,3.92,false,now);snprintf(s.system.chip,sizeof(s.system.chip),"ESP32-S3 rev.0");
    s.system.flashBytes=s.system.psramBytes=8*1048576;s.system.freeHeap=156*1024;s.system.uptimeSeconds=7256;
    ui.showPage(PageId::Settings,true);run(ui,s,650);assert(ui.page()==PageId::Settings&&!ui.scrolling());save(out+"/settings.ppm");
    assert(text(lv_screen_active(),"~63%")&&text(lv_screen_active(),"3.92 V / 未充电"));
    assert(text(lv_screen_active(),"演示模式\n实车连接已停止"));
    auto* viewport=lv_obj_get_parent(text(lv_screen_active(),"开发板电池"));
    auto* batteryTitle=text(viewport,"开发板电池");
    auto* batteryRow=lv_obj_get_child(viewport,lv_obj_get_index(batteryTitle)+1);
    auto* batteryIconBox=lv_obj_get_child(batteryRow,1);
    auto* batteryBody=lv_obj_get_child(batteryIconBox,0);auto* batteryTerminal=lv_obj_get_child(batteryIconBox,1);
    assert(lv_obj_get_y(batteryBody)+lv_obj_get_height(batteryBody)/2==lv_obj_get_y(batteryTerminal)+lv_obj_get_height(batteryTerminal)/2);
    assert(lv_obj_get_scroll_dir(viewport)==LV_DIR_VER);
    assert(lv_obj_has_flag(viewport,LV_OBJ_FLAG_SCROLL_MOMENTUM));
    auto* slider=object(viewport,&lv_slider_class);assert(slider&&!lv_obj_has_flag(slider,LV_OBJ_FLAG_SCROLL_CHAIN_HOR));
    auto* mirror=object(viewport,&lv_switch_class);lv_area_t viewArea,knobArea,mirrorArea;
    lv_obj_get_coords(viewport,&viewArea);lv_obj_get_coords(slider,&knobArea);lv_obj_get_coords(mirror,&mirrorArea);
    assert(knobArea.y1>=viewArea.y1&&knobArea.y2<=viewArea.y2&&mirrorArea.y2<=viewArea.y2);
    for(int x:{viewArea.x1,viewArea.x2})for(int y:{viewArea.y1,viewArea.y2})assert((x-233)*(x-233)+(y-233)*(y-233)<233*233);
    lv_obj_scroll_to_y(viewport,145,LV_ANIM_OFF);run(ui,s,50);int scroll=lv_obj_get_scroll_y(viewport);
    ui.showPage(PageId::Pedals,false);run(ui,s,100);ui.showPage(PageId::Settings,false);run(ui,s,100);
    assert(lv_obj_get_scroll_y(viewport)==scroll);
    lv_obj_scroll_to_y(viewport,scroll+40,LV_ANIM_ON);assert(ui.scrolling());
    ui.showPage(PageId::Overview,false);s.samples[Speed].shown=s.samples[Speed].raw=91;run(ui,s,100);
    assert(!ui.scrolling()&&!strcmp(lv_label_get_text(speedLabel),"91"));
    run(ui,s,700);ui.showPage(PageId::Settings,false);run(ui,s,100);scroll=lv_obj_get_scroll_y(viewport);
    // Real pointer gestures: vertical scrolling must not change tile; horizontal drag must.
    drag(ui,s,110,330,110,160);assert(ui.page()==PageId::Settings&&lv_obj_get_scroll_y(viewport)>scroll);
    drag(ui,s,120,300,390,300);assert(ui.page()==PageId::Pedals);
    drag(ui,s,390,300,100,300);assert(ui.page()==PageId::Settings);
    // Bring the brightness knob into view and verify it consumes horizontal motion.
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(slider)-125,LV_ANIM_OFF);run(ui,s,100);
    lv_area_t sliderArea;lv_obj_get_coords(slider,&sliderArea);int oldBrightness=ui.brightness();
    drag(ui,s,sliderArea.x1+115,(sliderArea.y1+sliderArea.y2)/2,sliderArea.x2-8,(sliderArea.y1+sliderArea.y2)/2);
    assert(ui.page()==PageId::Settings&&ui.brightness()>oldBrightness&&ui.takeAction()==Action::Brightness);
    s.brightness=ui.brightness();
    lv_obj_add_state(mirror,LV_STATE_CHECKED);lv_obj_send_event(mirror,LV_EVENT_VALUE_CHANGED,nullptr);
    assert(ui.takeAction()==Action::Mirror&&ui.mirror());s.mirror=true;
    s.mode=Mode::Live;s.demo=false;s.connection.ble=s.connection.ecu=s.connection.rssiValid=true;s.connection.rssi=-45;
    s.connection.state=ConnectionState::Running;s.connection.info.detected=7;snprintf(s.connection.info.description,sizeof(s.connection.info.description),"ISO 15765-4 CAN 29/500");
    snprintf(s.connection.address,sizeof(s.connection.address),"AA:BB:CC:DD:EE:FF");snprintf(s.connection.elm,sizeof(s.connection.elm),"ELM327 v2.3");snprintf(s.connection.protocol,sizeof(s.connection.protocol),"7");s.connection.activeProtocol=7;s.connection.lastReply=now;
    s.identity.vin="1D4GP00R55B123456";s.identity.ecu="ECU 7E8 / ENGINE-CONTROL";s.identity.detail="ECU 7E8 / 读取成功";s.identity.verified=true;
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"车辆信息")),LV_ANIM_OFF);run(ui,s,1200);save(out+"/settings-vehicle.ppm");
    assert(text(viewport,s.identity.vin.c_str())&&text(viewport,"未识别"));
    auto* vin=text(viewport,s.identity.vin.c_str());assert(lv_obj_get_width(vin)==280&&lv_obj_get_height(vin)<=28);
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"连接信息")),LV_ANIM_OFF);run(ui,s,1200);save(out+"/settings-connection.ppm");
    assert(text(viewport,"BLE 已连接\nECU 已响应")&&text(viewport,"-45 dBm")&&text(viewport,"ELM327 v2.3"));
    assert(text(viewport,"运行中\nISO 15765-4 CAN 29/500\n配置 自动 / 识别 7\n当前通道 7"));
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"实际 OBD 协议")),LV_ANIM_OFF);run(ui,s,150);save(out+"/settings-protocol.ppm");
    s.connection.activeProtocol=6;run(ui,s,1200);
    assert(text(viewport,"运行中\nISO 15765-4 CAN 29/500\n配置 自动 / 识别 7\n当前通道 6"));
    s.connection.activeProtocol=7;
    // Dynamic device information is throttled to 1 Hz and paused during list scrolling.
    auto* signal=text(viewport,"-45 dBm");s.connection.rssi=-70;run(ui,s,100);assert(!strcmp(lv_label_get_text(signal),"-45 dBm"));
    lv_obj_scroll_to_y(viewport,lv_obj_get_scroll_y(viewport)+150,LV_ANIM_ON);assert(ui.scrolling());
    run(ui,s,50);assert(!strcmp(lv_label_get_text(signal),"-45 dBm"));run(ui,s,1000);assert(!strcmp(lv_label_get_text(signal),"-70 dBm"));
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"系统信息")),LV_ANIM_OFF);run(ui,s,150);save(out+"/settings-system.ppm");
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"辅助说明")),LV_ANIM_OFF);run(ui,s,150);save(out+"/settings-help.ppm");
    s.mode=Mode::Demo;s.demo=true;s.connection=ConnectionInfo{};run(ui,s,1200);
    assert(text(viewport,"演示模式\n实车连接已停止")&&!text(viewport,"AA:BB:CC:DD:EE:FF")&&!text(viewport,"-70 dBm"));
    assert(text(viewport,"3.92 V / 未充电"));
    s.battery=BatteryInfo{};run(ui,s,1100);assert(text(viewport,"状态未知 / 电压无效"));
    auto* reconnect=lv_obj_get_parent(text(viewport,"重新连接"));lv_obj_send_event(reconnect,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::Retry);
    auto* profile=object(viewport,&lv_dropdown_class);assert(profile&&lv_dropdown_get_option_count(profile)==2);assert(!strcmp(lv_dropdown_get_options(profile),"通用 OBD\n奔驰 C260 2021"));
    lv_dropdown_set_selected(profile,1);lv_obj_send_event(profile,LV_EVENT_VALUE_CHANGED,nullptr);
    assert(ui.profile()==VehicleProfile::Mercedes&&ui.takeAction()==Action::Profile);selectProfile(s,ui.profile());
    lv_dropdown_open(profile);run(ui,s,50);auto* options=lv_dropdown_get_list(profile);lv_area_t menu;lv_obj_get_coords(options,&menu);
    assert(menu.x1>=83&&menu.x2<=383&&menu.y1>=86&&menu.y2<=396&&ui.scrolling());lv_dropdown_close(profile);
    auto* protocol=lv_obj_get_child(viewport,lv_obj_get_index(profile)+2);assert(lv_obj_check_type(protocol,&lv_dropdown_class));
    lv_dropdown_set_selected(profile,0);lv_obj_send_event(profile,LV_EVENT_VALUE_CHANGED,nullptr);assert(ui.profile()==VehicleProfile::Generic&&ui.takeAction()==Action::Profile);selectProfile(s,ui.profile());assert(s.requestedProtocol==0);
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"当前车型配置")),LV_ANIM_OFF);run(ui,s,1200);assert(lv_dropdown_get_selected(protocol)==0);save(out+"/settings-profile.ppm");
    lv_dropdown_open(profile);run(ui,s,50);save(out+"/settings-profile-options.ppm");lv_dropdown_close(profile);
    lv_dropdown_set_selected(protocol,9);lv_obj_send_event(protocol,LV_EVENT_VALUE_CHANGED,nullptr);assert(ui.protocol()==9&&ui.takeAction()==Action::Protocol);s.requestedProtocol=9;
    auto* identity=lv_obj_get_parent(text(viewport,"重新读取车辆信息"));lv_obj_send_event(identity,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::Identity);
    s.identity.ecu=std::string(80,'W');s.identity.detail=std::string(120,'W');s.demo=false;run(ui,s,1200);
    assert(lv_obj_get_height(text(viewport,s.identity.ecu.c_str()))>28);
    auto assertFlow=[&](){lv_obj_update_layout(viewport);for(uint32_t i=1;i<lv_obj_get_child_count(viewport);++i){auto* previous=lv_obj_get_child(viewport,i-1);auto* next=lv_obj_get_child(viewport,i);assert(lv_obj_get_y(previous)+lv_obj_get_height(previous)<=lv_obj_get_y(next));}};
    s.identity.ecu="ECU 7E8 / ENGINE-CONTROL\nECU 7E9 / TRANSMISSION-CONTROL\nECU 7EA / BRAKE-CONTROL";
    s.identity.model=std::string(100,'M');s.identity.detail=std::string(200,'S');s.bluetooth.saved={"AA:BB:CC:DD:EE:FF",std::string(64,'B'),1};
    run(ui,s,1200);assertFlow();auto* ecu=text(viewport,s.identity.ecu.c_str());assert(ecu&&lv_obj_get_height(ecu)>60);
    auto* following=text(viewport,"信息来源与状态");assert(lv_obj_get_y(following)>lv_obj_get_y(ecu)+lv_obj_get_height(ecu));
    s.identity.detail="ECU 7E8 / 读取成功\nECU 7E9 / 读取成功\nECU 7EA / 读取成功";run(ui,s,1200);assertFlow();
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"ECU 名称")),LV_ANIM_OFF);run(ui,s,150);save(out+"/settings-long-ecu.ppm");
    s.identity.ecu="ECU 7E8";s.identity.model="未识别";s.identity.detail="读取成功";run(ui,s,1200);assertFlow();
    assert(lv_obj_get_y(following)>lv_obj_get_y(text(viewport,"ECU 7E8"))+lv_obj_get_height(text(viewport,"ECU 7E8")));
    lv_obj_scroll_to_y(viewport,lv_obj_get_y(text(viewport,"已保存蓝牙设备")),LV_ANIM_OFF);run(ui,s,150);save(out+"/settings-bluetooth.ppm");
    auto* select=lv_obj_get_parent(text(viewport,"选择蓝牙设备"));lv_obj_send_event(select,LV_EVENT_CLICKED,nullptr);assert(ui.takeAction()==Action::Bluetooth);
    ui.showPage(PageId::Count,false);assert(ui.page()==PageId::Settings);
    ui.showPage(PageId::Overview,false);run(ui,s,100);drag(ui,s,120,300,390,300);assert(ui.page()==PageId::Overview);
    s.mode=Mode::Loading;s.status="正在扫描蓝牙";attemptAt=now;run(ui,s,300);assert(lv_obj_has_flag(retry,LV_OBJ_FLAG_HIDDEN));
    attemptAt=now-ConnectionTimeoutMs;run(ui,s,50);assert(!lv_obj_has_flag(retry,LV_OBJ_FLAG_HIDDEN));
    std::cout<<"PASS: timed logo fade/size/black background, immediate connection transition, boot/timeout, three tiles, real pointer swipe/scroll/slider gestures, scroll retention, info throttling, scroll freeze/catch-up, live/demo metadata, battery validity, controls/retry\n";
}
