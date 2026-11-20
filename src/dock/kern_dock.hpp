/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once
#include <mach/mach_types.h>
#include <sys/vnode.h>
class KernelPatcher;
namespace duolink {
void registerDockPatch(KernelPatcher &, bool validationHookReady);
void patchValidatedDockPage(vnode_t, const void *data);
} // namespace duolink
