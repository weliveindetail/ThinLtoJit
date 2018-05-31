#pragma once

#include "llvm/Analysis/TargetTransformInfo.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
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

#include <functional>
#include <memory>
#include <vector>

namespace llvm::thinltojit {

// -----------------------------------------------------------------------------

class SymbolResolverImpl : public llvm::JITSymbolResolver {
public:
  SymbolResolverImpl();

  llvm::JITSymbol findSymbolInLogicalDylib(const std::string &Name) override;
  llvm::JITSymbol findSymbol(const std::string &Name) override;

  using GlobalSymbolProvider_f =
      std::function<llvm::JITTargetAddress(std::string)>;

  using ModuleSymbolProvider_f = std::function<llvm::JITSymbol(std::string)>;

  GlobalSymbolProvider_f resolveExternalSymbol;
  ModuleSymbolProvider_f resolveSymbolInModule;
};

// -----------------------------------------------------------------------------

class OptimizerImpl {
public:
  using ModulePtr_t = std::shared_ptr<llvm::Module>;

  ModulePtr_t run(ModulePtr_t M, llvm::TargetMachine &TM);
  std::function<void(llvm::PassManagerBuilder &)> configure;
};

// -----------------------------------------------------------------------------

class OrcJitImpl {
public:
  OrcJitImpl();

  using IRCompiler_t = llvm::orc::SimpleCompiler;
  using IRTransform_f = std::function<std::shared_ptr<llvm::Module>(
      std::shared_ptr<llvm::Module>)>;

  using LObject_t = llvm::orc::RTDyldObjectLinkingLayer;
  using LCompile_t = llvm::orc::IRCompileLayer<LObject_t, IRCompiler_t>;
  using LOptimize_t = llvm::orc::IRTransformLayer<LCompile_t, IRTransform_f>;

  using ModuleHandle_t = LOptimize_t::ModuleHandleT;

  llvm::Expected<OrcJitImpl::ModuleHandle_t>
  addModule(std::unique_ptr<llvm::Module> M,
            SymbolResolverImpl::GlobalSymbolProvider_f P);

  llvm::TargetMachine &getTargetMachine() { return *TM; }

  using ModulePtr_t = std::shared_ptr<llvm::Module>;
  std::function<ModulePtr_t(ModulePtr_t)> optimizeModule;

private:
  llvm::TargetMachine *TM;
  llvm::DataLayout DL;

  std::vector<ModuleHandle_t> ModuleHandles;
  std::shared_ptr<llvm::RTDyldMemoryManager> MemoryManagerPtr;

  LObject_t ObjectLayer;
  LCompile_t CompileLayer;
  LOptimize_t OptimizeLayer;
};

} // namespace llvm::thinltojit
