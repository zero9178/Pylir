// Copyright 2022 Markus Böck
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "CodeGen.hpp"

#include <mlir/Dialect/Arithmetic/IR/Arithmetic.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlowOps.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>

#include <llvm/ADT/ScopeExit.h>
#include <llvm/ADT/TypeSwitch.h>

#include <pylir/Optimizer/PylirPy/IR/PylirPyAttributes.hpp>
#include <pylir/Optimizer/PylirPy/IR/PylirPyDialect.hpp>
#include <pylir/Optimizer/PylirPy/IR/PylirPyOps.hpp>
#include <pylir/Optimizer/PylirPy/Util/Builtins.hpp>
#include <pylir/Optimizer/PylirPy/Util/Util.hpp>
#include <pylir/Parser/Visitor.hpp>
#include <pylir/Support/Functional.hpp>
#include <pylir/Support/ValueReset.hpp>

pylir::CodeGen::CodeGen(mlir::MLIRContext* context, Diag::Document& document)
    : m_builder(
        [&]
        {
            context->loadDialect<pylir::Py::PylirPyDialect>();
            context->loadDialect<mlir::func::FuncDialect>();
            return context;
        }()),
      m_module(mlir::ModuleOp::create(m_builder.getUnknownLoc())),
      m_document(&document)
{
    for (const auto& iter : Py::Builtins::allBuiltins)
    {
        if (!iter.isPublic)
        {
            continue;
        }
        constexpr std::string_view builtinsModule = "builtins.";
        if (iter.name.substr(0, builtinsModule.size()) != builtinsModule)
        {
            continue;
        }
        m_builtinNamespace.emplace(iter.name.substr(builtinsModule.size()),
                                   mlir::FlatSymbolRefAttr::get(context, iter.name));
    }
}

mlir::ModuleOp pylir::CodeGen::visit(const pylir::Syntax::FileInput& fileInput)
{
    m_builder.setInsertionPointToEnd(m_module.getBody());
    createBuiltinsImpl();

    for (const auto& token : fileInput.globals)
    {
        auto locExit = changeLoc(token);
        auto op = m_builder.createGlobalHandle(m_qualifiers + std::string(token.getValue()));
        m_globalScope.identifiers.emplace(token.getValue(), Identifier{op.getOperation()});
    }
    m_builder.setCurrentLoc(m_builder.getUnknownLoc());

    auto initFunc =
        mlir::func::FuncOp::create(m_builder.getUnknownLoc(), m_qualifiers + "__init__", m_builder.getFunctionType({}, {}));
    auto reset = implementFunction(initFunc);
    // We aren't actually at function scope, even if wwe are implementing a function
    m_functionScope.reset();
    // Go through all globals again and initialize them explicitly to unbound
    auto unbound = m_builder.createConstant(m_builder.getUnboundAttr());
    for (auto& [name, identifier] : m_globalScope.identifiers)
    {
        m_builder.createStore(unbound, mlir::FlatSymbolRefAttr::get(pylir::get<mlir::Operation*>(identifier.kind)));
    }

    visit(fileInput.input);
    if (needsTerminator())
    {
        m_builder.create<mlir::func::ReturnOp>();
    }

    return m_module;
}

void pylir::CodeGen::visit(const Syntax::RaiseStmt& raiseStmt)
{
    if (!raiseStmt.maybeException)
    {
        // TODO: Get current exception via sys.exc_info()
        PYLIR_UNREACHABLE;
    }
    auto expression = visit(*raiseStmt.maybeException);
    if (!expression)
    {
        return;
    }
    // TODO: attach __cause__ and __context__
    auto locExit = changeLoc(raiseStmt);
    auto typeOf = m_builder.createTypeOf(expression);
    auto typeObject = m_builder.createTypeRef();
    auto isTypeSubclass = buildSubclassCheck(typeOf, typeObject);
    BlockPtr isType, instanceBlock;
    instanceBlock->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
    m_builder.create<mlir::cf::CondBranchOp>(isTypeSubclass, isType, instanceBlock, mlir::ValueRange{expression});

    {
        implementBlock(isType);
        auto baseException = m_builder.createBaseExceptionRef();
        auto isBaseException = buildSubclassCheck(expression, baseException);
        BlockPtr typeError, createException;
        m_builder.create<mlir::cf::CondBranchOp>(isBaseException, createException, typeError);

        {
            implementBlock(typeError);
            auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::TypeError.name, {},
                                                m_currentExceptBlock);
            raiseException(exception);
        }

        implementBlock(createException);
        auto exception = Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__call__",
                                                    m_builder.createMakeTuple({expression}), {}, m_currentExceptBlock);
        m_builder.create<mlir::cf::BranchOp>(instanceBlock, mlir::ValueRange{exception});
    }

    implementBlock(instanceBlock);
    typeOf = m_builder.createTypeOf(instanceBlock->getArgument(0));
    auto baseException = m_builder.createBaseExceptionRef();
    auto isBaseException = buildSubclassCheck(typeOf, baseException);
    BlockPtr typeError, raiseBlock;
    m_builder.create<mlir::cf::CondBranchOp>(isBaseException, raiseBlock, typeError);

    {
        implementBlock(typeError);
        auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::TypeError.name, {},
                                            m_currentExceptBlock);
        raiseException(exception);
    }

    implementBlock(raiseBlock);
    raiseException(instanceBlock->getArgument(0));
}

void pylir::CodeGen::visit(const Syntax::ReturnStmt& returnStmt)
{
    auto locExit = changeLoc(returnStmt);
    if (!returnStmt.maybeExpression)
    {
        executeFinallyBlocks(false);
        auto none = m_builder.createNoneRef();
        m_builder.create<mlir::func::ReturnOp>(mlir::ValueRange{none});
        m_builder.clearInsertionPoint();
        return;
    }
    auto value = visit(*returnStmt.maybeExpression);
    if (!value)
    {
        return;
    }
    executeFinallyBlocks(true);
    m_builder.create<mlir::func::ReturnOp>(mlir::ValueRange{value});
    m_builder.clearInsertionPoint();
}

void pylir::CodeGen::visit(const Syntax::SingleTokenStmt& singleTokenStmt)
{
    auto locExit = changeLoc(singleTokenStmt);
    switch (singleTokenStmt.token.getTokenType())
    {
        case TokenType::BreakKeyword:
            executeFinallyBlocks();
            m_builder.create<mlir::cf::BranchOp>(m_currentLoop.breakBlock);
            m_builder.clearInsertionPoint();
            return;
        case TokenType::ContinueKeyword:
            executeFinallyBlocks();
            m_builder.create<mlir::cf::BranchOp>(m_currentLoop.continueBlock);
            m_builder.clearInsertionPoint();
            return;
        case TokenType::PassKeyword: return;
        default: PYLIR_UNREACHABLE;
    }
}

void pylir::CodeGen::visit(const Syntax::GlobalOrNonLocalStmt& globalOrNonLocalStmt)
{
    if (globalOrNonLocalStmt.token.getTokenType() == TokenType::NonlocalKeyword)
    {
        return;
    }
    if (!m_functionScope)
    {
        return;
    }
    for (const auto& identifier : globalOrNonLocalStmt.identifiers)
    {
        auto result = m_globalScope.identifiers.find(identifier.getValue());
        PYLIR_ASSERT(result != m_globalScope.identifiers.end());
        m_functionScope->identifiers.insert(*result);
    }
}

void pylir::CodeGen::assignTarget(const Syntax::Atom& atom, mlir::Value value)
{
    auto locExit = changeLoc(atom);
    writeIdentifier(IdentifierToken{atom.token}, value);
}

void pylir::CodeGen::assignTarget(const Syntax::Subscription& subscription, mlir::Value value)
{
    auto locExit = changeLoc(subscription);
    auto container = visit(*subscription.object);
    if (!container)
    {
        return;
    }
    auto indices = visit(*subscription.index);
    if (!container)
    {
        return;
    }

    Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__setitem__",
                               m_builder.createMakeTuple({container, indices, value}), {}, m_currentExceptBlock);
}

void pylir::CodeGen::assignTarget(const Syntax::AttributeRef& attributeRef, mlir::Value value)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::assignTarget(const Syntax::TupleConstruct& tupleConstruct, mlir::Value value)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::assignTarget(const Syntax::ListDisplay& listDisplay, mlir::Value value)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::visit(const Syntax::AssignmentStmt& assignmentStmt)
{
    auto locExit = changeLoc(assignmentStmt);
    if (!assignmentStmt.maybeExpression)
    {
        return;
    }
    auto rhs = visit(*assignmentStmt.maybeExpression);
    if (!rhs)
    {
        return;
    }
    for (const auto& [list, token] : assignmentStmt.targets)
    {
        assignTarget(*list, rhs);
        if (!m_builder.getInsertionBlock())
        {
            return;
        }
    }
}

std::vector<pylir::Py::IterArg> pylir::CodeGen::visit(llvm::ArrayRef<Syntax::StarredItem> starredItems)
{
    std::vector<Py::IterArg> operands;
    for (const auto& iter : starredItems)
    {
        auto value = visit(*iter.expression);
        if (!value)
        {
            return {};
        }
        if (iter.maybeStar)
        {
            operands.emplace_back(Py::IterExpansion{value});
        }
        else
        {
            operands.emplace_back(value);
        }
    }
    return operands;
}

