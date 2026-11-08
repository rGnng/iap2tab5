#ifndef IAP2TAB5_SCREEN_BRIGHTNESS_H
#define IAP2TAB5_SCREEN_BRIGHTNESS_H

/* A configured 0% can look like an unresponsive screen: bounce it to Low
 * (5%). This guards brightness settings only. Screen Off still sends zero
 * directly to the backlight driver. Keep custom positive levels available. */
static inline int screen_brightness_normalize(int percent)
{
    if(percent <= 0) return 5;
    return percent > 100 ? 100 : percent;
}

#endif
