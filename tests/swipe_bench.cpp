// Measures host CPU rendering only, not panel FPS or ESP32 performance.
#include "ui.hpp"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace hud;
static uint64_t pixels=0,flushes=0;
static void flush(lv_display_t* d,const lv_area_t* a,uint8_t*){
    pixels+=lv_area_get_width(a)*lv_area_get_height(a);++flushes;lv_display_flush_ready(d);
}
int main(int argc,char** argv){
    bool allPages=argc>1&&std::strcmp(argv[1],"--all-pages")==0;
    lv_init();auto* d=lv_display_create(466,466);static uint16_t buf[466*32];
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(d,buf,nullptr,sizeof(buf),LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    Interface ui;ui.create();Snapshot s;s.mode=Mode::Demo;s.demo=true;DemoGenerator demo(2020);uint32_t now=0;
    for(int i=0;i<20;++i){now+=33;lv_tick_inc(33);demo.tick(s,now,.033);animate(s,now,33);ui.refresh(s,now,now);lv_timer_handler();}
    pixels=flushes=0;std::vector<double> frameUs;
    for(int pass=0;pass<24;++pass){
        static constexpr PageId route[]={PageId::Pedals,PageId::Settings,PageId::Pedals,PageId::Overview};
        ui.showPage(allPages?route[pass%4]:PageId((pass+1)%2),true);
        for(int frame=0;frame<16;++frame){
            now+=33;lv_tick_inc(33);demo.tick(s,now,.033);animate(s,now,33);
            auto t=std::chrono::steady_clock::now();uint64_t before=flushes;
            ui.refresh(s,now,now);lv_timer_handler();
            if(flushes!=before)frameUs.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t).count());
        }
    }
    std::sort(frameUs.begin(),frameUs.end());double sum=0;for(auto v:frameUs)sum+=v;
    printf("{\"host_render_frames\":%zu,\"mean_cpu_us\":%.1f,\"p95_cpu_us\":%.1f,\"flush_calls\":%llu,\"pixels\":%llu}\n",
        frameUs.size(),sum/frameUs.size(),frameUs[size_t((frameUs.size()-1)*.95)],(unsigned long long)flushes,(unsigned long long)pixels);
}