mlir::Value pylir::CodeGen::visit(const Syntax::TupleConstruct& tupleConstruct)
{
    auto locExit = changeLoc(tupleConstruct);
    return makeTuple(visit(tupleConstruct.items));
}

mlir::Value pylir::CodeGen::visit(const Syntax::Yield&)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

mlir::Value pylir::CodeGen::visit(const Syntax::Conditional& conditional)
{
    auto locExit = changeLoc(conditional);
    auto condition = toI1(visit(*conditional.condition));
    if (!condition)
    {
        return {};
    }
    auto found = BlockPtr{};
    auto elseBlock = BlockPtr{};
    auto thenBlock = BlockPtr{};
    thenBlock->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());

    m_builder.create<mlir::cf::CondBranchOp>(condition, found, elseBlock);

    implementBlock(found);
    auto trueValue = visit(*conditional.trueValue);
    if (trueValue)
    {
        m_builder.create<mlir::cf::BranchOp>(thenBlock, trueValue);
    }

    implementBlock(elseBlock);
    auto falseValue = visit(*conditional.elseValue);
    if (falseValue)
    {
        m_builder.create<mlir::cf::BranchOp>(thenBlock, falseValue);
    }

    if (thenBlock->hasNoPredecessors())
    {
        return {};
    }
    implementBlock(thenBlock);
    return thenBlock->getArgument(0);
}

