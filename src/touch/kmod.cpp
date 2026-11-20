/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "duolink_touch.hpp"
#include <mach/kmod.h>
extern "C" {
kern_return_t _start(kmod_info_t *, void *);
kern_return_t _stop(kmod_info_t *, void *);
static kern_return_t touchStart(kmod_info_t *, void *) {
    return DuoLinkTouch::registerInputCallbacks() ? KERN_SUCCESS : KERN_FAILURE;
}
// Installed hooks can still call the companion's callback table until reboot.
static kern_return_t touchStop(kmod_info_t *, void *) {
    return KERN_FAILURE;
}
KMOD_EXPLICIT_DECL(com.qonfused.DuoLinkTouch, KEXT_VERSION, _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = touchStart;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = touchStop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
