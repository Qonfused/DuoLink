/** @file
  Copyright (c) 2026, Cory Bennett. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#pragma once

class IOService;

namespace duolink {
void registerPenWireProbe();
void publishPenWireProbe(IOService *service);
} // namespace duolink