mlir::Value pylir::CodeGen::visit(const Syntax::BinOp& binOp)
{
    auto locExit = changeLoc(binOp);
    switch (binOp.operation.getTokenType())
    {
        case TokenType::OrKeyword:
        {
            auto lhs = visit(*binOp.lhs);
            if (!lhs)
            {
                return {};
            }
            auto found = BlockPtr{};
            found->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
            auto rhsTry = BlockPtr{};
            m_builder.create<mlir::cf::CondBranchOp>(toI1(lhs), found, lhs, rhsTry, mlir::ValueRange{});

            implementBlock(rhsTry);
            auto rhs = visit(*binOp.rhs);
            if (rhs)
            {
                m_builder.create<mlir::cf::BranchOp>(found, rhs);
            }

            implementBlock(found);
            return found->getArgument(0);
        }
        case TokenType::AndKeyword:
        {
            auto lhs = visit(*binOp.lhs);
            if (!lhs)
            {
                return {};
            }
            auto found = BlockPtr{};
            found->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
            auto rhsTry = BlockPtr{};
            m_builder.create<mlir::cf::CondBranchOp>(toI1(lhs), rhsTry, mlir::ValueRange{}, found,
                                                     mlir::ValueRange{lhs});

            implementBlock(rhsTry);
            auto rhs = visit(*binOp.rhs);
            if (rhs)
            {
                m_builder.create<mlir::cf::BranchOp>(found, rhs);
            }

            implementBlock(found);
            return found->getArgument(0);
        }
        case TokenType::Plus: return this->binOp("__add__", "__radd__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::Minus: return this->binOp("__sub__", "__rsub__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::BitOr: return this->binOp("__or__", "__ror__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::BitXor: return this->binOp("__xor__", "__rxor__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::BitAnd: return this->binOp("__and__", "__rand__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::ShiftLeft:
            return this->binOp("__lshift__", "__rlshift__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::ShiftRight:
            return this->binOp("__rshift__", "__rrshift__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::Star: return this->binOp("__mul__", "__rmul__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::Divide: return this->binOp("__div__", "__rdiv__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::IntDivide:
            return this->binOp("__floordiv__", "__rfloordiv__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::Remainder: return this->binOp("__mod__", "__rmod__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::AtSign: return this->binOp("__matmul__", "__rmatmul__", visit(*binOp.lhs), visit(*binOp.rhs));
        case TokenType::PowerOf: return this->binOp("__pow__", "__rpow__", visit(*binOp.lhs), visit(*binOp.rhs));
        default: PYLIR_UNREACHABLE;
    }
}

mlir::Value pylir::CodeGen::visit(const Syntax::UnaryOp& unaryOp)
{
    switch (unaryOp.operation.getTokenType())
    {
        case TokenType::NotKeyword:
        {
            auto locExit = changeLoc(unaryOp);
            auto value = toI1(visit(*unaryOp.expression));
            auto one = m_builder.create<mlir::arith::ConstantOp>(m_builder.getBoolAttr(true));
            auto inverse = m_builder.create<mlir::arith::XOrIOp>(one, value);
            return m_builder.createBoolFromI1(inverse);
        }
        case TokenType::Minus:
        case TokenType::Plus:
        case TokenType::BitNegate:
            // TODO:
            PYLIR_UNREACHABLE;
        default: PYLIR_UNREACHABLE;
    }
}

mlir::Value pylir::CodeGen::binOp(llvm::StringRef method, mlir::Value lhs, mlir::Value rhs)
{
    auto tuple = m_builder.createMakeTuple({lhs, rhs});
    return Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, method, tuple, {}, m_currentExceptBlock);
}

mlir::Value pylir::CodeGen::binOp(llvm::StringRef method, llvm::StringRef revMethod, mlir::Value lhs, mlir::Value rhs)
{
    auto trueC = m_builder.create<mlir::arith::ConstantIntOp>(true, 1);
    auto falseC = m_builder.create<mlir::arith::ConstantIntOp>(false, 1);
    BlockPtr endBlock;
    endBlock->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
    if (method == "__eq__" || method == "__ne__")
    {
        auto isSame = m_builder.createIs(lhs, rhs);
        BlockPtr continueNormal;
        m_builder.create<mlir::cf::CondBranchOp>(isSame, endBlock,
                                                 mlir::ValueRange{m_builder.createConstant(method == "__eq__")},
                                                 continueNormal, mlir::ValueRange{});
        implementBlock(continueNormal);
    }
    auto lhsType = m_builder.createTypeOf(lhs);
    auto rhsType = m_builder.createTypeOf(rhs);
    auto sameType = m_builder.createIs(lhsType, rhsType);
    BlockPtr normalMethodBlock;
    normalMethodBlock->addArgument(m_builder.getI1Type(), m_builder.getCurrentLoc());
    BlockPtr differentTypeBlock;
    m_builder.create<mlir::cf::CondBranchOp>(sameType, normalMethodBlock, mlir::ValueRange{trueC}, differentTypeBlock,
                                             mlir::ValueRange{});

    implementBlock(differentTypeBlock);
    auto subclass = buildSubclassCheck(rhsType, lhsType);
    BlockPtr isSubclassBlock;
    m_builder.create<mlir::cf::CondBranchOp>(subclass, isSubclassBlock, normalMethodBlock, mlir::ValueRange{falseC});

    implementBlock(isSubclassBlock);
    auto rhsMroTuple = m_builder.createTypeMRO(rhsType);
    auto lookup = m_builder.createMROLookup(rhsMroTuple, revMethod);
    BlockPtr hasReversedBlock;
    m_builder.create<mlir::cf::CondBranchOp>(lookup.getSuccess(), hasReversedBlock, normalMethodBlock,
                                             mlir::ValueRange{falseC});

    implementBlock(hasReversedBlock);
    auto lhsMroTuple = m_builder.createTypeMRO(lhsType);
    auto lhsLookup = m_builder.createMROLookup(lhsMroTuple, revMethod);
    BlockPtr callReversedBlock;
    BlockPtr lhsHasReversedBlock;
    m_builder.create<mlir::cf::CondBranchOp>(lhsLookup.getSuccess(), lhsHasReversedBlock, callReversedBlock);

    implementBlock(lhsHasReversedBlock);
    auto sameImplementation = m_builder.createIs(lookup.getResult(), lhsLookup.getResult());
    m_builder.create<mlir::cf::CondBranchOp>(sameImplementation, normalMethodBlock, mlir::ValueRange{falseC},
                                             callReversedBlock, mlir::ValueRange{});

    implementBlock(callReversedBlock);
    auto tuple = m_builder.createMakeTuple({rhs, lhs});
    auto reverseResult =
        Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, revMethod, tuple, {}, m_currentExceptBlock);
    auto isNotImplemented = m_builder.createIs(reverseResult, m_builder.createNotImplementedRef());
    m_builder.create<mlir::cf::CondBranchOp>(isNotImplemented, normalMethodBlock, mlir::ValueRange{trueC}, endBlock,
                                             mlir::ValueRange{reverseResult});

    implementBlock(normalMethodBlock);
    tuple = m_builder.createMakeTuple({lhs, rhs});
    BlockPtr typeErrorBlock;
    auto result = Py::buildTrySpecialMethodCall(m_builder.getCurrentLoc(), m_builder, method, tuple, {}, typeErrorBlock,
                                                m_currentExceptBlock);
    isNotImplemented = m_builder.createIs(result, m_builder.createNotImplementedRef());
    BlockPtr maybeTryReverse;
    m_builder.create<mlir::cf::CondBranchOp>(isNotImplemented, maybeTryReverse, endBlock, mlir::ValueRange{result});

    implementBlock(maybeTryReverse);
    BlockPtr actuallyTryReverse;
    m_builder.create<mlir::cf::CondBranchOp>(normalMethodBlock->getArgument(0), typeErrorBlock, actuallyTryReverse);

    implementBlock(actuallyTryReverse);
    tuple = m_builder.createMakeTuple({rhs, lhs});
    reverseResult = Py::buildTrySpecialMethodCall(m_builder.getCurrentLoc(), m_builder, revMethod, tuple, {},
                                                  typeErrorBlock, m_currentExceptBlock);
    isNotImplemented = m_builder.createIs(reverseResult, m_builder.createNotImplementedRef());
    m_builder.create<mlir::cf::CondBranchOp>(isNotImplemented, typeErrorBlock, endBlock,
                                             mlir::ValueRange{reverseResult});

    implementBlock(typeErrorBlock);
    if (method != "__eq__" && method != "__ne__")
    {
        auto typeError = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::TypeError.name, {},
                                            m_currentExceptBlock);
        raiseException(typeError);
    }
    else
    {
        mlir::Value isEqual = m_builder.createIs(lhs, rhs);
        if (method == "__ne__")
        {
            isEqual = m_builder.create<mlir::arith::XOrIOp>(isEqual, trueC);
        }
        mlir::Value boolean = m_builder.createBoolFromI1(isEqual);
        m_builder.create<mlir::cf::BranchOp>(endBlock, boolean);
    }

    implementBlock(endBlock);
    return endBlock->getArgument(0);
}

mlir::Value pylir::CodeGen::visit(const pylir::Syntax::Comparison& comparison)
{
    mlir::Value result;
    auto first = visit(*comparison.first);
    if (!first)
    {
        return {};
    }
    auto previousRHS = first;
    for (const auto& [op, rhs] : comparison.rest)
    {
        auto locExit = changeLoc(op.firstToken);
        BlockPtr found;
        if (result)
        {
            found->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
            auto rhsTry = BlockPtr{};
            m_builder.create<mlir::cf::CondBranchOp>(toI1(result), rhsTry, found, result);
            implementBlock(rhsTry);
        }

        enum class Comp
        {
            Lt,
            Gt,
            Eq,
            Ne,
            Ge,
            Le,
            Is,
            In,
        };
        bool invert = false;
        Comp comp;
        switch (op.firstToken.getTokenType())
        {
            case TokenType::LessThan: comp = Comp::Lt; break;
            case TokenType::LessOrEqual: comp = Comp::Le; break;
            case TokenType::GreaterThan: comp = Comp::Gt; break;
            case TokenType::GreaterOrEqual: comp = Comp::Ge; break;
            case TokenType::Equal: comp = Comp::Eq; break;
            case TokenType::NotEqual: comp = Comp::Ne; break;
            case TokenType::IsKeyword: comp = Comp::Is; break;
            case TokenType::InKeyword: comp = Comp::In; break;
            default: PYLIR_UNREACHABLE;
        }
        if (op.secondToken)
        {
            invert = true;
        }
        auto other = visit(*rhs);
        if (other)
        {
            mlir::Value cmp;
            switch (comp)
            {
                case Comp::Lt: cmp = binOp("__lt__", "__gt__", previousRHS, other); break;
                case Comp::Gt: cmp = binOp("__gt__", "__lt__", previousRHS, other); break;
                case Comp::Eq: cmp = binOp("__eq__", "__eq__", previousRHS, other); break;
                case Comp::Ne: cmp = binOp("__ne__", "__ne__", previousRHS, other); break;
                case Comp::Ge: cmp = binOp("__ge__", "__le__", previousRHS, other); break;
                case Comp::Le: cmp = binOp("__le__", "__ge__", previousRHS, other); break;
                case Comp::In: cmp = binOp("__contains__", previousRHS, other); break;
                case Comp::Is: cmp = m_builder.createBoolFromI1(m_builder.createIs(previousRHS, other)); break;
            }
            if (invert)
            {
                auto i1 = toI1(cmp);
                auto one = m_builder.create<mlir::arith::ConstantOp>(m_builder.getBoolAttr(true));
                auto inverse = m_builder.create<mlir::arith::XOrIOp>(one, i1);
                cmp = m_builder.createBoolFromI1(inverse);
            }
            previousRHS = other;
            if (!result)
            {
                result = cmp;
                continue;
            }
            m_builder.create<mlir::cf::BranchOp>(found, cmp);
        }

        implementBlock(found);
        result = found->getArgument(0);
        if (!other)
        {
            break;
        }
    }
    return result;
}

mlir::Value pylir::CodeGen::visit(const Syntax::Call& call)
{
    auto callable = visit(*call.expression);
    if (!callable)
    {
        return {};
    }
    auto locExit = changeLoc(call);
    auto [tuple, keywords] = pylir::match(
        call.variant,
        [&](const std::vector<Syntax::Argument>& vector) -> std::pair<mlir::Value, mlir::Value>
        { return visit(vector); },
        [&](const Syntax::Comprehension& comprehension) -> std::pair<mlir::Value, mlir::Value>
        {
            auto list = m_builder.createMakeList();
            auto one = m_builder.create<mlir::arith::ConstantIndexOp>(1);
            visit(
                [&](mlir::Value element)
                {
                    auto len = m_builder.createListLen(list);
                    auto newLen = m_builder.create<mlir::arith::AddIOp>(len, one);
                    m_builder.createListResize(list, newLen);
                    m_builder.createListSetItem(list, len, element);
                },
                comprehension);
            if (!m_builder.getInsertionBlock())
            {
                return {};
            }
            return {m_builder.createListToTuple(list), m_builder.createConstant(m_builder.getDictAttr())};
        });
    if (!tuple || !keywords)
    {
        return {};
    }
    return Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__call__",
                                      m_builder.createTuplePrepend(callable, tuple), keywords, m_currentExceptBlock);
}

void pylir::CodeGen::writeIdentifier(const IdentifierToken& identifierToken, mlir::Value value)
{
    auto locExit = changeLoc(identifierToken);
    if (m_classNamespace)
    {
        auto str = m_builder.createConstant(identifierToken.getValue());
        m_builder.createDictSetItem(m_classNamespace, str, value);
        return;
    }

    auto result = getCurrentScope().identifiers.find(identifierToken.getValue());
    // Should not be possible
    PYLIR_ASSERT(result != getCurrentScope().identifiers.end());

    pylir::match(
        result->second.kind,
        [&](mlir::Operation* global) { m_builder.createStore(value, mlir::FlatSymbolRefAttr::get(global)); },
        [&](mlir::Value cell)
        {
            auto cellType = m_builder.createCellRef();
            m_builder.createSetSlot(value, cellType, "cell_contents", cell);
        },
        [&](SSABuilder::DefinitionsMap& localMap) { localMap[m_builder.getBlock()] = value; });
}

mlir::Value pylir::CodeGen::readIdentifier(const IdentifierToken& identifierToken)
{
    auto locExit = changeLoc(identifierToken);
    BlockPtr classNamespaceFound;
    Scope* scope;
    if (m_classNamespace)
    {
        classNamespaceFound->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
        auto str = m_builder.createConstant(identifierToken.getValue());
        auto tryGet = m_builder.createDictTryGetItem(m_classNamespace, str);
        auto isUnbound = m_builder.createIsUnboundValue(tryGet);
        auto elseBlock = BlockPtr{};
        m_builder.create<mlir::cf::CondBranchOp>(isUnbound, elseBlock, classNamespaceFound, tryGet.getResult());
        implementBlock(elseBlock);

        // if not found in locals, it does not import free variables but rather goes straight to the global scope
        scope = &m_globalScope;
    }
    else
    {
        scope = &getCurrentScope();
    }
    auto result = scope->identifiers.find(identifierToken.getValue());
    if (result == scope->identifiers.end() && scope != &m_globalScope)
    {
        // Try the global namespace
        result = m_globalScope.identifiers.find(identifierToken.getValue());
        scope = &m_globalScope;
    }
    if (result == scope->identifiers.end())
    {
        if (auto builtin = m_builtinNamespace.find(identifierToken.getValue()); builtin != m_builtinNamespace.end())
        {
            auto builtinValue = m_builder.createConstant(builtin->second);
            if (!m_classNamespace)
            {
                return builtinValue;
            }
            m_builder.create<mlir::cf::BranchOp>(classNamespaceFound, mlir::ValueRange{builtinValue});
            implementBlock(classNamespaceFound);
            return classNamespaceFound->getArgument(0);
        }
        auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::NameError.name,
                                            /*TODO: string arg*/ {}, m_currentExceptBlock);
        raiseException(exception);
        if (!m_classNamespace)
        {
            return {};
        }
        implementBlock(classNamespaceFound);
        return classNamespaceFound->getArgument(0);
    }
    mlir::Value loadedValue;
    switch (result->second.kind.index())
    {
        case Identifier::Global:
            loadedValue =
                m_builder.createLoad(mlir::FlatSymbolRefAttr::get(pylir::get<mlir::Operation*>(result->second.kind)));
            break;
        case Identifier::StackAlloc:
            loadedValue = scope->ssaBuilder.readVariable(m_builder.getCurrentLoc(), m_builder.getDynamicType(),
                                                         pylir::get<SSABuilder::DefinitionsMap>(result->second.kind),
                                                         m_builder.getBlock());
            break;
        case Identifier::Cell:
        {
            auto cellType = m_builder.createCellRef();
            auto getAttrOp =
                m_builder.createGetSlot(pylir::get<mlir::Value>(result->second.kind), cellType, "cell_contents");
            auto successBlock = BlockPtr{};
            auto failureBlock = BlockPtr{};
            auto success = m_builder.createIsUnboundValue(getAttrOp);
            m_builder.create<mlir::cf::CondBranchOp>(success, successBlock, failureBlock);

            implementBlock(failureBlock);
            auto exception =
                Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::UnboundLocalError.name,
                                   /*TODO: string arg*/ {}, m_currentExceptBlock);
            raiseException(exception);

            implementBlock(successBlock);
            return getAttrOp;
        }
    }
    auto condition = m_builder.createIsUnboundValue(loadedValue);
    auto unbound = BlockPtr{};
    auto found = BlockPtr{};
    m_builder.create<mlir::cf::CondBranchOp>(condition, unbound, found);

    implementBlock(unbound);
    if (result->second.kind.index() == Identifier::Global)
    {
        auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::NameError.name,
                                            /*TODO: string arg*/ {}, m_currentExceptBlock);
        raiseException(exception);
    }
    else
    {
        auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::UnboundLocalError.name,
                                            /*TODO: string arg*/ {}, m_currentExceptBlock);
        raiseException(exception);
    }

    implementBlock(found);
    if (!m_classNamespace)
    {
        return loadedValue;
    }
    m_builder.create<mlir::cf::BranchOp>(classNamespaceFound, mlir::ValueRange{loadedValue});

    implementBlock(classNamespaceFound);
    return classNamespaceFound->getArgument(0);
}

