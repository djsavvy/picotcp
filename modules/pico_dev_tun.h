/*********************************************************************
   PicoTCP. Copyright (c) 2012-2015 Altran Intelligent Systems. Some rights reserved.
   See LICENSE and COPYING for usage.

 *********************************************************************/
#ifndef INCLUDE_PICO_TUN
#define INCLUDE_PICO_TUN
#include "pico_config.h"
#include "pico_device.h"

void pico_tun_destroy(struct pico_device *tun);
struct pico_device *pico_tun_create(char *name);
int pico_tun_WFI(struct pico_device *dev, int timeout_ms);


#endif

