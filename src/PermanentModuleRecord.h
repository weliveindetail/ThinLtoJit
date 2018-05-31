#pragma once

#include "SimpleOrcJit.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/LTO/LTO.h>
#include <llvm/LTO/legacy/ThinLTOCodeGenerator.h>
#include <llvm/Object/IRObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

namespace llvm::thinltojit {

class FunctionRecord;
class PermanentModuleRecord;

class ModuleRevisionRecord {
public:
  using ModuleHandle_t = OrcJitImpl::ModuleHandle_t;
  using GUIDs_t = SetVector<GlobalValue::GUID>;
  enum States { Open, Finalized };

  void filter(const ModuleRevisionRecord &Other,
              std::function<bool(GlobalValue::GUID)> Filter);

  void commitImports(FunctionImporter::ImportMapTy NewImports) {
    assert(State != Finalized);

    if (Imports.empty()) {
      Imports = std::move(NewImports);
    } else {
      for (auto &FileEntry : NewImports) {
        std::map<GlobalValue::GUID, unsigned> &NewEntries = FileEntry.second;
        Imports[FileEntry.first()].insert(NewEntries.begin(), NewEntries.end());
      }
    }
  }

  void commitCode(ModuleHandle_t NewCompiledCode) {
    assert(State != Finalized);
    CompiledCode = std::move(NewCompiledCode);
    State = Finalized;
  }

  void finalizeUnused() { State = Finalized; }

  void addDependent(PermanentModuleRecord *ModuleRecord) {
    Dependents.push_back(ModuleRecord);
  }

  bool isFinalized() const { return State == Finalized; }

  States State = Open;
  ModuleHandle_t CompiledCode;
  FunctionImporter::ImportMapTy Imports;

  GUIDs_t AllGuids;
  GUIDs_t PreservedGlobals;
  StringMap<std::shared_ptr<FunctionRecord>> Functions;
  std::vector<PermanentModuleRecord *> Dependents;
};

class PermanentModuleRecord {
public:
  using ModuleHandle_t = OrcJitImpl::ModuleHandle_t;
  using GUIDs_t = SetVector<GlobalValue::GUID>;

  PermanentModuleRecord(std::string ModuleId) : ModuleId(std::move(ModuleId)) {}

  Expected<JITTargetAddress> findSymbol(const std::string &Name,
                                        bool SkipLatest = false);

  std::shared_ptr<FunctionRecord> getFunction(StringRef Name) const {
    return Revisions.back().Functions.lookup(Name);
  }

  const FunctionImporter::ImportMapTy &getImportMap() {
    assert(Revisions.size() > 0);
    return Revisions.back().Imports;
  }

  const std::string &getModuleId() const { return ModuleId; }

  const std::vector<PermanentModuleRecord *> *
  openRevision(const GUIDs_t &Inserts, const GUIDs_t &Removals);
  void openRevisionForRebind();

  bool hasOpenRevision() const {
    return !(Revisions.empty() || Revisions.back().isFinalized());
  }

  GUIDs_t collectPreservedGuids() const;

  std::shared_ptr<FunctionRecord> addFunction(StringRef Name);

  void addDependent(PermanentModuleRecord *ModuleRecord) {
    Revisions.back().addDependent(ModuleRecord);
  }

  void commitImports(FunctionImporter::ImportMapTy NewImports) {
    Revisions.back().commitImports(std::move(NewImports));
  }

  void commitCode(ModuleHandle_t Handle) {
    Revisions.back().commitCode(std::move(Handle));
  }

  // If the Guid is not existing yet, this opens a new Revision!
  void registerGlobalValue(GlobalValue::GUID Guid);

  bool canResolveFromPreviousRevision(const std::string &GlobalVariableName) {
    bool SkipLatestRevision =
        Revisions.size() > 0 && !Revisions.back().isFinalized();

    auto Addr = findSymbol(GlobalVariableName, SkipLatestRevision);

    if (Error Err = Addr.takeError()) {
      consumeError(std::move(Err));
      return false;
    }

    return (*Addr != 0);
  }

  bool canResolveFromPreviousRevision(GlobalValue::GUID Guid) const {
    auto It = Revisions.rbegin();
    if (It != Revisions.rend() && !It->isFinalized())
      It++; // use previous finalized revision

    if (It != Revisions.rend() && It->isFinalized())
      return It->AllGuids.count(Guid) > 0;

    return false;
  }

private:
  std::string ModuleId;
  std::vector<ModuleRevisionRecord> Revisions;
};

// -----------------------------------------------------------------------------

class FunctionRecord {
public:
  FunctionRecord(std::string Name, PermanentModuleRecord *ModuleRecord)
      : OwningModuleRecord(ModuleRecord), Name(std::move(Name)) {}

  StringRef getName() const { return Name; }

  Expected<JITTargetAddress> getAddress() {
    if (CachedAddr)
      return CachedAddr;

    auto Addr = OwningModuleRecord->findSymbol(Name);
    if (Addr) {
      CachedAddr = *Addr;
      return Addr;
    }

    return std::move(Addr.takeError());
  }