mlir::Value pylir::CodeGen::visit(const pylir::Syntax::Atom& atom)
{
    switch (atom.token.getTokenType())
    {
        case TokenType::IntegerLiteral: return m_builder.createConstant(pylir::get<BigInt>(atom.token.getValue()));
        case TokenType::ComplexLiteral:
            // TODO:
            PYLIR_UNREACHABLE;
        case TokenType::FloatingPointLiteral:
            return m_builder.createConstant(pylir::get<double>(atom.token.getValue()));
        case TokenType::StringLiteral: return m_builder.createConstant(pylir::get<std::string>(atom.token.getValue()));
        case TokenType::ByteLiteral:
            // TODO:
            PYLIR_UNREACHABLE;
        case TokenType::TrueKeyword: return m_builder.createConstant(true);
        case TokenType::FalseKeyword: return m_builder.createConstant(false);
        case TokenType::NoneKeyword: return m_builder.createNoneRef();
        case TokenType::Identifier: return readIdentifier(IdentifierToken{atom.token});
        default: PYLIR_UNREACHABLE;
    }
}

mlir::Value pylir::CodeGen::visit(const pylir::Syntax::Subscription& subscription)
{
    auto container = visit(*subscription.object);
    if (!container)
    {
        return {};
    }
    auto indices = visit(*subscription.index);
    if (!container)
    {
        return {};
    }

    auto locExit = changeLoc(subscription);
    return buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__getitem__",
                                  m_builder.createMakeTuple({container, indices}), {}, m_currentExceptBlock);
}

mlir::Value pylir::CodeGen::toI1(mlir::Value value)
{
    auto locExit = changeLoc(value.getLoc());
    auto boolean = toBool(value);
    return m_builder.createBoolToI1(boolean);
}

mlir::Value pylir::CodeGen::toBool(mlir::Value value)
{
    auto locExit = changeLoc(value.getLoc());
    auto tuple = m_builder.createMakeTuple({m_builder.createBoolRef(), value});
    auto boolean =
        Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__call__", tuple, {}, m_currentExceptBlock);
    return boolean;
}

mlir::Value pylir::CodeGen::visit(const Syntax::ListDisplay& listDisplay)
{
    auto locExit = changeLoc(listDisplay);
    return pylir::match(
        listDisplay.variant,
        [&](const std::vector<Syntax::StarredItem>& list) -> mlir::Value
        {
            auto operands = visit(list);
            return makeList(operands);
        },
        [&](const Syntax::Comprehension& comprehension) -> mlir::Value
        {
            auto list = m_builder.createMakeList();
            auto one = m_builder.create<mlir::arith::ConstantIndexOp>(1);
            visit(
                [&](mlir::Value element)
                {
                    auto len = m_builder.createListLen(list);
                    auto newLen = m_builder.create<mlir::arith::AddIOp>(len, one);
                    m_builder.createListResize(list, newLen);
                    m_builder.createListSetItem(list, len, element);
                },
                comprehension);
            if (!m_builder.getInsertionBlock())
            {
                return {};
            }
            return list;
        });
}

mlir::Value pylir::CodeGen::visit(const Syntax::SetDisplay& setDisplay)
{
    auto locExit = changeLoc(setDisplay);
    return pylir::match(
        setDisplay.variant,
        [&](const std::vector<Syntax::StarredItem>& list) -> mlir::Value
        {
            auto operands = visit(list);
            return makeSet(operands);
        },
        [&](const Syntax::Comprehension&) -> mlir::Value
        {
            // TODO:
            PYLIR_UNREACHABLE;
        });
}

mlir::Value pylir::CodeGen::visit(const Syntax::DictDisplay& dictDisplay)
{
    auto locExit = changeLoc(dictDisplay);
    return pylir::match(
        dictDisplay.variant,
        [&](const std::vector<Syntax::DictDisplay::KeyDatum>& list) -> mlir::Value
        {
            std::vector<Py::DictArg> result;
            for (const auto& iter : list)
            {
                auto key = visit(*iter.key);
                if (!key)
                {
                    return {};
                }
                if (!iter.maybeValue)
                {
                    result.emplace_back(Py::MappingExpansion{key});
                    continue;
                }
                auto value = visit(*iter.maybeValue);
                if (!value)
                {
                    return {};
                }
                result.emplace_back(std::pair{key, value});
            }
            return m_builder.createMakeDict(result);
        },
        [&](const Syntax::DictDisplay::DictComprehension&) -> mlir::Value
        {
            // TODO:
            PYLIR_UNREACHABLE;
        });
}

mlir::Value pylir::CodeGen::visit(const pylir::Syntax::Assignment& assignment)
{
    auto locExit = changeLoc(assignment);
    auto value = visit(*assignment.expression);
    if (!value)
    {
        return {};
    }
    writeIdentifier(assignment.variable, value);
    return value;
}

void pylir::CodeGen::visit(const Syntax::IfStmt& ifStmt)
{
    auto locExit = changeLoc(ifStmt, ifStmt.ifKeyword);
    auto condition = visit(*ifStmt.condition);
    if (!condition)
    {
        return;
    }
    auto trueBlock = BlockPtr{};
    BlockPtr thenBlock;
    auto exitBlock = llvm::make_scope_exit(
        [&]
        {
            if (!thenBlock->hasNoPredecessors())
            {
                implementBlock(thenBlock);
            }
        });
    mlir::Block* elseBlock;
    if (!ifStmt.elseSection && ifStmt.elifs.empty())
    {
        elseBlock = thenBlock;
    }
    else
    {
        elseBlock = new mlir::Block;
    }
    m_builder.create<mlir::cf::CondBranchOp>(toI1(condition), trueBlock, elseBlock);

    implementBlock(trueBlock);
    visit(*ifStmt.suite);
    if (needsTerminator())
    {
        m_builder.create<mlir::cf::BranchOp>(thenBlock);
    }
    if (thenBlock == elseBlock)
    {
        return;
    }
    implementBlock(elseBlock);
    for (const auto& iter : llvm::enumerate(ifStmt.elifs))
    {
        auto locExit2 = changeLoc(ifStmt, iter.value().elif);
        condition = visit(*iter.value().condition);
        if (!condition)
        {
            return;
        }
        trueBlock = BlockPtr{};
        if (iter.index() == ifStmt.elifs.size() - 1 && !ifStmt.elseSection)
        {
            elseBlock = thenBlock;
        }
        else
        {
            elseBlock = new mlir::Block;
        }

        m_builder.create<mlir::cf::CondBranchOp>(toI1(condition), trueBlock, elseBlock);

        implementBlock(trueBlock);
        visit(*iter.value().suite);
        if (needsTerminator())
        {
            m_builder.create<mlir::cf::BranchOp>(thenBlock);
        }
        if (thenBlock != elseBlock)
        {
            implementBlock(elseBlock);
        }
    }
    if (ifStmt.elseSection)
    {
        visit(*ifStmt.elseSection->suite);
        if (needsTerminator())
        {
            m_builder.create<mlir::cf::BranchOp>(thenBlock);
        }
    }
}

