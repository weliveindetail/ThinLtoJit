#include "BitcodeCacheEntry.h"
#include "Errors.h"
#include "PermanentModuleRecord.h"
#include "SimpleOrcJit.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/LTO/LTO.h>
#include <llvm/LTO/legacy/ThinLTOCodeGenerator.h>
#include <llvm/Object/IRObjectFile.h>
#include <llvm/Support/MemoryBuffer.h>

#define DEBUG_TYPE "jitfromscratch"

namespace llvm::thinltojit {

// -----------------------------------------------------------------------------

template <class Return_t, class... Args_tt> class TypedFunctionHandle;

class FunctionHandle {
public:
  FunctionHandle() = default;
  FunctionHandle(std::shared_ptr<FunctionRecord> ManagedFunction)
      : ManagedFunction(std::move(ManagedFunction)) {}

  template <class Return_t, class... Args_tt>
  Return_t call(Args_tt &&... args) {
    using Signature = Return_t(Args_tt...);

    if (!ManagedFunction)
      return make_error<InvocationFailedError>("Missing FunctionRecord");

    auto Addr = ManagedFunction->getAddress();
    if (!Addr)
      return Addr.takeError();

    auto TypedPtr = (Signature *)*Addr;
    return TypedPtr(std::forward<Args_tt>(args)...);
  }

  template <class Return_t, class... Args_tt>
  TypedFunctionHandle<Return_t, Args_tt...> getTypedHandle() const;

private:
  std::shared_ptr<FunctionRecord> ManagedFunction;
};

// -----------------------------------------------------------------------------

template <class Return_t, class... Args_tt> class TypedFunctionHandle {
public:
  using Signature = Return_t(Args_tt...);

  TypedFunctionHandle() = default;
  TypedFunctionHandle(std::shared_ptr<FunctionRecord> ManagedFunction)
      : ManagedFunction(std::move(ManagedFunction)) {}

  Expected<Return_t> call(Args_tt &&... args) {
    if (!ManagedFunction)
      return make_error<InvocationFailedError>("Missing FunctionRecord");

    auto Addr = ManagedFunction->getAddress();
    if (!Addr)
      return Addr.takeError();

    auto TypedPtr = (Signature *)*Addr;
    return TypedPtr(std::forward<Args_tt>(args)...);
  }

  FunctionHandle getTypeErasedHandle() const {
    return FunctionHandle(ManagedFunction);
  }

private:
  std::shared_ptr<FunctionRecord> ManagedFunction;
};

template <class Return_t, class... Args_tt>
inline TypedFunctionHandle<Return_t, Args_tt...>
FunctionHandle::getTypedHandle() const {
  return TypedFunctionHandle<Return_t, Args_tt...>(ManagedFunction);
}

// -----------------------------------------------------------------------------

class ThinLtoLazyOrcJitRuntime {
  OrcJitImpl Jit;
  OptimizerImpl Optimizer;

  llvm::LLVMContext Context;
  llvm::StringSet<> GlobalPreservedSymbols;

  std::unique_ptr<ModuleSummaryIndex> CachedGlobalIndex =
      std::make_unique<ModuleSummaryIndex>();

  std::vector<std::string> BitcodeFileNames;
  llvm::StringMap<BitcodeCacheEntry> BitcodeCache;

  llvm::StringMap<PermanentModuleRecord> ModuleRecords;
  llvm::StringMap<std::shared_ptr<FunctionRecord>> Functions;

  llvm::StringMap<PermanentModuleRecord *> ModuleRecordsByHash;

public:
  using ModuleHandle_t = OrcJitImpl::ModuleHandle_t;
  using ImportMap_t = FunctionImporter::ImportMapTy;
  using GUIDs_t = SetVector<GlobalValue::GUID>;

  std::function<void(Error)> ErrorHandler;

  ThinLtoLazyOrcJitRuntime();

  void reportOrDrop(Error Err) {
    if (ErrorHandler) {
      ErrorHandler(std::move(Err));
    } else {
      llvm::consumeError(std::move(Err));
    }
  }

  bool addBitcodeFile(std::string BitcodeFileName) {
    // BitcodeFileName will become the module ID.
    auto [_, New] = BitcodeCache.try_emplace(BitcodeFileName);
    return New;
  }

