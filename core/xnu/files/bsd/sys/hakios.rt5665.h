#ifndef _SYS_HAKIOS_RT5665_H_
#define _SYS_HAKIOS_RT5665_H_

#include <stdbool.h>

#ifdef KERNEL
/* After UAIF0's clocks run: MAINMIC through AIF1. False: leave it as set at boot. */
void hakios_rt5665_capture(bool on);
#endif

#endif
