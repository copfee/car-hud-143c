#include "display_policy.hpp"
#include <cassert>
#include <iostream>

using namespace hud;

int main(){
    assert(numericOnly({30,122,435,230},0));
    assert(!numericOnly({30,122,435,230},1));
    assert(numericOnly({60,335,405,375},1));
    assert(!numericOnly({60,335,405,375},-1));
    assert(!numericOnly({26,26,439,439},0));
    assert(!numericOnly({113,166,140,274},1));

    DirtyRegions dirty;
    dirty.add({30,122,200,150},0);dirty.add({199,122,435,150},0);
    assert(!dirty.full()&&dirty.count()==1);
    dirty.add({113,166,140,274},0);
    assert(dirty.full()&&dirty.count()==0);
    dirty.clear();dirty.add({60,335,405,375},1);
    assert(!dirty.full()&&dirty.count()==1);
    std::cout<<"PASS: numeric-only regions merge; non-numeric invalidation promotes to full frame\n";
}
