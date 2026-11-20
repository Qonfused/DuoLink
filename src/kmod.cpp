/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include <mach/kmod.h>

extern "C" {
kern_return_t _start(kmod_info_t *, void *);
kern_return_t _stop(kmod_info_t *, void *);
kern_return_t DuoLink_kern_start(kmod_info_t *, void *);
// The native vtable retains our dispatch address until the next boot.
static kern_return_t refuseUnload(kmod_info_t *, void *) {
    return KERN_FAILURE;
}

KMOD_EXPLICIT_DECL(com.qonfused.DuoLink, KEXT_VERSION, _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain =
    DuoLink_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain =
    refuseUnload;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
