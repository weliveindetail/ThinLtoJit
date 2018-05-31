#pragma once

#include <memory>
#include <string>

#include <llvm/ADT/SmallString.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/ModuleSummaryIndex.h>
#include <llvm/Object/IRObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA1.h>

namespace llvm::thinltojit {

// -----------------------------------------------------------------------------

// Never convert to std::error_code
template <typename ThisErrT> class ErrorBase : public ErrorInfo<ThisErrT> {
public:
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }
};

// -----------------------------------------------------------------------------

class CantCompileFunctionError : public ErrorBase<CantCompileFunctionError> {
public:
  static char ID;
  const std::string Name;
  CantCompileFunctionError(std::string Name) : Name(std::move(Name)) {}

  void log(llvm::raw_ostream &os) const override {
    os << "Failed to compile function " << Name;
  }
};

// -----------------------------------------------------------------------------

class MissingModuleError : public ErrorBase<MissingModuleError> {
public:
  static char ID;
  const GlobalValue::GUID Guid;
  MissingModuleError(GlobalValue::GUID Guid) : Guid(Guid) {}

  void log(llvm::raw_ostream &os) const override {
    os << "Failed to find module for function GUID: " << Guid << "\n";
  }
};

// -----------------------------------------------------------------------------

class CrossImportError : public ErrorBase<CrossImportError> {
public:
  static char ID;
  const std::string Msg;
  CrossImportError(std::string Msg) : Msg(std::move(Msg)) {}

  void log(llvm::raw_ostream &os) const override {
    os << "Failed to verfy Module after CrossImport: " << Msg;
  }
};

// -----------------------------------------------------------------------------

class InvocationFailedError : public ErrorBase<InvocationFailedError> {
public:
  static char ID;
  const std::string Message;
  InvocationFailedError(std::string Msg) : Message(std::move(Msg)) {}

  void log(llvm::raw_ostream &os) const override {
    os << "Failed to invoke function: " << Message;
  }
};

} // namespace llvm::thinltojit
