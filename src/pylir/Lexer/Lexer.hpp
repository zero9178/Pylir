
#pragma once

#include <pylir/Diagnostics/DiagnosticsBuilder.hpp>
#include <pylir/Diagnostics/Document.hpp>
#include <pylir/Support/LazyCacheIterator.hpp>
#include <pylir/Support/Macros.hpp>
#include <pylir/Support/Text.hpp>

#include <cstdint>
#include <optional>
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
    Diag::Document* m_document;
    Diag::Document::iterator m_current;
    std::function<void(Diag::DiagnosticsBuilder&& diagnosticsBuilder)> m_warningCallback;

    bool parseNext();

    void parseIdentifier();

    tl::expected<std::string, std::string> parseLiteral(bool raw, bool bytes);

public:
    using value_type = Token;
    using reference = const Token&;
    using const_reference = reference;
    using iterator = LazyCacheIterator<value_type, Lexer, &Lexer::parseNext, &Lexer::m_tokens>;
    using const_iterator = iterator;
    using difference_type = iterator::difference_type;
    using size_type = std::size_t;

    explicit Lexer(
        Diag::Document& document, int fileId = 0,
        std::function<void(Diag::DiagnosticsBuilder&& diagnosticsBuilder)> warningCallback = [](auto&&) {});

    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    Lexer(Lexer&&) noexcept = default;
    Lexer& operator=(Lexer&&) noexcept = default;

    [[nodiscard]] iterator begin()
    {
        return iterator(*this, 0);
    }

    [[nodiscard]] const_iterator cbegin()
    {
        return begin();
    }

    [[nodiscard]] iterator end()
    {
        return iterator(*this, -1);
    }

    [[nodiscard]] const_iterator cend()
    {
        return end();
    }

    template <class S, class... Args>
    [[nodiscard]] Diag::DiagnosticsBuilder createDiagnosticsBuilder(std::size_t location, const S& message,
                                                                    Args&&... args)
    {
        return Diag::DiagnosticsBuilder(*m_document, location, message, std::forward<Args>(args)...);
    }
};
} // namespace pylir
