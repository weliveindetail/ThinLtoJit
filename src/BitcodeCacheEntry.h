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

struct BitcodeCacheEntry {
  std::string SourceFileName;
  llvm::SmallString<20> ContentHash;
  std::unique_ptr<llvm::MemoryBuffer> ContentBuffer;
  std::unique_ptr<llvm::BitcodeModule> SourceBitcodeModule;
  std::unique_ptr<llvm::ModuleSummaryIndex> ModuleIndex;
  bool Dirty = false;
  bool UpdateError = false;

  BitcodeCacheEntry() = default;

  llvm::MemoryBufferRef getBuffer() const {
    assert(ContentBuffer && "Cache corruption");
    return ContentBuffer->getMemBufferRef();
  }

  llvm::Error update(std::unique_ptr<llvm::MemoryBuffer> Buffer) {
    using namespace llvm;

    if (isDirty(Buffer->getMemBufferRef())) {
      ContentBuffer = std::move(Buffer);

      auto IRSymtab = object::readIRSymtab(ContentBuffer->getMemBufferRef());
      if (!IRSymtab)
        return IRSymtab.takeError();

      if (IRSymtab->Mods.size() != 1)
        return make_error<StringError>("Expected a single module",
                                       inconvertibleErrorCode());

      SourceFileName = IRSymtab->TheReader.getSourceFileName();
      SourceBitcodeModule =
          std::make_unique<BitcodeModule>(std::move(IRSymtab->Mods[0]));

      auto IndexOrErr = SourceBitcodeModule->getSummary();
      if (!IndexOrErr)
        return IndexOrErr.takeError();

      ModuleIndex = std::move(*IndexOrErr);
    }

    return Error::success();
  }

private:
  bool isDirty(llvm::MemoryBufferRef NewContent) {
    using namespace llvm;

    SHA1 Hasher;
    Hasher.update(NewContent.getBuffer());
    SmallString<20> NewHash = Hasher.final();

    Dirty = Dirty || !NewHash.equals(ContentHash);
    ContentHash = NewHash;

    return Dirty;
  }
};

} // namespace llvm::thinltojit
