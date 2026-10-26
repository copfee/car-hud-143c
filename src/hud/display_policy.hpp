#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hud {

struct DisplayRect {
    int16_t x1=0,y1=0,x2=-1,y2=-1;
};

inline constexpr DisplayRect FullDisplay{0,0,465,465};

// Dashboard labels that are allowed to use the 60 Hz path.  Padding covers
// glyph antialiasing and the old/new label bounds emitted by LVGL.
inline constexpr DisplayRect OverviewNumericRegions[] = {
    {24,112,441,235}, // speed
    {64,244,401,293}, // rpm
    {66,322,243,365}, // coolant
    {222,322,399,365},// fuel
    {58,392,407,431}, // odometer
};
inline constexpr DisplayRect DetailNumericRegions[] = {
    {52,113,227,156}, // pedal
    {238,113,413,156},// throttle
    {54,329,411,380}, // pressure
    {59,400,407,431}, // detail-page speed/rpm context
};

inline constexpr bool valid(const DisplayRect& r){return r.x1<=r.x2&&r.y1<=r.y2;}
inline constexpr bool contains(const DisplayRect& outer,const DisplayRect& inner){
    return inner.x1>=outer.x1&&inner.y1>=outer.y1&&inner.x2<=outer.x2&&inner.y2<=outer.y2;
}
template<size_t N>inline constexpr bool inRegions(const DisplayRect& r,const DisplayRect (&regions)[N]){
    for(const auto& region:regions)if(contains(region,r))return true;
    return false;
}
inline constexpr bool numericOnly(const DisplayRect& r,int page){
    return page==0?inRegions(r,OverviewNumericRegions):page==1?inRegions(r,DetailNumericRegions):false;
}
inline constexpr DisplayRect aligned(DisplayRect r){
    r.x1=std::max<int16_t>(0,r.x1&~1);r.y1=std::max<int16_t>(0,r.y1&~1);
    r.x2=std::min<int16_t>(465,r.x2|1);r.y2=std::min<int16_t>(465,r.y2|1);
    return r;
}
inline constexpr bool mergeable(const DisplayRect& a,const DisplayRect& b){
    return a.x1<=b.x2+2&&b.x1<=a.x2+2&&a.y1<=b.y2+2&&b.y1<=a.y2+2;
}
inline constexpr DisplayRect joined(const DisplayRect& a,const DisplayRect& b){
    return {std::min(a.x1,b.x1),std::min(a.y1,b.y1),std::max(a.x2,b.x2),std::max(a.y2,b.y2)};
}

class DirtyRegions {
    static constexpr size_t Capacity=16;
    DisplayRect areas_[Capacity]{};
    size_t count_=0;
    bool full_=false;
public:
    void clear(){count_=0;full_=false;}
    void add(DisplayRect area,int page){
        area=aligned(area);
        if(!numericOnly(area,page)){full_=true;count_=0;return;}
        if(full_)return;
        for(size_t i=0;i<count_;){
            if(mergeable(areas_[i],area)){
                area=joined(areas_[i],area);areas_[i]=areas_[--count_];i=0;
            }else ++i;
        }
        if(count_==Capacity){full_=true;count_=0;return;}
        areas_[count_++]=area;
    }
    bool full()const{return full_;}
    size_t count()const{return count_;}
    const DisplayRect& operator[](size_t i)const{return areas_[i];}
};

}
