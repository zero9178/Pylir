// Copyright 2022 Markus Böck
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <mlir/IR/OpImplementation.h>
#include <mlir/Interfaces/InferTypeOpInterface.h>
#include <mlir/Interfaces/SideEffectInterfaces.h>

#include "TestDialect.h.inc"

#define GET_ATTRDEF_CLASSES
#include "TestDialectAttributes.h.inc"

#define GET_TYPEDEF_CLASSES
#include "TestDialectTypes.h.inc"

#define GET_OP_CLASSES
#include "TestDialectOps.h.inc"
