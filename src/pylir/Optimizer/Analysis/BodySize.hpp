// Copyright 2022 Markus Böck
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <mlir/IR/Operation.h>

namespace pylir
{
class BodySize
{
    std::size_t m_size;

public:
    explicit BodySize(mlir::Operation* operation);

    std::size_t getSize() const
    {
        return m_size;
    }
};
} // namespace pylir
