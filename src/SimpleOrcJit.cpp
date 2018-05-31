#include "SimpleOrcJit.h"

#include "llvm/Analysis/TargetTransformInfo.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/IRCompileLayer.h>
#include <llvm/ExecutionEngine/Orc/IRTransformLayer.h>
#include <llvm/ExecutionEngine/Orc/IndirectionUtils.h>
#include <llvm/ExecutionEngine/Orc/LambdaResolver.h>
#include <llvm/ExecutionEngine/Orc/OrcError.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/Mangler.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/FunctionImportUtils.h>

#define DEBUG_TYPE "jitfromscratch"

using namespace llvm;
using namespace llvm::orc;

namespace llvm::thinltojit {

SymbolResolverImpl::SymbolResolverImpl() {
  // Load own executable as dynamic library.
  // Required for RTDyldMemoryManager::getSymbolAddressInProcess().
  sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
}

JITSymbol SymbolResolverImpl::findSymbolInLogicalDylib(const std::string &N) {
  return (resolveSymbolInModule) ? resolveSymbolInModule(N) : nullptr;
}

JITSymbol SymbolResolverImpl::findSymbol(const std::string &N) {
  if (resolveExternalSymbol)
    if (llvm::JITTargetAddress addr = resolveExternalSymbol(N))
      return llvm::JITSymbol(addr, JITSymbolFlags::Exported);

  if (auto addr = RTDyldMemoryManager::getSymbolAddressInProcess(N))
    return llvm::JITSymbol(addr, JITSymbolFlags::Exported);

  return nullptr;
}

OptimizerImpl::ModulePtr_t OptimizerImpl::run(ModulePtr_t M,
                                              llvm::TargetMachine &TM) {
  // Populate the PassManager
  PassManagerBuilder PMBuilder;
  PMBuilder.LibraryInfo = new TargetLibraryInfoImpl(TM.getTargetTriple());

  // ?
  PMBuilder.PerformThinLTO = true;

  if (configure)
    configure(PMBuilder);

  legacy::PassManager PM;

  // Register size and other TTI
  PM.add(createTargetTransformInfoWrapperPass(TM.getTargetIRAnalysis()));

  PMBuilder.populateThinLTOPassManager(PM);
  PM.run(*M);

  DEBUG({
    outs() << "Optimized module:\n\n";
    outs() << *M << "\n\n";
  });

  return M;
}

OrcJitImpl::OrcJitImpl()
    : TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
      MemoryManagerPtr(std::make_shared<SectionMemoryManager>()),
      ObjectLayer([this]() { return MemoryManagerPtr; }),
      CompileLayer(ObjectLayer, IRCompiler_t(*TM)),
      OptimizeLayer(CompileLayer, [this](std::shared_ptr<llvm::Module> M) {
        return (optimizeModule) ? optimizeModule(std::move(M)) : M;
      }) {}

Expected<OrcJitImpl::ModuleHandle_t>
OrcJitImpl::addModule(std::unique_ptr<Module> M,
                      SymbolResolverImpl::GlobalSymbolProvider_f P) {
  auto R = std::make_shared<SymbolResolverImpl>();

  DEBUG({
    dbgs() << "\n\n" << *M << "\n\n";
    dbgs().flush();
  });

  auto H = OptimizeLayer.addModule(std::move(M), R);
  if (!H)
    return H.takeError();

  R->resolveExternalSymbol = std::move(P);
  R->resolveSymbolInModule = [Handle = *H](const std::string &N) {
    return (*Handle)->getSymbol(N, false);
  };

  ModuleHandles.push_back(*H);
  return ModuleHandles.back();
}

} // namespace llvm::thinltojit
