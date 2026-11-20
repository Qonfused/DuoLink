/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

class KernelPatcher;

namespace duolink {

void installDisplayAndDockHooks(void *, KernelPatcher &);

} // namespace duolink