void pylir::CodeGen::visit(const Syntax::WhileStmt& whileStmt)
{
    auto locExit = changeLoc(whileStmt);
    auto conditionBlock = BlockPtr{};
    auto thenBlock = BlockPtr{};
    auto exitBlock = llvm::make_scope_exit(
        [&]
        {
            if (!thenBlock->hasNoPredecessors())
            {
                implementBlock(thenBlock);
            }
        });
    m_builder.create<mlir::cf::BranchOp>(conditionBlock);

    implementBlock(conditionBlock);
    auto conditionSeal = markOpenBlock(conditionBlock);
    auto condition = visit(*whileStmt.condition);
    if (!condition)
    {
        return;
    }
    mlir::Block* elseBlock;
    if (whileStmt.elseSection)
    {
        elseBlock = new mlir::Block;
    }
    else
    {
        elseBlock = thenBlock;
    }
    auto body = BlockPtr{};
    m_builder.create<mlir::cf::CondBranchOp>(toI1(condition), body, elseBlock);

    implementBlock(body);
    std::optional exit = pylir::ValueReset(m_currentLoop);
    m_currentLoop = {thenBlock, conditionBlock};
    visit(*whileStmt.suite);
    if (needsTerminator())
    {
        m_builder.create<mlir::cf::BranchOp>(conditionBlock);
    }
    exit.reset();
    if (elseBlock == thenBlock)
    {
        return;
    }
    implementBlock(elseBlock);
    visit(*whileStmt.elseSection->suite);
    if (needsTerminator())
    {
        m_builder.create<mlir::cf::BranchOp>(thenBlock);
    }
}

void pylir::CodeGen::visitForConstruct(const Syntax::Target& targets, mlir::Value iterable,
                                       llvm::function_ref<void()> execSuite,
                                       const std::optional<Syntax::IfStmt::Else>& elseSection)
{
    auto iterObject = Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__iter__",
                                                 m_builder.createMakeTuple({iterable}), {}, m_currentExceptBlock);

    BlockPtr condition;
    m_builder.create<mlir::cf::BranchOp>(condition);

    implementBlock(condition);
    auto conditionSeal = markOpenBlock(condition);
    BlockPtr stopIterationHandler, thenBlock;
    auto implementThenBlock = llvm::make_scope_exit(
        [&]
        {
            if (!thenBlock->hasNoPredecessors())
            {
                implementBlock(thenBlock);
            }
        });

    auto stopIterationSeal = markOpenBlock(stopIterationHandler);
    stopIterationHandler->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
    auto next = Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__next__",
                                           m_builder.createMakeTuple({iterObject}), {}, stopIterationHandler);
    assignTarget(targets, next);
    mlir::Block* elseBlock;
    if (elseSection)
    {
        elseBlock = new mlir::Block;
    }
    else
    {
        elseBlock = thenBlock;
    }
    BlockPtr body;
    m_builder.create<mlir::cf::BranchOp>(body);

    implementBlock(body);
    std::optional exit = pylir::ValueReset(m_currentLoop);
    m_currentLoop = {thenBlock, condition};
    execSuite();
    if (needsTerminator())
    {
        m_builder.create<mlir::cf::BranchOp>(condition);
    }
    exit.reset();
    if (!stopIterationHandler->hasNoPredecessors())
    {
        implementBlock(stopIterationHandler);
        auto stopIteration = m_builder.createStopIterationRef();
        auto typeOf = m_builder.createTypeOf(stopIterationHandler->getArgument(0));
        auto isStopIteration = m_builder.createIs(stopIteration, typeOf);
        auto* reraiseBlock = new mlir::Block;
        m_builder.create<mlir::cf::CondBranchOp>(isStopIteration, elseBlock, reraiseBlock);
        implementBlock(reraiseBlock);
        m_builder.createRaise(stopIterationHandler->getArgument(0));
    }
    if (elseBlock == thenBlock)
    {
        return;
    }
    implementBlock(elseBlock);
    visit(*elseSection->suite);
    if (needsTerminator())
    {
        m_builder.create<mlir::cf::BranchOp>(thenBlock);
    }
}

void pylir::CodeGen::visit(const pylir::Syntax::ForStmt& forStmt)
{
    auto locExit = changeLoc(forStmt);
    auto iterable = visit(*forStmt.expression);
    if (!iterable)
    {
        return;
    }
    auto loc = getLoc(forStmt, forStmt.forKeyword);
    visitForConstruct(
        *forStmt.targetList, iterable, [&] { visit(*forStmt.suite); }, forStmt.elseSection);
}

void pylir::CodeGen::visit(llvm::function_ref<void(mlir::Value)> insertOperation, const Syntax::Expression& iteration,
                           const Syntax::CompFor& compFor)
{
    auto locExit = changeLoc(compFor);
    auto iterable = visit(*compFor.test);
    if (!iterable)
    {
        return;
    }
    visitForConstruct(*compFor.targets, iterable,
                      [&]
                      {
                          pylir::match(
                              compFor.compIter, [&](std::monostate) { insertOperation(visit(iteration)); },
                              [&](const auto& ptr) { visit(insertOperation, iteration, *ptr); });
                      });
}

void pylir::CodeGen::visit(llvm::function_ref<void(mlir::Value)> insertOperation, const Syntax::Expression& iteration,
                           const Syntax::CompIf& compIf)
{
    auto locExit = changeLoc(compIf);
    auto condition = visit(*compIf.test);
    if (!condition)
    {
        return;
    }
    auto trueBlock = BlockPtr{};
    auto thenBlock = BlockPtr{};
    m_builder.setCurrentLoc(getLoc(compIf, compIf.ifToken));
    m_builder.create<mlir::cf::CondBranchOp>(toI1(condition), trueBlock, thenBlock);

    implementBlock(trueBlock);
    pylir::match(
        compIf.compIter, [&](std::monostate) { insertOperation(visit(iteration)); },
        [&](const auto& ptr) { visit(insertOperation, iteration, *ptr); });
    implementBlock(thenBlock);
}

void pylir::CodeGen::visit(llvm::function_ref<void(mlir::Value)> insertOperation,
                           const Syntax::Comprehension& comprehension)
{
    auto locExit = changeLoc(comprehension);
    visit(insertOperation, *comprehension.expression, comprehension.compFor);
}

void pylir::CodeGen::visit(const pylir::Syntax::TryStmt& tryStmt)
{
    auto locExit = changeLoc(tryStmt);
    BlockPtr exceptionHandler;
    exceptionHandler->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
    auto exceptionHandlerSeal = markOpenBlock(exceptionHandler);
    std::optional reset = pylir::valueResetMany(m_currentExceptBlock, m_currentExceptBlock);
    auto lambda = [&] { m_finallyBlocks.pop_back(); };
    std::optional<decltype(llvm::make_scope_exit(lambda))> popFinally;
    if (tryStmt.finally)
    {
        m_finallyBlocks.push_back({&*tryStmt.finally, m_currentLoop, m_currentExceptBlock});
        popFinally.emplace(llvm::make_scope_exit(lambda));
    }
    m_currentExceptBlock = exceptionHandler;
    visit(*tryStmt.suite);

    auto enterFinallyCode = [&]
    {
        auto back = m_finallyBlocks.back();
        m_finallyBlocks.pop_back();
        auto tuple = std::make_tuple(llvm::make_scope_exit([back, this] { m_finallyBlocks.push_back(back); }),
                                     pylir::valueResetMany(m_currentExceptBlock));
        m_currentExceptBlock = back.parentExceptBlock;
        return tuple;
    };

    if (needsTerminator())
    {
        if (tryStmt.elseSection)
        {
            visit(*tryStmt.elseSection->suite);
            if (needsTerminator() && tryStmt.finally)
            {
                auto finalSection = enterFinallyCode();
                visit(*tryStmt.finally->suite);
            }
        }
        else if (tryStmt.finally)
        {
            auto finalSection = enterFinallyCode();
            visit(*tryStmt.finally->suite);
        }
    }

    BlockPtr continueBlock;
    auto exitBlock = llvm::make_scope_exit(
        [&]
        {
            if (!continueBlock->hasNoPredecessors())
            {
                implementBlock(continueBlock);
            }
        });
    if (needsTerminator())
    {
        m_builder.setCurrentLoc(getLoc(tryStmt, tryStmt.tryKeyword));
        m_builder.create<mlir::cf::BranchOp>(continueBlock);
    }

    if (exceptionHandler->hasNoPredecessors())
    {
        return;
    }

    implementBlock(exceptionHandler);
    // Exceptions thrown in exception handlers (including the expression after except) are propagated upwards and not
    // handled by this block
    reset.reset();

    for (const auto& iter : tryStmt.excepts)
    {
        auto locExit2 = changeLoc(tryStmt, iter.exceptKeyword);
        auto value = visit(*iter.filter);
        if (!value)
        {
            return;
        }
        if (iter.maybeName)
        {
            // TODO: Python requires this identifier to be unbound at the end of the exception handler as if done in
            //       a finally section
            writeIdentifier(*iter.maybeName, exceptionHandler->getArgument(0));
        }
        auto tupleType = m_builder.createTupleRef();
        auto isTuple = m_builder.createIs(m_builder.createTypeOf(value), tupleType);
        auto tupleBlock = BlockPtr{};
        auto exceptionBlock = BlockPtr{};
        m_builder.create<mlir::cf::CondBranchOp>(isTuple, tupleBlock, exceptionBlock);

        BlockPtr skipBlock;
        BlockPtr suiteBlock;
        {
            implementBlock(exceptionBlock);
            // TODO: check value is a type
            auto baseException = m_builder.createBaseExceptionRef();
            auto isSubclass = buildSubclassCheck(value, baseException);
            BlockPtr raiseBlock;
            BlockPtr noTypeErrorBlock;
            m_builder.create<mlir::cf::CondBranchOp>(isSubclass, noTypeErrorBlock, raiseBlock);

            implementBlock(raiseBlock);
            auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder, Py::Builtins::TypeError.name, {},
                                                m_currentExceptBlock);
            raiseException(exception);

            implementBlock(noTypeErrorBlock);
            auto exceptionType = m_builder.createTypeOf(exceptionHandler->getArgument(0));
            isSubclass = buildSubclassCheck(exceptionType, value);
            m_builder.create<mlir::cf::CondBranchOp>(isSubclass, suiteBlock, skipBlock);
        }
        {
            implementBlock(tupleBlock);
            auto baseException = m_builder.createBaseExceptionRef();
            BlockPtr noTypeErrorsBlock;
            buildTupleForEach(value, noTypeErrorsBlock, {},
                              [&](mlir::Value entry)
                              {
                                  // TODO: check entry is a type
                                  auto isSubclass = buildSubclassCheck(entry, baseException);
                                  BlockPtr raiseBlock;
                                  BlockPtr noTypeErrorBlock;
                                  m_builder.create<mlir::cf::CondBranchOp>(isSubclass, noTypeErrorBlock, raiseBlock);

                                  implementBlock(raiseBlock);
                                  auto exception =
                                      Py::buildException(m_builder.getCurrentLoc(), m_builder,
                                                         Py::Builtins::TypeError.name, {}, m_currentExceptBlock);
                                  raiseException(exception);

                                  implementBlock(noTypeErrorBlock);
                              });
            implementBlock(noTypeErrorsBlock);
            auto exceptionType = m_builder.createTypeOf(exceptionHandler->getArgument(0));
            buildTupleForEach(value, skipBlock, {},
                              [&](mlir::Value entry)
                              {
                                  auto isSubclass = buildSubclassCheck(exceptionType, entry);
                                  BlockPtr continueLoop;
                                  m_builder.create<mlir::cf::CondBranchOp>(isSubclass, suiteBlock, continueLoop);
                                  implementBlock(continueLoop);
                              });
        }

        implementBlock(suiteBlock);
        visit(*iter.suite);
        if (needsTerminator())
        {
            if (tryStmt.finally)
            {
                auto finallySection = enterFinallyCode();
                visit(*tryStmt.finally->suite);
            }
            if (needsTerminator())
            {
                m_builder.create<mlir::cf::BranchOp>(continueBlock);
            }
        }
        implementBlock(skipBlock);
    }
    if (tryStmt.maybeExceptAll)
    {
        visit(*tryStmt.maybeExceptAll->suite);
        if (needsTerminator())
        {
            if (tryStmt.finally)
            {
                auto finallySection = enterFinallyCode();
                visit(*tryStmt.finally->suite);
            }
            if (needsTerminator())
            {
                m_builder.create<mlir::cf::BranchOp>(continueBlock);
            }
        }
    }
    if (needsTerminator())
    {
        if (tryStmt.finally)
        {
            auto finallyCode = enterFinallyCode();
            visit(*tryStmt.finally->suite);
        }
        if (needsTerminator())
        {
            m_builder.createRaise(exceptionHandler->getArgument(0));
        }
    }
}

