#include "ThinLtoJit.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Error.h>
#include <llvm/Transforms/IPO/FunctionImport.h>
#include <llvm/Transforms/Utils/FunctionImportUtils.h>

namespace llvm::thinltojit {

ThinLtoLazyOrcJitRuntime::ThinLtoLazyOrcJitRuntime() {
  Optimizer.configure = [](llvm::PassManagerBuilder &PMB) {
    // Inline all you can
    constexpr int inliningThreshold = 1 << 16;
    PMB.Inliner = llvm::createFunctionInliningPass(inliningThreshold);

    // Optimize reasonably
    PMB.OptLevel = 2;
    PMB.LoopVectorize = true;
    PMB.SLPVectorize = true;

    // Already did this in verifyLoadedModule()
    PMB.VerifyInput = false;
    PMB.VerifyOutput = false;
  };

  Jit.optimizeModule = [this](std::shared_ptr<Module> M) {
    return Optimizer.run(std::move(M), Jit.getTargetMachine());
  };
}

} // namespace llvm::thinltojit
