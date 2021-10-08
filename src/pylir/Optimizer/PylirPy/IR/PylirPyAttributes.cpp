#include "PylirPyAttributes.hpp"

#include <mlir/IR/DialectImplementation.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/TypeSwitch.h>

#include "PylirPyDialect.hpp"

namespace pylir::Py::detail
{
struct IntAttrStorage : public mlir::AttributeStorage
{
    IntAttrStorage(BigInt value) : value(std::move(value)) {}

    using KeyTy = BigInt;

    bool operator==(const KeyTy& other) const
    {
        return value == other;
    }

    static ::llvm::hash_code hashKey(const KeyTy& key)
    {
        auto count = mp_sbin_size(&key.getHandle());
        llvm::SmallVector<std::uint8_t, 10> data(count);
        auto result = mp_to_sbin(&key.getHandle(), data.data(), count, nullptr);
        PYLIR_ASSERT(result == MP_OKAY);
        return llvm::hash_value(makeArrayRef(data));
    }

    static IntAttrStorage* construct(::mlir::AttributeStorageAllocator& allocator, const KeyTy& key)
    {
        return new (allocator.allocate<IntAttrStorage>()) IntAttrStorage(key);
    }

    BigInt value;
};
} // namespace pylir::Py::detail

#define GET_ATTRDEF_CLASSES
#include "pylir/Optimizer/PylirPy/IR/PylirPyOpsAttributes.cpp.inc"

void pylir::Py::PylirPyDialect::initializeAttributes()
{
    addAttributes<BoolAttr,
#define GET_ATTRDEF_LIST
#include "pylir/Optimizer/PylirPy/IR/PylirPyOpsAttributes.cpp.inc"
                  >();
}

mlir::Attribute pylir::Py::PylirPyDialect::parseAttribute(::mlir::DialectAsmParser& parser, ::mlir::Type type) const
{
    llvm::StringRef keyword;
    if (parser.parseKeyword(&keyword))
    {
        return {};
    }
    if (keyword == Py::BoolAttr::getMnemonic())
    {
        return Py::BoolAttr::parse(getContext(), parser, type);
    }
    mlir::Attribute result;
    (void)generatedAttributeParser(getContext(), parser, keyword, type, result);
    return result;
}

void pylir::Py::PylirPyDialect::printAttribute(::mlir::Attribute attr, ::mlir::DialectAsmPrinter& os) const
{
    if (auto boolean = attr.dyn_cast_or_null<BoolAttr>())
    {
        return boolean.print(os);
    }
    (void)generatedAttributePrinter(attr, os);
}

pylir::BigInt pylir::Py::IntAttr::getValue() const
{
    return getImpl()->value;
}

void pylir::Py::IntAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<" << getValue().toString() << ">";
}

mlir::Attribute pylir::Py::IntAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser, ::mlir::Type)
{
    llvm::APInt apInt;
    if (parser.parseLess() || parser.parseInteger(apInt) || parser.parseGreater())
    {
        return {};
    }
    llvm::SmallString<10> str;
    apInt.toStringSigned(str);
    return IntAttr::get(context, BigInt(std::string{str.data(), str.size()}));
}

void pylir::Py::ListAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<[";
    llvm::interleaveComma(getValue(), printer);
    printer << "]>";
}

mlir::Attribute pylir::Py::ListAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser, ::mlir::Type)
{
    if (parser.parseLess() || parser.parseLSquare())
    {
        return {};
    }
    if (!parser.parseOptionalRSquare())
    {
        if (parser.parseGreater())
        {
            return {};
        }
        return get(context, {});
    }
    llvm::SmallVector<mlir::Attribute> attrs;
    if (parser.parseAttribute(attrs.emplace_back()))
    {
        return {};
    }
    while (!parser.parseOptionalComma())
    {
        if (parser.parseAttribute(attrs.emplace_back()))
        {
            return {};
        }
    }
    if (parser.parseRSquare() || parser.parseGreater())
    {
        return {};
    }
    return get(context, attrs);
}

void pylir::Py::TupleAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<(";
    llvm::interleaveComma(getValue(), printer);
    printer << ")>";
}

mlir::Attribute pylir::Py::TupleAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser,
                                            ::mlir::Type)
{
    if (parser.parseLess() || parser.parseLParen())
    {
        return {};
    }
    if (!parser.parseOptionalRParen())
    {
        if (parser.parseGreater())
        {
            return {};
        }
        return get(context, {});
    }
    llvm::SmallVector<mlir::Attribute> attrs;
    if (parser.parseAttribute(attrs.emplace_back()))
    {
        return {};
    }
    while (!parser.parseOptionalComma())
    {
        if (parser.parseAttribute(attrs.emplace_back()))
        {
            return {};
        }
    }
    if (parser.parseRParen() || parser.parseGreater())
    {
        return {};
    }
    return get(context, attrs);
}

void pylir::Py::SetAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<{";
    llvm::interleaveComma(getValue(), printer);
    printer << "}>";
}

mlir::Attribute pylir::Py::SetAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser, ::mlir::Type)
{
    if (parser.parseLess() || parser.parseLBrace())
    {
        return {};
    }
    if (!parser.parseOptionalRBrace())
    {
        if (parser.parseGreater())
        {
            return {};
        }
        return get(context, {});
    }
    llvm::SmallVector<mlir::Attribute> attrs;
    if (parser.parseAttribute(attrs.emplace_back()))
    {
        return {};
    }
    while (!parser.parseOptionalComma())
    {
        if (parser.parseAttribute(attrs.emplace_back()))
        {
            return {};
        }
    }
    if (parser.parseRBrace() || parser.parseGreater())
    {
        return {};
    }
    return get(context, attrs);
}