void pylir::CodeGen::visit(const pylir::Syntax::WithStmt& withStmt)
{
    //TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::visit(const pylir::Syntax::FuncDef& funcDef)
{
    std::vector<Py::IterArg> defaultParameters;
    std::vector<Py::DictArg> keywordOnlyDefaultParameters;
    std::vector<IdentifierToken> functionParametersTokens;
    std::vector<FunctionParameter> functionParameters;
    for (const auto& iter : funcDef.parameterList)
    {
        functionParametersTokens.push_back(iter.name);
        functionParameters.push_back({std::string{iter.name.getValue()},
                                      static_cast<FunctionParameter::Kind>(iter.kind), iter.maybeDefault != nullptr});
        if (!iter.maybeDefault)
        {
            continue;
        }
        auto value = visit(*iter.maybeDefault);
        if (!value)
        {
            return;
        }
        if (iter.kind != Syntax::Parameter::KeywordOnly)
        {
            defaultParameters.emplace_back(value);
            continue;
        }
        auto locExit = changeLoc(iter);
        auto name = m_builder.createConstant(iter.name.getValue());
        keywordOnlyDefaultParameters.push_back(std::pair{name, value});
    }
    auto locExit = changeLoc(funcDef);
    auto qualifiedName = m_qualifiers + std::string(funcDef.funcName.getValue());
    std::vector<IdentifierToken> usedClosures;
    mlir::func::FuncOp func;
    {
        pylir::ValueReset namespaceReset(m_classNamespace);
        m_classNamespace = {};
        func =
            mlir::func::FuncOp::create(m_builder.getCurrentLoc(), formImplName(qualifiedName + "$impl"),
                                       m_builder.getFunctionType(std::vector<mlir::Type>(1 + functionParameters.size(),
                                                                                         m_builder.getDynamicType()),
                                                                 {m_builder.getDynamicType()}));
        func.setPrivate();
        auto reset = implementFunction(func);

        m_qualifiers.append(funcDef.funcName.getValue());
        m_qualifiers += ".<locals>.";
        auto locals = funcDef.localVariables;
        auto closures = funcDef.closures;
        for (auto [name, value] : llvm::zip(functionParametersTokens, llvm::drop_begin(func.getArguments())))
        {
            if (funcDef.closures.count(name))
            {
                auto closureType = m_builder.createCellRef();
                auto tuple = m_builder.createMakeTuple({closureType, value});
                auto emptyDict = m_builder.createConstant(m_builder.getDictAttr());
                auto metaType = m_builder.createTypeOf(closureType);
                auto newMethod = m_builder.createGetSlot(closureType, metaType, "__new__");
                mlir::Value cell = m_builder.createFunctionCall(newMethod, {newMethod, tuple, emptyDict});
                m_functionScope->identifiers.emplace(name.getValue(), Identifier{cell});
                closures.erase(name);
            }
            else
            {
                m_functionScope->identifiers.emplace(
                    name.getValue(), Identifier{SSABuilder::DefinitionsMap{{m_builder.getBlock(), value}}});
                locals.erase(name);
            }
        }
        for (const auto& iter : locals)
        {
            m_functionScope->identifiers.emplace(iter.getValue(), Identifier{SSABuilder::DefinitionsMap{}});
        }
        for (const auto& iter : closures)
        {
            auto closureType = m_builder.createCellRef();
            auto tuple = m_builder.createMakeTuple({closureType});
            auto emptyDict = m_builder.createConstant(m_builder.getDictAttr());
            auto metaType = m_builder.createTypeOf(closureType);
            auto newMethod = m_builder.createGetSlot(closureType, metaType, "__new__");
            mlir::Value cell = m_builder.createFunctionCall(newMethod, {newMethod, tuple, emptyDict});
            m_functionScope->identifiers.emplace(iter.getValue(), Identifier{cell});
        }
        if (!funcDef.nonLocalVariables.empty())
        {
            auto self = func.getArgument(0);
            auto metaType = m_builder.createFunctionRef();
            auto closureTuple = m_builder.createGetSlot(self, metaType, "__closure__");
            for (const auto& iter : llvm::enumerate(funcDef.nonLocalVariables))
            {
                auto constant = m_builder.create<mlir::arith::ConstantIndexOp>(iter.index());
                auto cell = m_builder.createTupleGetItem(closureTuple, constant);
                m_functionScope->identifiers.emplace(iter.value().getValue(), Identifier{mlir::Value{cell}});
                usedClosures.push_back(iter.value());
            }
        }

        visit(*funcDef.suite);
        if (needsTerminator())
        {
            m_builder.create<mlir::func::ReturnOp>(mlir::ValueRange{m_builder.createNoneRef()});
        }
        func = buildFunctionCC(formImplName(qualifiedName + "$cc"), func, functionParameters);
    }
    mlir::Value value = m_builder.createMakeFunc(mlir::FlatSymbolRefAttr::get(func));
    auto type = m_builder.createTypeOf(value);
    m_builder.createSetSlot(value, type, "__qualname__", m_builder.createConstant(qualifiedName));
    {
        mlir::Value defaults;
        if (defaultParameters.empty())
        {
            defaults = m_builder.createNoneRef();
        }
        else
        {
            defaults = m_builder.createMakeTuple(defaultParameters);
        }
        m_builder.createSetSlot(value, type, "__defaults__", defaults);
    }
    {
        mlir::Value kwDefaults;
        if (keywordOnlyDefaultParameters.empty())
        {
            kwDefaults = m_builder.createNoneRef();
        }
        else
        {
            kwDefaults = m_builder.createMakeDict(keywordOnlyDefaultParameters);
        }
        m_builder.createSetSlot(value, type, "__kwdefaults__", kwDefaults);
    }
    {
        mlir::Value closure;
        if (usedClosures.empty())
        {
            closure = m_builder.createNoneRef();
        }
        else
        {
            std::vector<Py::IterArg> args(usedClosures.size());
            std::transform(usedClosures.begin(), usedClosures.end(), args.begin(),
                           [&](const IdentifierToken& token) -> Py::IterArg
                           {
                               auto result = getCurrentScope().identifiers.find(token.getValue());
                               PYLIR_ASSERT(result != getCurrentScope().identifiers.end());
                               return pylir::get<mlir::Value>(result->second.kind);
                           });
            closure = m_builder.createMakeTuple(args);
        }
        m_builder.createSetSlot(value, type, "__closure__", closure);
    }
    for (const auto& iter : llvm::reverse(funcDef.decorators))
    {
        auto locExit2 = changeLoc(iter);
        auto decorator = visit(*iter.expression);
        if (!decorator)
        {
            return;
        }
        value = Py::buildSpecialMethodCall(m_builder.getCurrentLoc(), m_builder, "__call__",
                                           m_builder.createMakeTuple({decorator, value}), {}, m_currentExceptBlock);
    }
    writeIdentifier(funcDef.funcName, value);
}

void pylir::CodeGen::visit(const pylir::Syntax::ClassDef& classDef)
{
    m_builder.setCurrentLoc(getLoc(classDef, classDef.className));
    mlir::Value bases, keywords;
    if (classDef.inheritance)
    {
        std::tie(bases, keywords) = visit(classDef.inheritance->argumentList);
    }
    else
    {
        bases = m_builder.createConstant(m_builder.getTupleAttr());
        keywords = m_builder.createConstant(m_builder.getDictAttr());
    }
    auto qualifiedName = m_qualifiers + std::string(classDef.className.getValue());
    auto name = m_builder.createConstant(qualifiedName);

    mlir::func::FuncOp func;
    {
        func =
            mlir::func::FuncOp::create(m_builder.getCurrentLoc(), formImplName(qualifiedName + "$impl"),
                                 m_builder.getFunctionType(std::vector<mlir::Type>(2 /* cell tuple + namespace dict */,
                                                                                   m_builder.getDynamicType()),
                                                           {m_builder.getDynamicType()}));
        func.setPrivate();
        auto reset = implementFunction(func);
        m_qualifiers.append(classDef.className.getValue()) += ".";

        pylir::ValueReset namespaceReset(m_classNamespace);
        m_classNamespace = func.getArgument(1);

        visit(*classDef.suite);
        m_builder.create<mlir::func::ReturnOp>(m_classNamespace);
    }
    // TODO:
    //    auto value = m_builder.createMakeClass(mlir::FlatSymbolRefAttr::get(func), name, bases, keywords);
    //    writeIdentifier(classDef.className, value);
}

void pylir::CodeGen::visit(const Syntax::Suite& suite)
{
    for (const auto& iter : suite.statements)
    {
        pylir::match(iter, [&](const auto& ptr) { visit(*ptr); });
    }
}

std::pair<mlir::Value, mlir::Value> pylir::CodeGen::visit(llvm::ArrayRef<pylir::Syntax::Argument> argumentList)
{
    std::vector<Py::IterArg> iterArgs;
    std::vector<Py::DictArg> dictArgs;
    for (const auto& iter : argumentList)
    {
        if (iter.maybeName)
        {
            auto locExit = changeLoc(iter);
            mlir::Value key = m_builder.createConstant(iter.maybeName->getValue());
            dictArgs.push_back(std::pair{key, visit(*iter.expression)});
            continue;
        }
        if (!iter.maybeExpansionsOrEqual)
        {
            iterArgs.emplace_back(visit(*iter.expression));
            continue;
        }
        switch (iter.maybeExpansionsOrEqual->getTokenType())
        {
            case TokenType::Star: iterArgs.emplace_back(Py::IterExpansion{visit(*iter.expression)}); break;
            case TokenType::PowerOf: dictArgs.emplace_back(Py::MappingExpansion{visit(*iter.expression)}); break;
            default: PYLIR_UNREACHABLE;
        }
    }
    auto tuple = makeTuple(iterArgs);
    auto dict = dictArgs.empty() ? m_builder.createConstant(m_builder.getDictAttr()) : makeDict(dictArgs);
    return {tuple, dict};
}

std::string pylir::CodeGen::formImplName(std::string_view symbol)
{
    auto result = std::string(symbol);
    auto& index = m_implNames[result];
    result += "[" + std::to_string(index) + "]";
    index++;
    return result;
}

void pylir::CodeGen::raiseException(mlir::Value exceptionObject)
{
    if (m_currentExceptBlock)
    {
        m_builder.create<mlir::cf::BranchOp>(m_currentExceptBlock, exceptionObject);
    }
    else
    {
        m_builder.createRaise(exceptionObject);
    }
    m_builder.clearInsertionPoint();
}

std::vector<pylir::CodeGen::UnpackResults>
    pylir::CodeGen::unpackArgsKeywords(mlir::Value tuple, mlir::Value dict,
                                       const std::vector<FunctionParameter>& parameters,
                                       llvm::function_ref<mlir::Value(std::size_t)> posDefault,
                                       llvm::function_ref<mlir::Value(std::string_view)> kwDefault)
{
    auto tupleLen = m_builder.createTupleLen(tuple);

    std::vector<UnpackResults> args;
    std::size_t posIndex = 0;
    std::size_t posDefaultsIndex = 0;
    for (const auto& iter : parameters)
    {
        mlir::Value argValue;
        switch (iter.kind)
        {
            case FunctionParameter::Normal:
            case FunctionParameter::PosOnly:
            {
                auto constant = m_builder.create<mlir::arith::ConstantIndexOp>(posIndex++);
                auto isLess =
                    m_builder.create<mlir::arith::CmpIOp>(mlir::arith::CmpIPredicate::ult, constant, tupleLen);
                auto lessBlock = BlockPtr{};
                auto unboundBlock = BlockPtr{};
                m_builder.create<mlir::cf::CondBranchOp>(isLess, lessBlock, unboundBlock);

                auto resultBlock = BlockPtr{};
                resultBlock->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
                implementBlock(unboundBlock);
                auto unboundValue = m_builder.createConstant(m_builder.getUnboundAttr());
                m_builder.create<mlir::cf::BranchOp>(resultBlock, mlir::ValueRange{unboundValue});

                implementBlock(lessBlock);
                auto fetched = m_builder.createTupleGetItem(tuple, constant);
                m_builder.create<mlir::cf::BranchOp>(resultBlock, mlir::ValueRange{fetched});

                implementBlock(resultBlock);
                argValue = resultBlock->getArgument(0);
                if (iter.kind == FunctionParameter::PosOnly)
                {
                    break;
                }
                [[fallthrough]];
            }
            case FunctionParameter::KeywordOnly:
            {
                auto constant = m_builder.createConstant(iter.name);
                auto lookup = m_builder.createDictTryGetItem(dict, constant);
                auto lookupIsUnbound = m_builder.createIsUnboundValue(lookup);
                auto foundBlock = BlockPtr{};
                auto notFoundBlock = BlockPtr{};
                m_builder.create<mlir::cf::CondBranchOp>(lookupIsUnbound, notFoundBlock, foundBlock);

                auto resultBlock = BlockPtr{};
                resultBlock->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
                implementBlock(notFoundBlock);
                auto elseValue = argValue ? argValue : m_builder.createConstant(m_builder.getUnboundAttr());
                m_builder.create<mlir::cf::BranchOp>(resultBlock, mlir::ValueRange{elseValue});

                implementBlock(foundBlock);
                m_builder.createDictDelItem(dict, constant);
                // value can't be assigned both through a positional argument as well as keyword argument
                if (argValue)
                {
                    auto isUnbound = m_builder.createIsUnboundValue(argValue);
                    auto boundBlock = BlockPtr{};
                    m_builder.create<mlir::cf::CondBranchOp>(
                        isUnbound, resultBlock, mlir::ValueRange{lookup.getResult()}, boundBlock, mlir::ValueRange{});

                    implementBlock(boundBlock);
                    auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder,
                                                        Py::Builtins::TypeError.name, {}, m_currentExceptBlock);
                    raiseException(exception);
                }
                else
                {
                    m_builder.create<mlir::cf::BranchOp>(resultBlock, mlir::ValueRange{lookup.getResult()});
                }

                implementBlock(resultBlock);
                argValue = resultBlock->getArgument(0);
                break;
            }
            case FunctionParameter::PosRest:
            {
                auto start = m_builder.create<mlir::arith::ConstantIndexOp>(posIndex);
                argValue = m_builder.createTupleDropFront(start, tuple);
                break;
            }
            case FunctionParameter::KeywordRest:
                // TODO: make copy of dict
                argValue = dict;
                break;
        }
        switch (iter.kind)
        {
            case FunctionParameter::PosOnly:
            case FunctionParameter::Normal:
            case FunctionParameter::KeywordOnly:
            {
                auto isUnbound = m_builder.createIsUnboundValue(argValue);
                auto unboundBlock = BlockPtr{};
                auto boundBlock = BlockPtr{};
                boundBlock->addArgument(m_builder.getDynamicType(), m_builder.getCurrentLoc());
                boundBlock->addArgument(m_builder.getI1Type(), m_builder.getCurrentLoc());
                auto trueConstant = m_builder.create<mlir::arith::ConstantIntOp>(true, 1);
                m_builder.create<mlir::cf::CondBranchOp>(isUnbound, unboundBlock, boundBlock,
                                                         mlir::ValueRange{argValue, trueConstant});

                implementBlock(unboundBlock);
                if (!iter.hasDefaultParam)
                {
                    auto exception = Py::buildException(m_builder.getCurrentLoc(), m_builder,
                                                        Py::Builtins::TypeError.name, {}, m_currentExceptBlock);
                    raiseException(exception);
                }
                else
                {
                    mlir::Value defaultArg;
                    switch (iter.kind)
                    {
                        case FunctionParameter::Normal:
                        case FunctionParameter::PosOnly:
                        {
                            PYLIR_ASSERT(posDefault);
                            defaultArg = posDefault(posDefaultsIndex++);
                            break;
                        }
                        case FunctionParameter::KeywordOnly:
                        {
                            PYLIR_ASSERT(kwDefault);
                            defaultArg = kwDefault(iter.name);
                            break;
                        }
                        default: PYLIR_UNREACHABLE;
                    }
                    auto falseConstant = m_builder.create<mlir::arith::ConstantIntOp>(false, 1);
                    m_builder.create<mlir::cf::BranchOp>(boundBlock, mlir::ValueRange{defaultArg, falseConstant});
                }

                implementBlock(boundBlock);
                args.push_back({boundBlock->getArgument(0), boundBlock->getArgument(1)});
                break;
            }
            case FunctionParameter::PosRest:
            case FunctionParameter::KeywordRest: args.push_back({argValue, {}}); break;
        }
    }
    return args;
}

