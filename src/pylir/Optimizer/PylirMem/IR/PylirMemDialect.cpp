// Copyright 2022 Markus Böck
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "PylirMemDialect.hpp"

#include <mlir/IR/Builders.h>
#include <mlir/IR/DialectImplementation.h>

#include <llvm/ADT/TypeSwitch.h>

#include <pylir/Optimizer/PylirPy/IR/PylirPyDialect.hpp>
#include <pylir/Support/Functional.hpp>

#include "PylirMemOps.hpp"

#define GET_TYPEDEF_CLASSES
#include "pylir/Optimizer/PylirMem/IR/PylirMemOpsTypes.cpp.inc"

#define GET_ATTRDEF_CLASSES
#include "pylir/Optimizer/PylirMem/IR/PylirMemOpsAttributes.cpp.inc"

void pylir::Mem::PylirMemDialect::initialize()
{
    addOperations<
#define GET_OP_LIST
#include "pylir/Optimizer/PylirMem/IR/PylirMemOps.cpp.inc"
        >();
    addTypes<
#define GET_TYPEDEF_LIST
#include "pylir/Optimizer/PylirMem/IR/PylirMemOpsTypes.cpp.inc"
        >();
    addAttributes<
#define GET_ATTRDEF_LIST
#include "pylir/Optimizer/PylirMem/IR/PylirMemOpsAttributes.cpp.inc"
        >();
}

#include <pylir/Optimizer/PylirMem/IR/PylirMemOpsDialect.cpp.inc>