void pylir::Py::DictAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<{";
    llvm::interleaveComma(getValue(), printer, [&](auto&& pair) { printer << pair.first << " to " << pair.second; });
    printer << "}>";
}

mlir::Attribute pylir::Py::DictAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser, ::mlir::Type)
{
    if (parser.parseLess() || parser.parseLBrace())
    {
        return {};
    }
    if (!parser.parseOptionalRBrace())
    {
        if (parser.parseGreater())
        {
            return {};
        }
        return get(context, {});
    }
    llvm::SmallVector<std::pair<mlir::Attribute, mlir::Attribute>> attrs;
    if (parser.parseAttribute(attrs.emplace_back().first) || parser.parseKeyword("to")
        || parser.parseAttribute(attrs.back().second))
    {
        return {};
    }
    while (!parser.parseOptionalComma())
    {
        if (parser.parseAttribute(attrs.emplace_back().first) || parser.parseKeyword("to")
            || parser.parseAttribute(attrs.back().second))
        {
            return {};
        }
    }
    if (parser.parseRBrace() || parser.parseGreater())
    {
        return {};
    }
    return get(context, attrs);
}

pylir::Py::BoolAttr pylir::Py::BoolAttr::get(::mlir::MLIRContext* context, bool value)
{
    return Base::get(context, BigInt(value ? 1 : 0));
}

mlir::Attribute pylir::Py::BoolAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser, ::mlir::Type)
{
    if (parser.parseLess())
    {
        return {};
    }
    llvm::StringRef keyword;
    auto loc = parser.getCurrentLocation();
    if (parser.parseKeyword(&keyword))
    {
        return {};
    }
    if (keyword != "True" && keyword != "False")
    {
        parser.emitError(loc, "Expected 'True' or 'False' instead of ") << keyword;
        return {};
    }
    if (parser.parseGreater())
    {
        return {};
    }
    return get(context, keyword == "True");
}

void pylir::Py::BoolAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<" << (getValue() ? "True" : "False") << ">";
}

bool pylir::Py::BoolAttr::getValue() const
{
    return !getImpl()->value.isZero();
}

void pylir::Py::ObjectAttr::print(::mlir::DialectAsmPrinter& printer) const
{
    printer << getMnemonic() << "<type: " << getType();
    if (getBuiltinValue())
    {
        printer << ", value: " << *getBuiltinValue();
    }
    printer << ", __dict__: " << getAttributes() << ">";
}

mlir::Attribute pylir::Py::ObjectAttr::parse(::mlir::MLIRContext* context, ::mlir::DialectAsmParser& parser,
                                             ::mlir::Type)
{
    mlir::Attribute type;
    Py::DictAttr dictAttr;
    if (parser.parseLess() || parser.parseKeyword("type") || parser.parseColon() || parser.parseAttribute(type)
        || parser.parseComma())
    {
        return {};
    }
    llvm::Optional<mlir::Attribute> builtinValue;
    if (!parser.parseOptionalKeyword("value"))
    {
        builtinValue.emplace();
        if (parser.parseColon() || parser.parseAttribute(*builtinValue) || parser.parseComma())
        {
            return {};
        }
    }
    if (parser.parseKeyword("__dict__") || parser.parseColon() || parser.parseAttribute(dictAttr)
        || parser.parseGreater())
    {
        return {};
    }
    return get(context, type, dictAttr, builtinValue);
}

void pylir::Py::ListAttr::walkImmediateSubElements(llvm::function_ref<void(mlir::Attribute)> walkAttrsFn,
                                                   llvm::function_ref<void(mlir::Type)>) const
{
    std::for_each(getValue().begin(), getValue().end(), walkAttrsFn);
}

void pylir::Py::TupleAttr::walkImmediateSubElements(llvm::function_ref<void(mlir::Attribute)> walkAttrsFn,
                                                    llvm::function_ref<void(mlir::Type)>) const
{
    std::for_each(getValue().begin(), getValue().end(), walkAttrsFn);
}

void pylir::Py::SetAttr::walkImmediateSubElements(llvm::function_ref<void(mlir::Attribute)> walkAttrsFn,
                                                  llvm::function_ref<void(mlir::Type)>) const
{
    std::for_each(getValue().begin(), getValue().end(), walkAttrsFn);
}

void pylir::Py::DictAttr::walkImmediateSubElements(llvm::function_ref<void(mlir::Attribute)> walkAttrsFn,
                                                   llvm::function_ref<void(mlir::Type)>) const
{
    std::for_each(getValue().begin(), getValue().end(),
                  [&](auto&& pair)
                  {
                      walkAttrsFn(pair.first);
                      walkAttrsFn(pair.second);
                  });
}

void pylir::Py::ObjectAttr::walkImmediateSubElements(llvm::function_ref<void(mlir::Attribute)> walkAttrsFn,
                                                     llvm::function_ref<void(mlir::Type)>) const
{
    walkAttrsFn(getType());
    walkAttrsFn(getAttributes());
    if (getBuiltinValue())
    {
        walkAttrsFn(*getBuiltinValue());
    }
}