mlir::func::FuncOp pylir::CodeGen::buildFunctionCC(llvm::Twine name, mlir::func::FuncOp implementation,
                                             const std::vector<FunctionParameter>& parameters)
{
    auto cc = mlir::func::FuncOp::create(
        m_builder.getCurrentLoc(), name.str(),
        mlir::FunctionType::get(m_builder.getContext(),
                                {m_builder.getDynamicType(), m_builder.getDynamicType(), m_builder.getDynamicType()},
                                {m_builder.getDynamicType()}));
    cc.setPrivate();
    auto reset = implementFunction(cc);

    auto closure = cc.getArgument(0);
    auto tuple = cc.getArgument(1);
    auto dict = cc.getArgument(2);

    auto functionType = m_builder.createFunctionRef();
    auto defaultTuple = m_builder.createGetSlot(closure, functionType, "__defaults__");
    auto kwDefaultDict = m_builder.createGetSlot(closure, functionType, "__kwdefaults__");

    auto unpacked = unpackArgsKeywords(
        tuple, dict, parameters,
        [&](std::size_t posIndex) -> mlir::Value
        {
            auto index = m_builder.create<mlir::arith::ConstantIndexOp>(posIndex);
            return m_builder.createTupleGetItem(defaultTuple, index);
        },
        [&](std::string_view keyword) -> mlir::Value
        {
            auto index = m_builder.createConstant(keyword);
            auto lookup = m_builder.createDictTryGetItem(kwDefaultDict, index);
            // TODO: __kwdefaults__ is writeable. This may not hold. I have no clue how and whether this
            // also
            //      affects __defaults__
            return lookup.getResult();
        });
    llvm::SmallVector<mlir::Value> args{closure};
    args.resize(1 + unpacked.size());
    std::transform(unpacked.begin(), unpacked.end(), args.begin() + 1,
                   [](const UnpackResults& unpackResults) { return unpackResults.parameterValue; });

    auto result = m_builder.create<Py::CallOp>(implementation, args);
    m_builder.create<mlir::func::ReturnOp>(result->getResults());
    return cc;
}