  Error updateModuleCacheFromBitcodeFiles() {
    using namespace llvm;

    // When an error is encoutered, queue it and continue.
    Error Errs = Error::success();
    auto QueueErr = [&Errs](Error Err) {
      Errs = joinErrors(std::move(Errs), std::move(Err));
    };

    for (auto &Entry : BitcodeCache) {
      StringRef BitcodeFileName = Entry.first();
      BitcodeCacheEntry &CacheEntry = Entry.second;

      // bool IsNew = CacheEntry.getBuffer().empty();
      // bool WasCorrupt = CacheEntry.UpdateError;

      // Can access file contents?
      auto BufferOrErr = MemoryBuffer::getFile(BitcodeFileName);
      if (!BufferOrErr) {
        QueueErr(errorCodeToError(BufferOrErr.getError()));
        CacheEntry.UpdateError = true;
        continue;
      }

      // Update cached data.
      if (Error Err = CacheEntry.update(std::move(*BufferOrErr))) {
        QueueErr(std::move(Err));
        CacheEntry.UpdateError = true;
        continue;
      }

      CacheEntry.UpdateError = false;
    }

    return Errs;
  }

  StringRef findModuleIdFor(uint64_t FuncGuid) {
    for (const auto &Entry : BitcodeCache) {
      if (!Entry.second.UpdateError) {
        const BitcodeCacheEntry &CacheEntry = Entry.second;
        if (ValueInfo info = CacheEntry.ModuleIndex->getValueInfo(FuncGuid))
          return Entry.first();
      }
    }
    return StringRef{};
  }

  Expected<std::unique_ptr<ModuleSummaryIndex>> linkCombinedIndex() {
    auto CombinedIndex = std::make_unique<ModuleSummaryIndex>();
    uint64_t ModuleIdCount = 0;

    for (const auto &Entry : BitcodeCache) {
      if (!Entry.second.UpdateError) {
        Error ReadSummaryErr = llvm::readModuleSummaryIndex(
            Entry.second.getBuffer(), *CombinedIndex, ModuleIdCount++);
        if (ReadSummaryErr)
          return std::move(ReadSummaryErr);
      }
    }

    return CombinedIndex;
  }

  static GlobalValueSummary *getSummary(const GlobalValueSummaryInfo &Info) {
    if (Info.SummaryList.empty())
      return nullptr;

    assert(Info.SummaryList.size() == 1 &&
           "Multiple summaries not supported for now");
    return Info.SummaryList.front().get();
  }

  Expected<std::unique_ptr<Module>>
  linkCombinedModule(std::unique_ptr<Module> MainModule,
                     PermanentModuleRecord *ModuleRecord) {
    // Function import will call back when it needs another module. We can
    // provide a lazy one, it will be parsed on demand. TODO: Caching?
    FunctionImporter Importer(*CachedGlobalIndex, [this](StringRef ModuleId) {
      constexpr bool IsImporting = true;
      constexpr bool ShouldLazyLoadMetadata = true;
      BitcodeCacheEntry &CachedModule = BitcodeCache[ModuleId];
      return llvm::getLazyBitcodeModule(CachedModule.getBuffer(), Context,
                                        ShouldLazyLoadMetadata, IsImporting);
    });

    const auto &Imports = ModuleRecord->getImportMap();

    // Trigger cross-module import and verify the result.
    auto Res = Importer.importFunctions(*MainModule, Imports);
    if (!Res)
      return Res.takeError();

    std::string ErrMsg;
    raw_string_ostream ErrMsgStream(ErrMsg);
    if (llvm::verifyModule(*MainModule, &ErrMsgStream))
      return make_error<CrossImportError>(ErrMsgStream.str());

    // DEBUG(dbgs() << "After CrossModule Import:\n\n" << M << "\n\n");
    return MainModule;
  }

  void adjustLinkageOfPreservedFunctions(
      const DenseSet<GlobalValue::GUID> &GlobalPreservedGuids) {
    for (auto &Entry : *CachedGlobalIndex) {
      if (auto *Summary = getSummary(Entry.second)) {
        if (Summary->getSummaryKind() == GlobalValueSummary::FunctionKind) {
          bool Preserve = GlobalPreservedGuids.count(Entry.first) > 0;
          if (Preserve) {
            if (GlobalValue::isLocalLinkage(Summary->linkage()))
              Summary->setLinkage(GlobalValue::ExternalLinkage);
          } else {
            // Internalize all others
            if (!GlobalValue::isLocalLinkage(Summary->linkage()))
              Summary->setLinkage(GlobalValue::InternalLinkage);
          }
        }
      }
    }
  }

  std::string getModuleHashStrFromIndex(PermanentModuleRecord *M) const {
    // Same as ModuleSummaryIndex::getGlobalNameForLocal()
    auto ModHash = CachedGlobalIndex->getModuleHash(M->getModuleId());
    return utostr((uint64_t(ModHash[0]) << 32) | ModHash[1]);
  }

