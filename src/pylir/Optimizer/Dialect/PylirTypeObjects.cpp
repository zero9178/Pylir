#include "PylirTypeObjects.hpp"

#include <mlir/Dialect/StandardOps/IR/Ops.h>

#include <pylir/Support/Macros.hpp>

namespace
{
template <class F>
pylir::Dialect::ConstantGlobalOp getConstant(::pylir::Dialect::ObjectType type, mlir::ModuleOp& module,
                                             std::string_view name, F fillDict)
{
    static_assert(std::is_invocable_r_v<std::vector<std::pair<mlir::Attribute, mlir::Attribute>>, F>);
    auto symbolTable = mlir::SymbolTable(module);
    if (auto symbol = symbolTable.lookup<pylir::Dialect::ConstantGlobalOp>(name))
    {
        return symbol;
    }
    auto globalOp =
        pylir::Dialect::ConstantGlobalOp::create(mlir::UnknownLoc::get(module.getContext()), name, type, {});
    symbolTable.insert(globalOp);
    // Insert first, then generate initializer to stop recursion
    auto dict = fillDict();
    globalOp.initializerAttr(pylir::Dialect::DictAttr::get(module.getContext(), dict));
    return globalOp;
}

template <class F>
mlir::FlatSymbolRefAttr genFunction(mlir::ModuleOp& module, mlir::FunctionType signature, std::string_view name,
                                    F genBody)
{
    auto func = mlir::FuncOp::create(mlir::UnknownLoc::get(module.getContext()), name, signature);
    func->setAttr("linkonce", mlir::UnitAttr::get(module.getContext()));
    module.push_back(func);
    mlir::OpBuilder builder(module.getContext());
    builder.setInsertionPointToStart(func.addEntryBlock());
    genBody(builder, func);
    return mlir::FlatSymbolRefAttr::get(module.getContext(), name);
}
} // namespace

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getTypeTypeObject(mlir::ModuleOp& module)
{
    return getConstant(ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), typeTypeObjectName)), module,
                       typeTypeObjectName,
                       [&]()
                       {
                           std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;

                           return dict;
                       });
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getFunctionTypeObject(mlir::ModuleOp& module)
{
    return getConstant(
        ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), getTypeTypeObject(module).sym_name())),
        module, functionTypeObjectName,
        [&]()
        {
            std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;
            dict.emplace_back(
                mlir::StringAttr::get(module.getContext(), "__call__"),
                genFunction(
                    module,
                    pylir::Dialect::GetTypeSlotOp::returnTypeFromPredicate(module.getContext(), TypeSlotPredicate::Call)
                        .cast<mlir::FunctionType>(),
                    "__builtins__.function.__call__",
                    [&](mlir::OpBuilder& builder, mlir::FuncOp funcOp)
                    {
                        mlir::Value self = funcOp.getArgument(0);
                        mlir::Value args = funcOp.getArgument(1);
                        mlir::Value dict = funcOp.getArgument(2);
                        auto unboxed = builder.create<Dialect::UnboxOp>(builder.getUnknownLoc(),
                                                                        getCCFuncType(module.getContext()), self);
                        auto result = builder.create<mlir::CallIndirectOp>(builder.getUnknownLoc(), unboxed,
                                                                           mlir::ValueRange{self, args, dict});
                        builder.create<mlir::ReturnOp>(builder.getUnknownLoc(), result.getResult(0));
                    }));
            return dict;
        });
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getIntTypeObject(mlir::ModuleOp& module)
{
    return getConstant(
        ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), getTypeTypeObject(module).sym_name())),
        module, intTypeObjectName,
        [&]()
        {
            std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;
            dict.emplace_back(
                mlir::StringAttr::get(module.getContext(), "__mul__"),
                genFunction(
                    module,
                    pylir::Dialect::GetTypeSlotOp::returnTypeFromPredicate(module.getContext(),
                                                                           TypeSlotPredicate::Multiply)
                        .cast<mlir::FunctionType>(),
                    "__builtins__.int.__mul__",
                    [&](mlir::OpBuilder& builder, mlir::FuncOp funcOp)
                    {
                        auto lhs = funcOp.getArgument(0);
                        auto rhs = funcOp.getArgument(1);
                        // TODO type check
                        auto lhsValue = builder.create<Dialect::UnboxOp>(builder.getUnknownLoc(),
                                                                         builder.getType<Dialect::IntegerType>(), lhs);
                        auto rhsValue = builder.create<Dialect::UnboxOp>(builder.getUnknownLoc(),
                                                                         builder.getType<Dialect::IntegerType>(), rhs);
                        auto result = builder.create<Dialect::IMulOp>(builder.getUnknownLoc(), lhsValue, rhsValue);
                        auto box = builder.create<Dialect::BoxOp>(builder.getUnknownLoc(),
                                                                  ObjectType::get(getIntTypeObject(module)), result);
                        auto gcAlloc = builder.create<Dialect::GCAllocOp>(
                            builder.getUnknownLoc(),
                            Dialect::PointerType::get(ObjectType::get(getIntTypeObject(module))), mlir::Value{});
                        builder.create<pylir::Dialect::StoreOp>(builder.getUnknownLoc(), box, gcAlloc);
                        builder.create<Dialect::ReturnOp>(builder.getUnknownLoc(), mlir::ValueRange{gcAlloc});
                    }));
            return dict;
        });
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getNoneTypeObject(mlir::ModuleOp& module)
{
    return getConstant(
        ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), getTypeTypeObject(module).sym_name())),
        module, "__builtins__.None",
        [&]()
        {
            std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;

            return dict;
        });
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getNoneObject(mlir::ModuleOp& module)
{
    return getConstant(
        ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), getNoneTypeObject(module).sym_name())),
        module, noneTypeObjectName,
        [&]()
        {
            std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;

            return dict;
        });
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getNotImplementedTypeObject(mlir::ModuleOp& module)
{
    return getConstant(
        ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), getTypeTypeObject(module).sym_name())),
        module, notImplementedTypeObjectName,
        [&]()
        {
            std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;

            return dict;
        });
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getNotImplementedObject(mlir::ModuleOp& module)
{
    return getConstant(ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(),
                                                                    getNotImplementedTypeObject(module).sym_name())),
                       module, "__builtins__.NotImplemented",
                       [&]()
                       {
                           std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;

                           return dict;
                       });
}

mlir::FunctionType pylir::Dialect::getCCFuncType(mlir::MLIRContext* context)
{
    auto ref = Dialect::PointerType::get(ObjectType::get(context));
    return mlir::FunctionType::get(context,
                                   {ref,
                                    Dialect::PointerType::get(Dialect::ObjectType::get(
                                        mlir::FlatSymbolRefAttr::get(context, tupleTypeObjectName))),
                                    Dialect::PointerType::get(Dialect::DictType::get(context))},
                                   {ref});
}

pylir::Dialect::ConstantGlobalOp pylir::Dialect::getTupleTypeObject(mlir::ModuleOp& module)
{
    return getConstant(
        ObjectType::get(mlir::FlatSymbolRefAttr::get(module.getContext(), getTypeTypeObject(module).sym_name())),
        module, tupleTypeObjectName,
        [&]()
        {
            std::vector<std::pair<mlir::Attribute, mlir::Attribute>> dict;

            return dict;
        });
}