void pylir::CodeGen::executeFinallyBlocks(bool fullUnwind)
{
    // This whole sequence here is made quite complicated due to a few reasons:
    // try statements can be nested and they can execute ANY code. Including function returns.
    // If we were to simply execute all finally blocks in reverse this could easily lead to an infinite recursion in
    // the following case:
    //
    // try:
    //      ...
    // finally:
    //      return
    //
    // The return would lead us to executeFinallyBlocks here and it'd once again generate the finally that we are
    // currently executing. For that reason we are saving the current finally stack, pop one and generate that, and at
    // the end restore it for future statements.
    //
    // Further care needs to be taken for `raise` inside of finally:
    //
    // def foo():
    //    try: #1
    //        try: #2
    //            return
    //        finally:
    //            raise ValueError
    //    except ValueError:
    //        return "caught"
    //    finally:
    //        raise ValueError
    //
    // The finallies are basically executed as if outside the try block (even if we don't generate them as such)
    // which means exceptions raised within them are propagated upwards and not handled by their exception handler
    // but the enclosing one (if it exists)
    auto copy = m_finallyBlocks;
    auto reset = llvm::make_scope_exit([&] { m_finallyBlocks = std::move(copy); });

    for (auto iter = copy.rbegin();
         iter != copy.rend() && (fullUnwind || iter->parentLoop == m_currentLoop) && needsTerminator(); iter++)
    {
        auto exceptReset = pylir::valueResetMany(m_currentExceptBlock, m_currentExceptBlock);
        m_currentExceptBlock = iter->parentExceptBlock;
        m_finallyBlocks.pop_back();
        visit(*iter->finallySuite->suite);
    }
}

mlir::Value pylir::CodeGen::makeTuple(const std::vector<Py::IterArg>& args)
{
    if (!m_currentExceptBlock)
    {
        return m_builder.createMakeTuple(args);
    }
    if (std::all_of(args.begin(), args.end(),
                    [](const Py::IterArg& arg) { return std::holds_alternative<mlir::Value>(arg); }))
    {
        return m_builder.createMakeTuple(args);
    }
    return m_builder.createMakeTupleEx(args, m_currentExceptBlock);
}

mlir::Value pylir::CodeGen::makeList(const std::vector<Py::IterArg>& args)
{
    if (!m_currentExceptBlock)
    {
        return m_builder.createMakeList(args);
    }
    if (std::all_of(args.begin(), args.end(),
                    [](const Py::IterArg& arg) { return std::holds_alternative<mlir::Value>(arg); }))
    {
        return m_builder.createMakeList(args);
    }
    return m_builder.createMakeListEx(args, m_currentExceptBlock);
}

mlir::Value pylir::CodeGen::makeSet(const std::vector<Py::IterArg>& args)
{
    if (!m_currentExceptBlock)
    {
        return m_builder.createMakeSet(args);
    }
    if (std::all_of(args.begin(), args.end(),
                    [](const Py::IterArg& arg) { return std::holds_alternative<mlir::Value>(arg); }))
    {
        return m_builder.createMakeSet(args);
    }
    return m_builder.createMakeSetEx(args, m_currentExceptBlock);
}

mlir::Value pylir::CodeGen::makeDict(const std::vector<Py::DictArg>& args)
{
    if (!m_currentExceptBlock)
    {
        return m_builder.createMakeDict(args);
    }
    if (std::all_of(args.begin(), args.end(),
                    [](const Py::DictArg& arg)
                    { return std::holds_alternative<std::pair<mlir::Value, mlir::Value>>(arg); }))
    {
        return m_builder.createMakeDict(args);
    }
    return m_builder.createMakeDictEx(args, m_currentExceptBlock);
}

mlir::Value pylir::CodeGen::buildSubclassCheck(mlir::Value type, mlir::Value base)
{
    auto mro = m_builder.createTypeMRO(type);
    return m_builder.createTupleContains(mro, base);
}

void pylir::CodeGen::buildTupleForEach(mlir::Value tuple, mlir::Block* endBlock, mlir::ValueRange endArgs,
                                       llvm::function_ref<void(mlir::Value)> iterationCallback)
{
    auto tupleSize = m_builder.createTupleLen(tuple);
    auto startConstant = m_builder.create<mlir::arith::ConstantIndexOp>(0);
    auto conditionBlock = BlockPtr{};
    conditionBlock->addArgument(m_builder.getIndexType(), m_builder.getCurrentLoc());
    auto conditionBlockSeal = markOpenBlock(conditionBlock);
    m_builder.create<mlir::cf::BranchOp>(conditionBlock, mlir::ValueRange{startConstant});

    implementBlock(conditionBlock);
    auto isLess = m_builder.create<mlir::arith::CmpIOp>(mlir::arith::CmpIPredicate::ult, conditionBlock->getArgument(0),
                                                        tupleSize);
    auto body = BlockPtr{};
    m_builder.create<mlir::cf::CondBranchOp>(isLess, body, endBlock, endArgs);

    implementBlock(body);
    auto entry = m_builder.createTupleGetItem(tuple, conditionBlock->getArgument(0));
    iterationCallback(entry);
    PYLIR_ASSERT(needsTerminator());
    auto one = m_builder.create<mlir::arith::ConstantIndexOp>(1);
    auto nextIter = m_builder.create<mlir::arith::AddIOp>(conditionBlock->getArgument(0), one);
    m_builder.create<mlir::cf::BranchOp>(conditionBlock, mlir::ValueRange{nextIter});
}

void pylir::CodeGen::visit(const Syntax::ExpressionStmt& expressionStmt)
{
    visit(*expressionStmt.expression);
}

mlir::Value pylir::CodeGen::visit(const Syntax::Slice& slice)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

mlir::Value pylir::CodeGen::visit(const Syntax::Lambda& lambda)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

mlir::Value pylir::CodeGen::visit(const Syntax::AttributeRef& attributeRef)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

mlir::Value pylir::CodeGen::visit(const Syntax::Generator& generator)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::visit(const Syntax::AssertStmt& assertStmt)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::visit(const Syntax::DelStmt& delStmt)
{
    // TODO:
    PYLIR_UNREACHABLE;
}

void pylir::CodeGen::visit(const Syntax::ImportStmt& importStmt)
{
    // TODO:
    PYLIR_UNREACHABLE;
}