  void deleteOwnExistingDefinitions(Module &M,
                                    PermanentModuleRecord *ModuleRecord) {
    unsigned NumDeletedDefs = 0;
    auto ModHash = getModuleHashStrFromIndex(ModuleRecord);

    for (GlobalVariable &GV : M.globals()) {
      // Manual promotion of global variables: delete the definition and rename
      // manually, if it exists in a previous revision already.
      auto [UnpromotedName, ModHashStr] = GV.getName().split(".llvm.");
      if (ModHashStr.empty()) {
        if (ModuleRecord->canResolveFromPreviousRevision(UnpromotedName)) {
          DEBUG(dbgs() << "\n  " << GV.getGUID() << " = " << GV.getName());

          GV.setName(UnpromotedName + ".llvm." + ModHash);
          GV.setLinkage(GlobalValue::ExternalLinkage);
          GV.setInitializer(nullptr);
          GV.clearMetadata();
        }
      }
    }

    if (NumDeletedDefs > 0) {
      ModuleRecordsByHash[ModHash] = ModuleRecord;
      DEBUG(dbgs() << "\nTurned " << NumDeletedDefs
                   << " existing global variable definitions from "
                   << M.getName() << " into declarations");
    }
  }

  // Adds name of local constant (like strings) to the symbol table.
  void makePrivateVariablesInternal(Module &M) {
    for (GlobalVariable &GV : M.globals()) {
      if (GV.getLinkage() == GlobalValue::PrivateLinkage) {
        GV.setLinkage(GlobalValue::InternalLinkage);
        GV.setVisibility(GlobalValue::DefaultVisibility);
      }
    }
  }

  bool dependsOnDirtyBitcodeSource(StringRef OwnModuleId,
                                   const ImportMap_t &Imports) {
    if (BitcodeCache[OwnModuleId].Dirty)
      return true;

    for (const auto &Source : Imports)
      if (BitcodeCache[Source.first()].Dirty)
        return true;

    return false;
  }

  DenseSet<GlobalValue::GUID> collectGlobalPreservedGuids() const {
    DenseSet<GlobalValue::GUID> GlobalPreservedGuids;
    for (const auto &ModuleEntry : ModuleRecords) {
      auto Guids = ModuleEntry.second.collectPreservedGuids();
      GlobalPreservedGuids.insert(Guids.begin(), Guids.end());
    }
    return GlobalPreservedGuids;
  }

  std::pair<StringMap<GUIDs_t>, StringMap<GUIDs_t>>
  buildIndexDiff(const ModuleSummaryIndex &CurrentIndex,
                 const ModuleSummaryIndex &NewIndex) {
    StringMap<GUIDs_t> Inserted;
    StringMap<GUIDs_t> Removed;

    for (const auto &CEntry : CurrentIndex) {
      if (const auto *CSummary = getSummary(CEntry.second)) {
        GlobalValue::GUID Guid = CEntry.first;
        StringRef ModuleId = CSummary->modulePath();

        if (auto *NSummary = NewIndex.findSummaryInModule(Guid, ModuleId)) {
          if (NSummary->getSummaryKind() != CSummary->getSummaryKind()) {
            assert(false && "Collision in Global Unique IDs");
            // Modified[ModuleId].insert(Guid);
          }
        } else {
          Removed[ModuleId].insert(Guid);
        }
      }
    }

    for (const auto &NEntry : NewIndex) {
      if (const auto *NSummary = getSummary(NEntry.second)) {
        GlobalValue::GUID Guid = NEntry.first;
        StringRef ModuleId = NSummary->modulePath();

        if (!CurrentIndex.findSummaryInModule(Guid, ModuleId)) {
          Inserted[ModuleId].insert(Guid);
        }
      }
    }

    // TODO: Modified
    return std::make_pair(std::move(Inserted), std::move(Removed));
  }

  void removeMatchingGuids(DenseSet<GlobalValue::GUID> &Guids,
                           const StringMap<GUIDs_t> &RemovedGuids) {
    for (const auto &ModuleEntry : RemovedGuids) {
      for (GlobalValue::GUID Guid : ModuleEntry.second) {
        Guids.erase(Guid);
      }
    }
  }