  void invalidate() {
    CachedAddr = 0; // On next call new address must be resolved
  }

  void outdated() { assert(false && "TODO: Signature changed, etc."); }

  bool isCompatible(StringRef Name) {
    return true; // for now everything is compatible
  }

private:
  llvm::JITTargetAddress CachedAddr = 0;
  PermanentModuleRecord *OwningModuleRecord = nullptr;
  std::string Name;
};

// -----------------------------------------------------------------------------

inline std::shared_ptr<FunctionRecord>
PermanentModuleRecord::addFunction(StringRef Name) {
  assert(!Revisions.back().isFinalized());

  auto &Functions = Revisions.back().Functions;
  std::shared_ptr<FunctionRecord> Function;

  auto It = Functions.find(Name);
  if (It != Functions.end()) {
    Function = It->second;
    if (Function->isCompatible(Name)) {
      Function->invalidate(); // rebind on next call
    } else {
      Function->outdated(); // shared owners may still call it
      Functions.erase(It);
    }
  } else {
    Function = std::make_shared<FunctionRecord>(Name, this);
    Functions.insert(std::make_pair(Name, Function));
  }

  return Function;
}

inline void
ModuleRevisionRecord::filter(const ModuleRevisionRecord &Other,
                             std::function<bool(GlobalValue::GUID)> Filter) {
  // Copy functions from previous revision (except removed)
  for (auto &Entry : Other.Functions) {
    auto &FunctionRecord = Entry.second;
    auto Guid = GlobalValue::getGUID(FunctionRecord->getName());

    if (!Filter(Guid)) {
      FunctionRecord->invalidate(); // resolve symbol on next call
      Functions.insert(std::make_pair(Entry.first(), Entry.second));
    }
  }

  // Copy preserved globals from previous revision (except removed)
  for (GlobalValue::GUID Guid : Other.PreservedGlobals) {
    if (!Filter(Guid)) {
      PreservedGlobals.insert(Guid);
    }
  }

  // Copy all involved GUIDs from previous revision (except removed)
  for (GlobalValue::GUID Guid : Other.AllGuids) {
    if (!Filter(Guid)) {
      AllGuids.insert(Guid);
    }
  }

  for (PermanentModuleRecord *D : Other.Dependents) {
    addDependent(D);
  }
}

inline const std::vector<PermanentModuleRecord *> *
PermanentModuleRecord::openRevision(const GUIDs_t &Inserts,
                                    const GUIDs_t &Removals) {
  if (Revisions.empty()) {
    assert(Removals.empty());
    Revisions.emplace_back();
  } else {
    auto removed = [&Removals](GlobalValue::GUID Guid) {
      auto It = std::find(Removals.begin(), Removals.end(), Guid);
      return It != Removals.end();
    };

    ModuleRevisionRecord &Latest = Revisions.back();
    if (Latest.isFinalized()) {
      ModuleRevisionRecord NewRevision;
      NewRevision.filter(Latest, removed);
      Revisions.push_back(NewRevision);
    } else {
      Latest.filter(Latest, removed);
    }
  }

  // Append newly added GUIDs (not sure if we actually need them).
  Revisions.back().AllGuids.insert(Inserts.begin(), Inserts.end());

  // We currently take-over all dependents.
  return &Revisions.back().Dependents;
}

inline void PermanentModuleRecord::openRevisionForRebind() {
  assert(Revisions.back().isFinalized());

  GUIDs_t NoGuids{};
  openRevision(NoGuids, NoGuids);

  assert(!Revisions.back().isFinalized());
}

inline SetVector<GlobalValue::GUID>
PermanentModuleRecord::collectPreservedGuids() const {
  if (Revisions.empty())
    return {};

  // Copy GUIDs for preserved global variables.
  auto Guids = Revisions.back().PreservedGlobals;

  // Names of all existing functions must be preserved too.
  for (const auto &F : Revisions.back().Functions) {
    Guids.insert(GlobalValue::getGUID(F.second->getName()));
  }

  return Guids;
}

inline Expected<JITTargetAddress>
PermanentModuleRecord::findSymbol(const std::string &Name, bool SkipLatest) {
  auto latest = Revisions.rbegin();
  auto end = Revisions.rend();

  if (SkipLatest)
    ++latest;

  // When an error is encoutered, queue it and continue.
  Error Errs = Error::success();
  auto QueueErr = [&Errs](Error Err) {
    Errs = joinErrors(std::move(Errs), std::move(Err));
  };

  for (auto it = latest; it != end; ++it) {
    JITSymbol Symbol = (*it->CompiledCode)->getSymbol(Name, false);
    if (Error Err = Symbol.takeError()) {
      QueueErr(std::move(Err));
      continue;
    }

    if (!Symbol)
      continue; // Search in older revision.

    auto Addr = Symbol.getAddress();
    if (Error Err = Addr.takeError()) {
      QueueErr(std::move(Err));
      continue;
    }

    // Ignore errors in success case.
    llvm::consumeError(std::move(Errs));
    return *Addr;
  }

  if (Errs)
    return std::move(Errs);

  return JITTargetAddress{};
}

} // namespace llvm::thinltojit
