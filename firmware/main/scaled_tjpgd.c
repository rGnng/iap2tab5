/* LVGL's bundled decoder removes the scaling implementation. Build the
 * complete upstream decoder separately, without editing managed files. */
#include "scaled_tjpgd.h"
#define jd_prepare tab5_jd_prepare
#define jd_decomp tab5_jd_decomp
#include "../../third_party/tjpgd/tjpgd.c"