  void
  setupNewRevisionsForAffectedModules(const StringMap<GUIDs_t> &InsertedGuids,
                                      const StringMap<GUIDs_t> &RemovedGuids) {
    for (const auto &ModuleEntry : InsertedGuids) {
      StringRef ModuleId = ModuleEntry.first();
      ModuleRecords.try_emplace(ModuleId, ModuleId);
    }

    for (const auto &ModuleEntry : RemovedGuids) {
      StringRef ModuleId = ModuleEntry.first();
      ModuleRecords.try_emplace(ModuleId, ModuleId);
    }

    GUIDs_t NoGuids{};
    DenseSet<PermanentModuleRecord *> DependentModules;

    for (auto &Entry : ModuleRecords) {
      StringRef ModuleId = Entry.first();
      auto IIt = InsertedGuids.find(ModuleId);
      auto RIt = RemovedGuids.find(ModuleId);
      const auto &I = (IIt == InsertedGuids.end()) ? NoGuids : IIt->second;
      const auto &R = (RIt == RemovedGuids.end()) ? NoGuids : RIt->second;

      if (I.size() + R.size() > 0) {
        PermanentModuleRecord *ModuleRecord = &Entry.second;
        if (const auto *Dependents = ModuleRecord->openRevision(I, R)) {
          DependentModules.insert(Dependents->begin(), Dependents->end());
        }
      }
    }

    for (PermanentModuleRecord *D : DependentModules)
      if (!D->hasOpenRevision())
        D->openRevisionForRebind();
  }

  template <class Return_t, class... Args_tt>
  TypedFunctionHandle<Return_t, Args_tt...> compile(const std::string &Name) {
    auto error = [this, &Name](Error err1, Error err2 = Error::success()) {
      reportOrDrop(joinErrors(make_error<CantCompileFunctionError>(Name),
                              joinErrors(std::move(err1), std::move(err2))));
      return TypedFunctionHandle<Return_t, Args_tt...>{}; // empty result
    };

    Error LoadBitcodeErrs = updateModuleCacheFromBitcodeFiles();

    uint64_t FuncGuid = GlobalValue::getGUID(Name);
    StringRef ModuleId = findModuleIdFor(FuncGuid);
    if (ModuleId.empty())
      return error(make_error<MissingModuleError>(FuncGuid),
                   std::move(LoadBitcodeErrs));

    auto IndexOrErr = linkCombinedIndex();
    if (!IndexOrErr)
      return error(IndexOrErr.takeError(), std::move(LoadBitcodeErrs));
    std::unique_ptr<ModuleSummaryIndex> NewGlobalIndex = std::move(*IndexOrErr);

    auto [ModuleRecordIt, NewModule] =
        ModuleRecords.try_emplace(ModuleId, ModuleId);

    PermanentModuleRecord *ModuleRecord = &ModuleRecordIt->second;
    if (!NewModule) {
      // Dead symbols not stripped in this import list.
      ImportMap_t PreliminaryImports;
      llvm::ComputeCrossModuleImportForModule(ModuleId, *NewGlobalIndex,
                                              PreliminaryImports);

      // Early exit: Anything to do at all?
      if (!dependsOnDirtyBitcodeSource(ModuleId, PreliminaryImports)) {
        consumeError(std::move(LoadBitcodeErrs));
        auto FuncRecord = ModuleRecord->getFunction(Name);
        return TypedFunctionHandle<Return_t, Args_tt...>(FuncRecord);
      }
    }

    auto [InsertedGuids, RemovedGuids] =
        buildIndexDiff(*CachedGlobalIndex, *NewGlobalIndex);

    CachedGlobalIndex = std::move(NewGlobalIndex);

    // Remove symbols that are unreachable from any preserved one
    auto GlobalPreservedGuids = collectGlobalPreservedGuids();
    removeMatchingGuids(GlobalPreservedGuids, RemovedGuids);
    GlobalPreservedGuids.insert(GlobalValue::getGUID(Name));
    llvm::computeDeadSymbols(*CachedGlobalIndex, GlobalPreservedGuids);

    setupNewRevisionsForAffectedModules(InsertedGuids, RemovedGuids);
    adjustLinkageOfPreservedFunctions(GlobalPreservedGuids);

    auto Function = ModuleRecord->addFunction(Name);

    std::vector<PermanentModuleRecord *> InvalidatedModules =
        invalidateModules(ModuleRecord, GlobalPreservedGuids);

    updateModuleHashes(InvalidatedModules);

    if (Error Err = recompileInvalidatedModules(std::move(InvalidatedModules)))
      return error(std::move(Err), std::move(LoadBitcodeErrs));

    // Test Symbol Resolution
    auto Addr = Function->getAddress();
    if (!Addr)
      return error(Addr.takeError(), std::move(LoadBitcodeErrs));

    // Load-erros may be reported as warnings.
    llvm::consumeError(std::move(LoadBitcodeErrs));
    return TypedFunctionHandle<Return_t, Args_tt...>(std::move(Function));
  }

