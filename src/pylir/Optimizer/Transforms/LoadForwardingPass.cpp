#include <mlir/IR/Dominance.h>

#include <llvm/ADT/DepthFirstIterator.h>

#include <pylir/Optimizer/Analysis/MemorySSA.hpp>
#include <pylir/Optimizer/Interfaces/MemoryFoldInterface.hpp>

#include "PassDetail.hpp"
#include "Passes.hpp"

namespace
{

struct LoadForwardingPass : pylir::LoadForwardingBase<LoadForwardingPass>
{
protected:
    void runOnOperation() override;
};

void LoadForwardingPass::runOnOperation()
{
    if (getOperation().isDeclaration())
    {
        markAllAnalysesPreserved();
        return;
    }
    auto& memorySSA = getAnalysisManager().getAnalysis<pylir::MemorySSA>();
    auto& aliasAnalysis = getAnalysisManager().getAnalysis<mlir::AliasAnalysis>();
    bool changed = false;

    memorySSA.getMemoryRegion().walk(
        [&](pylir::MemSSA::MemoryUseOp use)
        {
            auto memoryFold = mlir::dyn_cast<pylir::MemoryFoldInterface>(use.instruction());
            if (!memoryFold)
            {
                return;
            }
            auto defOp = use.definition().getDefiningOp<pylir::MemSSA::MemoryDefOp>();
            if (!defOp)
            {
                return;
            }
            llvm::SmallVector<mlir::MemoryEffects::EffectInstance> effects;
            if (auto memOp = llvm::dyn_cast<mlir::MemoryEffectOpInterface>(defOp.instruction()))
            {
                memOp.getEffects(effects);
            }
            // Conservative. It was a mem def but we don't know why or how
            if (effects.empty())
            {
                return;
            }
            for (auto& iter : effects)
            {
                if (!llvm::isa<mlir::MemoryEffects::Write>(iter.getEffect()))
                {
                    continue;
                }
                if (!iter.getValue())
                {
                    return;
                }
                if (aliasAnalysis.alias(use.read(), iter.getValue()).isMay())
                {
                    return;
                }
            }

            llvm::SmallVector<mlir::OpFoldResult> results;
            if (mlir::failed(memoryFold.foldUsage(defOp.instruction(), results)))
            {
                return;
            }
            changed = true;
            for (auto [foldResult, opResult] : llvm::zip(results, memoryFold->getResults()))
            {
                if (auto value = foldResult.dyn_cast<mlir::Value>())
                {
                    opResult.replaceAllUsesWith(value);
                    m_localLoadsReplaced++;
                }
                else if (auto attr = foldResult.dyn_cast<mlir::Attribute>())
                {
                    mlir::OpBuilder builder(memoryFold);
                    auto constant = memoryFold->getDialect()->materializeConstant(builder, attr, opResult.getType(),
                                                                                  memoryFold->getLoc());
                    PYLIR_ASSERT(constant);
                    opResult.replaceAllUsesWith(constant->getResult(0));
                    m_localLoadsReplaced++;
                }
            }
            if (mlir::isOpTriviallyDead(memoryFold))
            {
                memoryFold->erase();
                use.erase();
            }
        });

    if (!changed)
    {
        markAllAnalysesPreserved();
        return;
    }
    markAnalysesPreserved<mlir::DominanceInfo, pylir::MemorySSA>();
}

} // namespace

std::unique_ptr<mlir::Pass> pylir::createLoadForwardingPass()
{
    return std::make_unique<LoadForwardingPass>();
}
