// Copyright 2022 Markus Böck
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <pylir/Diagnostics/DiagnosticsBuilder.hpp>
#include <pylir/Diagnostics/Document.hpp>
#include <pylir/Support/LazyCacheIterator.hpp>
#include <pylir/Support/Macros.hpp>
#include <pylir/Support/Text.hpp>

#include <cstdint>
#include <optional>
#include <stack>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "Token.hpp"

namespace pylir
{
class Lexer
{
    int m_fileId;
    std::vector<Token> m_tokens;
    const Diag::Document* m_document;
    Diag::Document::const_iterator m_current;
    std::function<void(Diag::DiagnosticsBuilder&& diagnosticsBuilder)> m_warningCallback;
    std::size_t m_depth = 0;
    std::stack<std::pair<std::size_t, std::size_t>> m_indentation{{{0, static_cast<std::size_t>(-1)}}};

    bool parseNext();

    void parseIdentifier();

    tl::expected<std::string, std::string> parseLiteral(bool raw, bool bytes);

    void parseNumber();

    void parseIndent();

public:
    using value_type = Token;
    using reference = const Token&;
    using const_reference = reference;
    using iterator = LazyCacheIterator<value_type, Lexer, &Lexer::parseNext, &Lexer::m_tokens>;
    using const_iterator = iterator;
    using difference_type = iterator::difference_type;
    using size_type = std::size_t;

    explicit Lexer(
        const Diag::Document& document, int fileId = 0,
        std::function<void(Diag::DiagnosticsBuilder&& diagnosticsBuilder)> warningCallback = [](auto&&) {});

    ~Lexer() = default;

    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    Lexer(Lexer&&) noexcept = default;
    Lexer& operator=(Lexer&&) noexcept = default;

    [[nodiscard]] iterator begin()
    {
        return {*this, 0};
    }

    [[nodiscard]] const_iterator cbegin()
    {
        return begin();
    }

    [[nodiscard]] iterator end()
    {
        return {*this, static_cast<std::size_t>(-1)};
    }

    [[nodiscard]] const_iterator cend()
    {
        return end();
    }

    template <class T, class S, class... Args>
    [[nodiscard]] Diag::DiagnosticsBuilder createDiagnosticsBuilder(const T& location, const S& message,
                                                                    Args&&... args) const
    {
        return Diag::DiagnosticsBuilder(*m_document, location, message, std::forward<Args>(args)...);
    }
};
} // namespace pylir