  std::vector<PermanentModuleRecord *>
  invalidateModules(PermanentModuleRecord *Origin,
                    const DenseSet<GlobalValue::GUID> &GlobalPreservedGuids) {
    BitcodeCache[Origin->getModuleId()].Dirty = false;

    std::vector<PermanentModuleRecord *> Process;
    Process.reserve(ModuleRecords.size()); // never reallocate!
    Process.push_back(Origin);

    // This is adding to the end of the vector, so we WANT to reevaluate the
    // end iterator after each iteration!
    for (auto It = Process.begin(); It != Process.end(); It++) {
      ImportMap_t Imports;
      PermanentModuleRecord *ModuleRecord = *It;
      llvm::ComputeCrossModuleImportForModule(ModuleRecord->getModuleId(),
                                              *CachedGlobalIndex, Imports);

      for (const auto &Source : Imports) {
        StringRef SourceModuleId = Source.first();
        BitcodeCacheEntry &CacheEntry = BitcodeCache[SourceModuleId];

        auto SourceRecordIt = ModuleRecords.find(SourceModuleId);
        assert(SourceRecordIt != ModuleRecords.end());

        SourceRecordIt->second.addDependent(ModuleRecord);

        if (CacheEntry.Dirty) {
          Process.push_back(&SourceRecordIt->second);
          CacheEntry.Dirty = false;
        }
      }

      ModuleRecord->commitImports(std::move(Imports));
    }

    return Process;
  }

  // Bitcode changes affect the module hash. Add new hashes to our mapping.
  // All existing entries stay intact.
  void updateModuleHashes(std::vector<PermanentModuleRecord *> Modules) {
    for (PermanentModuleRecord *M : Modules) {
      ModuleRecordsByHash[getModuleHashStrFromIndex(M)] = M;
    }
  }

  Error
  recompileInvalidatedModules(std::vector<PermanentModuleRecord *> Modules) {
    // Not all errors are fatal. Collect them on the way.
    Error Errs = Error::success();
    auto QueueErr = [&Errs](Error Err) {
      Errs = joinErrors(std::move(Errs), std::move(Err));
    };

    // Provide memory addresses for global variables that exist in other
    // modules or previous revisions. This way we preserve program state.
    auto CrossModuleSymbolProvider = [this](const std::string &N) {
      // We only care about existing global variables. They became
      // declarations and were marked as external earlier, which triggered
      // promotion when building the combined module (adding the postfix with
      // the source module hash to their names).
      auto [UnpromotedName, ModHashStr] = StringRef(N).split(".llvm.");

      // Everything without a postfix must be resolved elsewhere.
      if (ModHashStr.empty())
        return JITTargetAddress{};

      // We don't import global variable definitions. They remain in their
      // original module. We can query that one using using our updated
      // mapping from above.
      PermanentModuleRecord *ModuleRecord = ModuleRecordsByHash[ModHashStr];

      // The first occurance of the symbol has NOT been promoted. Search for
      // the raw symbol name backwards through the module's revisions.
      auto Addr = ModuleRecord->findSymbol(UnpromotedName);
      if (Addr && *Addr)
        return *Addr;

      reportOrDrop(Addr.takeError());
      return JITTargetAddress{};
    };

    // Recompile all modules and submit them to the JIT. Finalization and
    // linking will happen on demand.
    for (PermanentModuleRecord *M : Modules) {
      // Materialize the main module from the cached buffer.
      auto Buffer = BitcodeCache[M->getModuleId()].getBuffer();
      auto MainModuleOrErr = llvm::parseBitcodeFile(Buffer, Context);
      if (!MainModuleOrErr) {
        QueueErr(MainModuleOrErr.takeError());
        continue;
      }

      deleteOwnExistingDefinitions(**MainModuleOrErr, M);

      auto ModuleOrErr = linkCombinedModule(std::move(*MainModuleOrErr), M);
      if (!ModuleOrErr) {
        QueueErr(ModuleOrErr.takeError());
        continue;
      }

      makePrivateVariablesInternal(**ModuleOrErr);

      auto HandleOrErr =
          Jit.addModule(std::move(*ModuleOrErr), CrossModuleSymbolProvider);
      if (!HandleOrErr) {
        QueueErr(HandleOrErr.takeError());
        continue;
      }

      M->commitCode(std::move(*HandleOrErr));
    }

    return Errs;
  }
};

} // namespace llvm::thinltojit
