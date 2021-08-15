
#include "LLVMPostProcess.hpp"

#include <llvm/IR/IRBuilder.h>

void pylir::postProcessLLVMModule(llvm::Module& module)
{
    // __main__.__init__ in the future
    auto aliasee = module.getFunction("__init__");
    // Normal name so it can be called from C
    llvm::GlobalAlias::create("pylir__main____init__", aliasee);

    // Apply comdat to all functions in the ctor array
    auto* ctors = module.getGlobalVariable("llvm.global_ctors");
    if (!ctors)
    {
        return;
    }
    auto array = llvm::dyn_cast<llvm::ConstantArray>(ctors->getInitializer());
    if (!array)
    {
        return;
    }
    llvm::Constant* element;
    for (std::size_t i = 0; (element = array->getAggregateElement(i)); i++)
    {
        if (!llvm::isa<llvm::ConstantStruct>(element))
        {
            continue;
        }
        auto* function = llvm::dyn_cast_or_null<llvm::Function>(element->getAggregateElement(1));
        if (!function)
        {
            continue;
        }
        auto* comdat = module.getOrInsertComdat(function->getName());
        comdat->setSelectionKind(llvm::Comdat::Any);
    }
}