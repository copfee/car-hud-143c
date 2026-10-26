#include "display_policy.hpp"

// Compile-time checks keep the display classifier usable without Arduino/LVGL
// and catch accidental expansion of the 60 Hz path.
static_assert(hud::numericOnly({30,122,435,230},0));
static_assert(!hud::numericOnly({30,122,435,230},1));
static_assert(hud::numericOnly({60,335,405,375},1));
static_assert(!hud::numericOnly({60,335,405,375},-1));
static_assert(!hud::numericOnly({26,26,439,439},0));
static_assert(!hud::numericOnly({113,166,140,274},1));

