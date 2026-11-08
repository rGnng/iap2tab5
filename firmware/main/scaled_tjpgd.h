#pragma once

/* Private, complete scaling-enabled TJPGD. Stock LVGL keeps its own decoder. */
#define jd_prepare tab5_jd_prepare
#define jd_decomp tab5_jd_decomp
#include "../../third_party/tjpgd/tjpgd.h"
#undef jd_prepare
#undef jd_decomp
