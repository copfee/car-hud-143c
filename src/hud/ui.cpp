#include "ui.hpp"
#include "version.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
LV_FONT_DECLARE(hud_cn_18);
LV_FONT_DECLARE(hud_speed_96);
LV_IMAGE_DECLARE(hud_mercedes_logo);
namespace hud {
static constexpr uint32_t White=0xEFF3F7,Muted=0x81909E,Cyan=0x12CBE8,Amber=0xFFBD6B,Track=0x283440;
static void setText(lv_obj_t* o,const char* value){if(std::strcmp(lv_label_get_text(o),value)!=0)lv_label_set_text(o,value);}
static lv_obj_t* label(lv_obj_t* p,const char* text,int x,int y,int w,const lv_font_t* f,uint32_t color=White) {
    auto* o=lv_label_create(p); lv_label_set_text(o,text);lv_obj_set_pos(o,x,y);lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,f,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_CLICKABLE);return o;
}
static void plain(lv_obj_t* o) {
    lv_obj_remove_style_all(o);lv_obj_set_style_bg_color(o,lv_color_black(),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_pad_all(o,0,0);lv_obj_set_size(o,466,466);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t* button(lv_obj_t* p,const char* text,int x,int y,int w,uint32_t color) {
    auto* b=lv_button_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,46);lv_obj_set_style_radius(b,23,0);
    lv_obj_set_style_bg_color(b,lv_color_hex(color),0);lv_obj_set_style_shadow_width(b,0,0);
    auto* t=label(b,text,0,0,w,&hud_cn_18,0xFFFFFF);lv_obj_center(t);return b;
}
static void rule(lv_obj_t* p,int x,int y,int w) {
    auto* o=lv_obj_create(p);lv_obj_remove_style_all(o);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,1);
    lv_obj_set_style_bg_color(o,lv_color_hex(Track),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
}
static lv_obj_t* bar(lv_obj_t* p,int x,int y,int w,int h,uint32_t color) {
    auto* o=lv_bar_create(p);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_bar_set_range(o,0,1000);
    lv_obj_set_style_bg_color(o,lv_color_hex(Track),LV_PART_MAIN);lv_obj_set_style_bg_opa(o,255,LV_PART_MAIN);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),LV_PART_INDICATOR);lv_obj_set_style_radius(o,4,LV_PART_MAIN);
    lv_obj_set_style_radius(o,4,LV_PART_INDICATOR);return o;
}
static void fmt(char* b,size_t n,Metric m,const Sample& s) {
    if(!s.valid){snprintf(b,n,"--");return;}
    if(m==Odometer) { auto v=roundedKm(s.raw);std::string d=std::to_string(v);for(int i=int(d.size())-3;i>0;i-=3)d.insert(i,",");snprintf(b,n,"%s km",d.c_str()); }
    else if(m==Coolant)snprintf(b,n,"%.0f°C",s.shown);
    else if(m==Fuel||m==Pedal||m==Throttle)snprintf(b,n,"%.1f%%",s.shown);
    else if(m==Pressure)snprintf(b,n,"%.2f bar",s.shown);
    else if(m==Rpm)snprintf(b,n,"%.0f rpm",s.shown);
    else snprintf(b,n,"%.0f",s.shown);
}
void Interface::clicked(lv_event_t* e) {
    auto* self=static_cast<Interface*>(lv_event_get_user_data(e));auto* target=lv_event_get_target_obj(e);
    if(target==self->retry_)self->action_=Action::Retry;
    else if(target==self->demoButton_)self->action_=Action::Demo;
    else if(target==self->bootBluetooth_)self->action_=Action::Bluetooth;
    else if(target==self->scanButton_)self->action_=Action::ScanBluetooth;
    else if(target==self->cancelBluetooth_)self->action_=Action::CancelBluetooth;
}
void Interface::control(lv_event_t* e) {
    auto* self=static_cast<Interface*>(lv_event_get_user_data(e));
    self->action_=lv_event_get_target_obj(e)==self->slider_?Action::Brightness:Action::Mirror;
    if(self->action_==Action::Brightness){char b[24];snprintf(b,sizeof(b),"%d%%",self->brightness());setText(self->brightnessValue_,b);}
}
void Interface::scrollEvent(lv_event_t* e){
    auto* self=static_cast<Interface*>(lv_event_get_user_data(e));
    auto* target=lv_event_get_target_obj(e);
    bool started=lv_event_get_code(e)==LV_EVENT_SCROLL_BEGIN;
    if(target==self->tiles_)self->scrolling_=started;
    else if(target==self->settingsScroll_)self->settingsScrolling_=started;
    else return;
    if(!started){self->lastNumbers_=0;self->lastDynamics_=0;self->lastInfo_=0;}
}
void Interface::create() {
    splash_=lv_obj_create(nullptr);plain(splash_);
    logo_=lv_image_create(splash_);lv_image_set_src(logo_,&hud_mercedes_logo);lv_obj_center(logo_);
    lv_obj_set_style_image_opa(logo_,LV_OPA_TRANSP,0);
    lv_obj_remove_flag(logo_,lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    boot_=lv_obj_create(nullptr);plain(boot_);
    label(boot_,"车辆连接",100,48,266,&hud_cn_18,Muted);
    spinner_=lv_spinner_create(boot_);lv_obj_set_size(spinner_,98,98);lv_obj_set_pos(spinner_,184,102);
    lv_spinner_set_anim_params(spinner_,1200,220);
    lv_obj_set_style_arc_color(spinner_,lv_color_hex(Track),LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner_,lv_color_hex(Cyan),LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner_,5,LV_PART_MAIN);lv_obj_set_style_arc_width(spinner_,5,LV_PART_INDICATOR);
    status_=label(boot_,"正在扫描蓝牙",68,216,330,&hud_cn_18);
    bootPeer_=label(boot_,"请选择蓝牙设备",93,270,280,&hud_cn_18,Muted);lv_obj_set_height(bootPeer_,24);lv_label_set_long_mode(bootPeer_,LV_LABEL_LONG_DOT);
    bootBluetooth_=button(boot_,"选择蓝牙设备",140,395,186,0x163C47);lv_obj_add_event_cb(bootBluetooth_,clicked,LV_EVENT_CLICKED,this);
    countdown_=label(boot_,"等待连接",70,296,326,&hud_cn_18,Muted);
    retry_=button(boot_,"重试",92,342,130,0x163C47);demoButton_=button(boot_,"演示模式",244,342,130,0x345563);
    lv_obj_add_event_cb(retry_,clicked,LV_EVENT_CLICKED,this);lv_obj_add_event_cb(demoButton_,clicked,LV_EVENT_CLICKED,this);
    lv_obj_add_flag(retry_,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(demoButton_,LV_OBJ_FLAG_HIDDEN);
    dash_=lv_obj_create(nullptr);plain(dash_);
    tiles_=lv_tileview_create(dash_);plain(tiles_);lv_obj_add_flag(tiles_,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tiles_,LV_SCROLLBAR_MODE_OFF);
    // The physical AMOLED is circular. Software corner masking adds ARGB layers
    // for the whole viewport on every scroll frame without changing visible pixels.
    lv_obj_set_scroll_dir(tiles_,LV_DIR_HOR);lv_obj_set_style_radius(tiles_,0,0);lv_obj_set_style_clip_corner(tiles_,false,0);
    lv_obj_add_event_cb(tiles_,scrollEvent,LV_EVENT_SCROLL_BEGIN,this);
    lv_obj_add_event_cb(tiles_,scrollEvent,LV_EVENT_SCROLL_END,this);
    for(unsigned p=0;p<PageCount;++p) {
        auto dir=p==0?LV_DIR_RIGHT:p==PageCount-1?LV_DIR_LEFT:LV_DIR_HOR;
        pages_[p]=lv_tileview_add_tile(tiles_,p,0,dir);
        plain(pages_[p]);lv_obj_set_pos(pages_[p],466*p,0);
        lv_obj_add_flag(pages_[p],LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        demoBadge_[p]=label(pages_[p],"演示",180,p==0?100:p==1?66:59,106,&hud_cn_18,Amber);
        for(unsigned j=0;j<PageCount;++j) {
            dots_[p][j]=lv_obj_create(pages_[p]);lv_obj_remove_style_all(dots_[p][j]);lv_obj_set_size(dots_[p][j],10,10);
            lv_obj_set_pos(dots_[p][j],208+j*20,433);lv_obj_set_style_radius(dots_[p][j],5,0);lv_obj_set_style_bg_opa(dots_[p][j],255,0);
            lv_obj_remove_flag(dots_[p][j],lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
        }
    }
    auto* home=pages_[0];
    gauge_=lv_arc_create(home);lv_obj_set_pos(gauge_,26,26);lv_obj_set_size(gauge_,414,414);
    lv_arc_set_rotation(gauge_,0);lv_arc_set_bg_angles(gauge_,180,360);lv_arc_set_range(gauge_,0,8000);
    lv_obj_set_style_arc_width(gauge_,6,LV_PART_MAIN);lv_obj_set_style_arc_width(gauge_,6,LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(gauge_,lv_color_hex(Track),LV_PART_MAIN);lv_obj_set_style_arc_color(gauge_,lv_color_hex(Cyan),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(gauge_,0,LV_PART_KNOB);lv_obj_set_style_pad_all(gauge_,0,LV_PART_KNOB);lv_obj_remove_flag(gauge_,LV_OBJ_FLAG_CLICKABLE);
    auto* red=lv_arc_create(home);lv_obj_set_pos(red,26,26);lv_obj_set_size(red,414,414);lv_arc_set_rotation(red,0);
    lv_arc_set_bg_angles(red,326,360);lv_obj_set_style_arc_color(red,lv_color_hex(0xF45164),LV_PART_MAIN);
    lv_obj_set_style_arc_width(red,6,LV_PART_MAIN);lv_obj_set_style_arc_opa(red,0,LV_PART_INDICATOR);lv_obj_set_style_bg_opa(red,0,LV_PART_KNOB);lv_obj_remove_flag(red,LV_OBJ_FLAG_CLICKABLE);
    static lv_point_precise_t tickPoints[33][2];
    for(int i=0;i<33;++i){
        double a=(180+i*180.0/32)*3.14159265/180;
        int inner=i%8==0?192:199;
        tickPoints[i][0]={int32_t(std::lround(233+inner*std::cos(a))),int32_t(std::lround(233+inner*std::sin(a)))};
        tickPoints[i][1]={int32_t(std::lround(233+207*std::cos(a))),int32_t(std::lround(233+207*std::sin(a)))};
        auto* line=lv_line_create(home);lv_line_set_points(line,tickPoints[i],2);lv_obj_set_style_line_color(line,lv_color_hex(Muted),0);lv_obj_set_style_line_width(line,i%8==0?2:1,0);
    }
    for(int i=0;i<5;++i) {
        double a=(180+i*45)*3.14159265/180;char text[4];snprintf(text,sizeof(text),"%d",i*2);
        label(home,text,int(233+173*std::cos(a))-16,int(233+173*std::sin(a))-10,32,&lv_font_montserrat_18);
    }
    label(home,"x1000 rpm",150,76,166,&lv_font_montserrat_16,Muted);
    number_[Speed]=label(home,"--",30,122,406,&hud_speed_96);
    label(home,"km/h",160,220,146,&lv_font_montserrat_22,Muted);
    number_[Rpm]=label(home,"--",70,250,326,&lv_font_montserrat_32);
    rule(home,78,294,310);rule(home,90,365,286);
    label(home,"水温",90,305,130,&hud_cn_18,Muted);label(home,"油量",246,305,130,&hud_cn_18,Muted);
    number_[Coolant]=label(home,"--",72,328,166,&lv_font_montserrat_24,Amber);
    number_[Fuel]=label(home,"--",228,328,166,&lv_font_montserrat_24,Cyan);
    label(home,"总里程",150,374,166,&hud_cn_18,Muted);
    number_[Odometer]=label(home,"--",65,398,336,&lv_font_montserrat_22);
    auto* detail=pages_[1];label(detail,"踏板与制动",110,34,246,&hud_cn_18);
    label(detail,"油门踏板",74,93,132,&hud_cn_18,Cyan);label(detail,"节气门",260,93,132,&hud_cn_18,Cyan);
    number_[Pedal]=label(detail,"--",58,119,164,&lv_font_montserrat_24);
    number_[Throttle]=label(detail,"--",244,119,164,&lv_font_montserrat_24);
    pedalBar_=bar(detail,113,166,28,108,Cyan);throttleBar_=bar(detail,299,166,28,108,Cyan);
    for(int i=0;i<3;++i) {
        char text[8];snprintf(text,sizeof(text),"%d",100-i*50);
        label(detail,text,146,157+i*53,44,&lv_font_montserrat_16,Muted);
        label(detail,text,332,157+i*53,44,&lv_font_montserrat_16,Muted);
    }
    rule(detail,60,292,346);label(detail,"制动主缸压力",100,307,266,&hud_cn_18,Muted);
    number_[Pressure]=label(detail,"--",60,335,346,&lv_font_montserrat_32);
    brakeBar_=bar(detail,112,383,242,5,Amber);
    context_=label(detail,"-- km/h  |  -- rpm",65,406,336,&lv_font_montserrat_16,Muted);
    createSettings(pages_[unsigned(PageId::Settings)]);createBluetooth();
    lv_screen_load(splash_);displayed_=Mode::Splash;
}
static lv_obj_t* flowBox(lv_obj_t* parent,int width,int minimum,lv_flex_flow_t flow){
    auto* box=lv_obj_create(parent);lv_obj_remove_style_all(box);lv_obj_set_width(box,width);lv_obj_set_height(box,LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(box,minimum,0);lv_obj_set_flex_flow(box,flow);lv_obj_set_flex_align(box,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);return box;
}
static lv_obj_t* flowText(lv_obj_t* parent,const char* value,int width=280,uint32_t color=White,const lv_font_t* font=&hud_cn_18){
    auto* o=label(parent,value,0,0,width,font,color);lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);
    lv_obj_set_height(o,LV_SIZE_CONTENT);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_LEFT,0);lv_obj_set_style_text_line_space(o,4,0);return o;
}
void Interface::createSettings(lv_obj_t* page){
    label(page,"设置与设备",133,30,200,&hud_cn_18);
    label(page,"上下滑动查看更多",123,405,220,&hud_cn_18,Muted);
    settingsScroll_=lv_obj_create(page);plain(settingsScroll_);
    lv_obj_set_pos(settingsScroll_,83,86);lv_obj_set_size(settingsScroll_,300,310);
    lv_obj_set_style_pad_all(settingsScroll_,10,0);lv_obj_set_style_pad_row(settingsScroll_,8,0);
    lv_obj_set_flex_flow(settingsScroll_,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settingsScroll_,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_START);
    lv_obj_add_flag(settingsScroll_,lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_MOMENTUM|LV_OBJ_FLAG_SCROLL_CHAIN_HOR));
    lv_obj_remove_flag(settingsScroll_,lv_obj_flag_t(LV_OBJ_FLAG_SCROLL_ELASTIC|LV_OBJ_FLAG_SCROLL_CHAIN_VER));
    lv_obj_set_scroll_dir(settingsScroll_,LV_DIR_VER);lv_obj_set_scrollbar_mode(settingsScroll_,LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(settingsScroll_,2,LV_PART_SCROLLBAR);lv_obj_set_style_bg_color(settingsScroll_,lv_color_hex(Muted),LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(settingsScroll_,scrollEvent,LV_EVENT_SCROLL_BEGIN,this);lv_obj_add_event_cb(settingsScroll_,scrollEvent,LV_EVENT_SCROLL_END,this);
    auto line=[&](const char* value,uint32_t color=White,const lv_font_t* font=&hud_cn_18){return flowText(settingsScroll_,value,280,color,font);};
    bool first=true;
    auto section=[&](const char* title){if(!first)rule(settingsScroll_,0,0,280);first=false;line(title,Cyan);};
    auto field=[&](const char* title){line(title,Muted);return line("--");};
    auto row=[&](int minimum){auto* o=flowBox(settingsScroll_,280,minimum,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(o,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);return o;};
    auto actionButton=[&](const char* title,Action action){
        auto* holder=row(46);lv_obj_set_flex_align(holder,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        auto* o=button(holder,title,0,0,240,0x163C47);
        lv_obj_set_user_data(o,reinterpret_cast<void*>(uintptr_t(action)));
        lv_obj_add_event_cb(o,[](lv_event_t* e){auto* self=static_cast<Interface*>(lv_event_get_user_data(e));
            self->action_=Action(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e))));
        },LV_EVENT_CLICKED,this);return o;
    };
    section("开发板电池");
    auto* batteryRow=row(36);batteryValue_=flowText(batteryRow,"--",200,White,&lv_font_montserrat_32);
    auto* iconBox=flowBox(batteryRow,44,22,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(iconBox,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    auto* icon=lv_obj_create(iconBox);lv_obj_remove_style_all(icon);lv_obj_set_size(icon,37,20);
    lv_obj_set_style_border_width(icon,2,0);lv_obj_set_style_border_color(icon,lv_color_hex(Muted),0);lv_obj_set_style_radius(icon,3,0);
    lv_obj_remove_flag(icon,lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    batteryFill_=lv_obj_create(icon);lv_obj_remove_style_all(batteryFill_);lv_obj_set_pos(batteryFill_,2,2);lv_obj_set_size(batteryFill_,1,12);
    lv_obj_set_style_bg_color(batteryFill_,lv_color_hex(Cyan),0);lv_obj_set_style_bg_opa(batteryFill_,255,0);
    lv_obj_remove_flag(batteryFill_,lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    auto* terminal=lv_obj_create(iconBox);lv_obj_remove_style_all(terminal);lv_obj_set_size(terminal,3,8);
    lv_obj_set_style_bg_color(terminal,lv_color_hex(Muted),0);lv_obj_set_style_bg_opa(terminal,255,0);lv_obj_remove_flag(terminal,lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE));
    batteryState_=line("状态未知");line("电量由电压估算 / 安装状态未知",Muted);
    section("显示设置");
    auto* brightnessRow=row(24);flowText(brightnessRow,"亮度",150,Muted);brightnessValue_=flowText(brightnessRow,"50%",90,White,&lv_font_montserrat_18);lv_obj_set_style_text_align(brightnessValue_,LV_TEXT_ALIGN_RIGHT,0);
    // Keep the slider directly under the viewport: existing gesture ownership is unchanged.
    slider_=lv_slider_create(settingsScroll_);lv_obj_set_size(slider_,256,12);lv_obj_set_style_margin_top(slider_,8,0);lv_obj_set_style_margin_bottom(slider_,8,0);lv_obj_set_style_margin_left(slider_,12,0);
    lv_slider_set_range(slider_,10,100);lv_slider_set_value(slider_,50,LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_,lv_color_hex(Track),LV_PART_MAIN);lv_obj_set_style_bg_color(slider_,lv_color_hex(Cyan),LV_PART_INDICATOR);lv_obj_set_style_bg_color(slider_,lv_color_hex(White),LV_PART_KNOB);
    lv_obj_remove_flag(slider_,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);lv_obj_set_ext_click_area(slider_,16);lv_obj_add_event_cb(slider_,control,LV_EVENT_VALUE_CHANGED,this);
    auto* mirrorRow=row(34);flowText(mirrorRow,"镜像显示",200);
    mirror_=lv_switch_create(mirrorRow);lv_obj_set_size(mirror_,56,30);lv_obj_set_style_bg_color(mirror_,lv_color_hex(Cyan),LV_PART_INDICATOR|LV_STATE_CHECKED);lv_obj_add_event_cb(mirror_,control,LV_EVENT_VALUE_CHANGED,this);
    section("车辆信息");model_=field("读取到的车型");line("VIN / 车架号",Muted);vin_=line("--",White,&lv_font_montserrat_18);
    ecuName_=field("ECU 名称");identityState_=field("信息来源与状态");
    auto selector=[&](const char* title,const char* options){
        line(title,Muted);auto* o=lv_dropdown_create(settingsScroll_);lv_obj_set_size(o,278,42);
        lv_dropdown_set_options(o,options);lv_obj_set_style_text_font(o,&hud_cn_18,0);lv_obj_set_style_text_font(o,&lv_font_montserrat_18,LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(o,lv_color_hex(Track),0);lv_obj_set_style_text_color(o,lv_color_hex(White),0);lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
        lv_obj_add_event_cb(o,[](lv_event_t* e){auto* self=static_cast<Interface*>(lv_event_get_user_data(e));auto* o=lv_event_get_target_obj(e);
            if(lv_event_get_code(e)==LV_EVENT_VALUE_CHANGED)self->action_=o==self->profileSelect_?Action::Profile:Action::Protocol;
            else if(lv_event_get_code(e)==LV_EVENT_READY){auto* list=lv_dropdown_get_list(o);lv_obj_set_style_text_font(list,&hud_cn_18,0);lv_obj_set_width(list,280);lv_obj_set_style_max_height(list,210,0);lv_obj_set_pos(list,93,128);lv_obj_remove_flag(list,LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
                lv_obj_set_style_bg_color(list,lv_color_hex(Track),0);lv_obj_set_style_text_color(list,lv_color_hex(White),0);
                lv_async_call([](void* p){auto* selector=static_cast<lv_obj_t*>(p);if(!lv_dropdown_is_open(selector))return;auto* menu=lv_dropdown_get_list(selector);lv_obj_set_width(menu,280);lv_obj_align(menu,LV_ALIGN_TOP_LEFT,93,128);},o);
            }
        },LV_EVENT_ALL,this);return o;
    };
    std::string profileOptions;for(unsigned i=0;i<ProfileCount;++i){if(i)profileOptions+='\n';profileOptions+=ProfileNames[i];}
    profileSelect_=selector("当前车型配置",profileOptions.c_str());
    protocolSelect_=selector("协议选择","自动识别\n1 / J1850 PWM\n2 / J1850 VPW\n3 / ISO 9141\n4 / KWP 慢初始化\n5 / KWP 快初始化\n6 / CAN 11位 500\n7 / CAN 29位 500\n8 / CAN 11位 250\n9 / CAN 29位 250");
    actionButton("重新读取车辆信息",Action::Identity);line("车型未识别时不会由配置或 VIN 猜测。换车请手动选择通用模式。",Muted);
    section("连接信息");
    savedPeer_=field("已保存蓝牙设备");peerState_=field("蓝牙选择状态");actionButton("选择蓝牙设备",Action::Bluetooth);
    connectionState_=line("未连接");address_=field("BLE 地址");signal_=field("信号强度");elm_=field("适配器版本");
    protocol_=field("实际 OBD 协议");availability_=field("数据能力");lastReply_=field("最近有效 ECU 响应");actionButton("重新连接",Action::Retry);
    line("BLE 服务 UUID",Muted);line("000018f0-0000-1000\n8000-00805f9b34fb",White,&lv_font_montserrat_16);
    line("接收 UUID / Notify",Muted);line("00002af0-0000-1000\n8000-00805f9b34fb",White,&lv_font_montserrat_16);
    line("发送 UUID / Write",Muted);line("00002af1-0000-1000\n8000-00805f9b34fb",White,&lv_font_montserrat_16);
    section("系统信息");line("固件版本",Muted);line(FirmwareVersion);line("开发板",Muted);line("ESP32-S3-Touch\nAMOLED-1.43C");
    chip_=field("芯片");capacity_=field("实际存储容量");heap_=field("可用堆内存");uptime_=field("运行时长");
    section("辅助说明");
    line("左右滑动切换三屏；本页上下滚动查看。拖动亮度滑块可调节亮度，镜像用于反射显示。");
    line("首次开机扫描并选择蓝牙设备，验证 OBD 通道后保存。下次开机自动连接，可在本页更换。");
    line("油门踏板表示驾驶员输入，节气门表示阀门位置，两者不同。制动主缸压力单位为 bar。");
    line("总里程按公里四舍五入。数据不可用或过期时显示 --。");
    line("检测60秒后可重试或进入演示，后台按退避间隔自动恢复。演示车辆数据为模拟值，电池仍为实测。");
    line("电量为单节锂电池电压估算，充电和负载会影响结果。硬件无法可靠识别电池是否安装；未充电不代表已充满。",Muted);
}
void Interface::createBluetooth(){
    bluetoothScreen_=lv_obj_create(nullptr);plain(bluetoothScreen_);label(bluetoothScreen_,"选择蓝牙设备",123,32,220,&hud_cn_18);
    bluetoothStatus_=label(bluetoothScreen_,"正在扫描附近设备",83,78,300,&hud_cn_18,Muted);lv_label_set_long_mode(bluetoothStatus_,LV_LABEL_LONG_WRAP);
    bluetoothList_=lv_obj_create(bluetoothScreen_);plain(bluetoothList_);lv_obj_set_pos(bluetoothList_,83,137);lv_obj_set_size(bluetoothList_,300,218);
    lv_obj_set_style_pad_all(bluetoothList_,4,0);lv_obj_set_style_pad_row(bluetoothList_,8,0);lv_obj_set_flex_flow(bluetoothList_,LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(bluetoothList_,lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_MOMENTUM));lv_obj_set_scroll_dir(bluetoothList_,LV_DIR_VER);lv_obj_set_scrollbar_mode(bluetoothList_,LV_SCROLLBAR_MODE_ACTIVE);
    scanButton_=button(bluetoothScreen_,"重新扫描",90,370,132,0x163C47);cancelBluetooth_=button(bluetoothScreen_,"返回",244,370,132,0x345563);
    lv_obj_add_event_cb(scanButton_,clicked,LV_EVENT_CLICKED,this);lv_obj_add_event_cb(cancelBluetooth_,clicked,LV_EVENT_CLICKED,this);
}
void Interface::refreshBluetooth(const Snapshot& s){
    setText(bluetoothStatus_,s.bluetooth.detail.c_str());
    if(s.bluetooth.scanning)lv_obj_add_state(scanButton_,LV_STATE_DISABLED);else lv_obj_remove_state(scanButton_,LV_STATE_DISABLED);
    if(bluetoothRevision_==s.bluetooth.revision||lv_obj_is_scrolling(bluetoothList_))return;
    auto* input=lv_indev_active();if(input&&lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED)return;
    bluetoothRevision_=s.bluetooth.revision;renderedPeers_=s.bluetooth.devices;lv_obj_clean(bluetoothList_);
    for(size_t i=0;i<renderedPeers_.size();++i){
        const auto& peer=renderedPeers_[i];auto* card=lv_button_create(bluetoothList_);lv_obj_set_width(card,292);lv_obj_set_height(card,LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card,10,0);lv_obj_set_style_pad_row(card,5,0);lv_obj_set_style_bg_color(card,lv_color_hex(Track),0);lv_obj_set_style_shadow_width(card,0,0);lv_obj_set_flex_flow(card,LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);flowText(card,peer.name.c_str(),272);
        char detail[100];snprintf(detail,sizeof(detail),"%s\n%d dBm / %s",peer.address.c_str(),peer.rssi,peer.obdService?"发现 OBD 服务":"通道待验证");flowText(card,detail,272,Muted);
        for(uint32_t j=0;j<lv_obj_get_child_count(card);++j){auto* child=lv_obj_get_child(card,j);lv_obj_add_flag(child,LV_OBJ_FLAG_EVENT_BUBBLE);}
        lv_obj_set_user_data(card,reinterpret_cast<void*>(i));
        lv_obj_add_event_cb(card,[](lv_event_t* e){auto* self=static_cast<Interface*>(lv_event_get_user_data(e));auto* card=lv_event_get_current_target_obj(e);auto index=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(card));
            if(index<self->renderedPeers_.size()){self->chosenPeer_=self->renderedPeers_[index];self->action_=Action::SelectBluetooth;}
        },LV_EVENT_CLICKED,this);
    }
    if(renderedPeers_.empty())flowText(bluetoothList_,s.bluetooth.scanning?"正在查找设备…":"未发现设备，请确认适配器供电后重新扫描。",280,Muted);
}
void Interface::refreshSettings(const Snapshot& s,uint32_t now){
    char b[640];
    setText(model_,s.identity.model.empty()?"未识别":s.identity.model.c_str());
    setText(vin_,s.demo?"--":s.identity.vin.empty()?"--":s.identity.vin.c_str());
    auto* vinFont=s.identity.conflict?&hud_cn_18:&lv_font_montserrat_18;
    if(lv_obj_get_style_text_font(vin_,LV_PART_MAIN)!=vinFont)lv_obj_set_style_text_font(vin_,vinFont,0);
    setText(ecuName_,s.identity.ecu.empty()?"--":s.identity.ecu.c_str());
    setText(identityState_,s.demo?"演示模式 / 无实车身份":s.identity.detail.c_str());
    std::string saved=validPeer(s.bluetooth.saved)?s.bluetooth.saved.name+"\n"+s.bluetooth.saved.address:"未选择";
    setText(savedPeer_,saved.c_str());
    std::string peerState=s.connection.ble?s.bluetooth.detail:validPeer(s.bluetooth.target)&&!samePeer(s.bluetooth.target,s.bluetooth.saved)?"所选设备待验证 / 成功后保存":validPeer(s.bluetooth.saved)?"已保存 / 下次开机自动连接":"请选择蓝牙设备";
    setText(peerState_,peerState.c_str());
    if(!lv_dropdown_is_open(profileSelect_))lv_dropdown_set_selected(profileSelect_,unsigned(s.profile));
    if(!lv_dropdown_is_open(protocolSelect_))lv_dropdown_set_selected(protocolSelect_,s.requestedProtocol);
    if(s.battery.valid){
        snprintf(b,sizeof(b),"~%d%%",s.battery.percent);setText(batteryValue_,b);
        snprintf(b,sizeof(b),"%.2f V / %s",s.battery.volts,s.battery.charging?"充电中":"未充电");setText(batteryState_,b);
        int fill=std::max(1,s.battery.percent*29/100);if(lv_obj_get_width(batteryFill_)!=fill)lv_obj_set_width(batteryFill_,fill);
        auto color=lv_color_hex(s.battery.percent<=15?Amber:Cyan);
        if(!lv_color_eq(lv_obj_get_style_bg_color(batteryFill_,LV_PART_MAIN),color))lv_obj_set_style_bg_color(batteryFill_,color,0);
    }else {setText(batteryValue_,"--");setText(batteryState_,"状态未知 / 电压无效");lv_obj_set_width(batteryFill_,1);}
    snprintf(b,sizeof(b),"%d%%",s.brightness);setText(brightnessValue_,b);
    setText(connectionState_,s.demo?"演示模式\n实车连接已停止":s.connection.ble?(s.connection.ecu?"BLE 已连接\nECU 已响应":"BLE 已连接\n等待 ECU 响应"):"实车未连接");
    setText(address_,s.connection.ble&&s.connection.address[0]?s.connection.address:"--");
    if(s.connection.ble&&s.connection.rssiValid)snprintf(b,sizeof(b),"%d dBm",s.connection.rssi);else snprintf(b,sizeof(b),"--");setText(signal_,b);
    setText(elm_,s.connection.ble&&s.connection.elm[0]?s.connection.elm:"--");
    const char* states[]={"未连接","扫描蓝牙","初始化适配器","识别协议","发现数据","运行中","等待 ECU 唤醒","后台恢复","演示","连接蓝牙"};
    snprintf(b,sizeof(b),"%s\n%s\n配置 %s / 识别 %u\n当前通道 %u",states[unsigned(s.connection.state)],s.connection.info.description,
             s.requestedProtocol?std::to_string(s.requestedProtocol).c_str():"自动",s.connection.info.detected,s.connection.activeProtocol);setText(protocol_,b);
    std::string availability;static const char* names[]={"车速","转速","水温","油量","总里程","油门踏板","节气门","制动压力"};
    static const char* statuses[]={"未知","不支持","无响应","有效","已过期"};
    for(unsigned i=0;i<Count;++i){if(i)availability+='\n';auto state=s.capabilities.status[i];if(state==MetricStatus::Valid&&!s.samples[i].valid)state=MetricStatus::Stale;availability+=std::string(names[i])+" / "+statuses[unsigned(state)];}setText(availability_,availability.c_str());
    if(!s.demo&&s.connection.ble&&s.connection.ecu)snprintf(b,sizeof(b),"%u 秒前",unsigned(std::max(0,int32_t(now-s.connection.lastReply))/1000));
    else snprintf(b,sizeof(b),"--");setText(lastReply_,b);
    setText(chip_,s.system.chip[0]?s.system.chip:"--");
    snprintf(b,sizeof(b),"Flash %.1f MB\nPSRAM %.1f MB",s.system.flashBytes/1048576.0,s.system.psramBytes/1048576.0);setText(capacity_,b);
    snprintf(b,sizeof(b),"%.1f KB",s.system.freeHeap/1024.0);setText(heap_,b);
    auto seconds=s.system.uptimeSeconds;
    snprintf(b,sizeof(b),"%llu 天 %02u:%02u:%02u",static_cast<unsigned long long>(seconds/86400),unsigned(seconds/3600%24),unsigned(seconds/60%60),unsigned(seconds%60));setText(uptime_,b);
}
void Interface::refresh(const Snapshot& s,uint32_t now,uint32_t sinceAttempt) {
    demo_=s.demo;
    if(displayed_!=s.mode) {
        auto* target=s.mode==Mode::Splash?splash_:s.mode==Mode::Bluetooth?bluetoothScreen_:s.mode==Mode::Live||s.mode==Mode::Demo?dash_:boot_;
        if(lv_screen_active()!=target){
            if(displayed_==Mode::Splash||s.mode==Mode::Splash)lv_screen_load(target);
            else lv_screen_load_anim(target,LV_SCR_LOAD_ANIM_FADE_IN,180,0,false);
        }
        displayed_=s.mode;
        lv_slider_set_value(slider_,s.brightness,LV_ANIM_OFF);
        if(s.mirror)lv_obj_add_state(mirror_,LV_STATE_CHECKED);else lv_obj_remove_state(mirror_,LV_STATE_CHECKED);
    }
    if(s.mode==Mode::Splash){
        auto opacity=splashOpacity(s.splashElapsed);
        if(lv_obj_get_style_image_opa(logo_,LV_PART_MAIN)!=opacity)lv_obj_set_style_image_opa(logo_,opacity,0);
        return;
    }
    if(s.mode==Mode::Bluetooth){refreshBluetooth(s);return;}
    if(s.mode==Mode::Loading||s.mode==Mode::Timeout) {
        setText(bootPeer_,validPeer(s.bluetooth.target)?s.bluetooth.target.name.c_str():"请选择蓝牙设备");
        setText(status_,s.status.c_str());
        bool timeout=s.mode==Mode::Timeout||sinceAttempt>=ConnectionTimeoutMs;
        if(timeout) { lv_obj_add_flag(spinner_,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(retry_,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(demoButton_,LV_OBJ_FLAG_HIDDEN);setText(countdown_,"请检查点火状态 / 后台继续检测"); }
        else { lv_obj_remove_flag(spinner_,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(retry_,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(demoButton_,LV_OBJ_FLAG_HIDDEN);char b[64];snprintf(b,sizeof(b),"等待连接 %u / %u 秒",unsigned(std::min(ConnectionTimeoutMs/1000,sinceAttempt/1000)),unsigned(ConnectionTimeoutMs/1000));setText(countdown_,b); }
        return;
    }
    // Sampling and model smoothing continue. Let the gesture own the render budget;
    // the settled page catches up with the latest snapshot instead of replaying values.
    if(scrolling())return;
    int active=int(page());bool pageChanged=active!=previousPage_;
    if(!visualStateKnown_||previousDemo_!=s.demo){
        for(unsigned p=0;p<PageCount;++p){
            if(s.demo)lv_obj_remove_flag(demoBadge_[p],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(demoBadge_[p],LV_OBJ_FLAG_HIDDEN);
        }
        previousDemo_=s.demo;visualStateKnown_=true;
    }
    if(pageChanged){
        for(unsigned p=0;p<PageCount;++p)for(unsigned j=0;j<PageCount;++j)
            lv_obj_set_style_bg_color(dots_[p][j],lv_color_hex(j==active?Cyan:Track),0);
        previousPage_=active;
    }
    if(active==int(PageId::Settings)){
        if(pageChanged||!lastInfo_||uint32_t(now-lastInfo_)>=1000){refreshSettings(s,now);lastInfo_=now;}
        return;
    }
    auto value=[&](Metric m){return s.samples[m].valid?s.samples[m].shown:0;};
    // Non-numeric dynamics use the 30 Hz full-frame path.  Numeric labels are
    // allowed to invalidate independently on every 60 Hz UI tick below.
    if(pageChanged||!lastDynamics_||uint32_t(now-lastDynamics_)>=33){
        if(active==0)lv_arc_set_value(gauge_,int(value(Rpm)));
        else {
            lv_bar_set_value(pedalBar_,int(value(Pedal)*10),LV_ANIM_OFF);lv_bar_set_value(throttleBar_,int(value(Throttle)*10),LV_ANIM_OFF);
            lv_bar_set_value(brakeBar_,int(std::max(0.0,std::min(100.0,value(Pressure)))*10),LV_ANIM_OFF);
        }
        lastDynamics_=now;
    }
    if(!pageChanged&&uint32_t(now-lastNumbers_)<16)return;lastNumbers_=now;
    char b[64];
    if(active==0){
        for(auto m:{Speed,Rpm,Coolant,Fuel,Odometer}){fmt(b,sizeof(b),m,s.samples[m]);setText(number_[m],b);}
    }else{
        for(auto m:{Pedal,Throttle,Pressure}){fmt(b,sizeof(b),m,s.samples[m]);setText(number_[m],b);}
        char speed[24],rpm[32];fmt(speed,sizeof(speed),Speed,s.samples[Speed]);fmt(rpm,sizeof(rpm),Rpm,s.samples[Rpm]);
        snprintf(b,sizeof(b),"%s km/h  |  %s",speed,rpm);setText(context_,b);
    }
}
Action Interface::takeAction(){auto a=action_;action_=Action::None;return a;}
PageId Interface::page() const{
    auto* active=lv_tileview_get_tile_active(tiles_);
    for(unsigned p=0;p<PageCount;++p)if(active==pages_[p])return PageId(p);
    return PageId::Overview;
}
void Interface::showPage(PageId p,bool anim){if(unsigned(p)<PageCount)lv_tileview_set_tile_by_index(tiles_,unsigned(p),0,anim?LV_ANIM_ON:LV_ANIM_OFF);}
int Interface::brightness() const{return lv_slider_get_value(slider_);}
VehicleProfile Interface::profile()const{return normalizeProfile(lv_dropdown_get_selected(profileSelect_));}
uint8_t Interface::protocol()const{return uint8_t(lv_dropdown_get_selected(protocolSelect_));}
bool Interface::mirror() const{return lv_obj_has_state(mirror_,LV_STATE_CHECKED);}
bool Interface::scrolling() const{
    // An offscreen list may still be coasting; it must not freeze the visible gauges.
    return lv_dropdown_is_open(profileSelect_)||lv_dropdown_is_open(protocolSelect_)||scrolling_||lv_obj_is_scrolling(tiles_)||
        (page()==PageId::Settings&&(settingsScrolling_||lv_obj_is_scrolling(settingsScroll_)));
}
}
