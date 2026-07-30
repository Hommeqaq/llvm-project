//===-- EJitCrossLink.h - EJIT Cross-TU Inlining at Link Time -------------===//

#ifndef LLD_ELF_EJITCROSSLINK_H
#define LLD_ELF_EJITCROSSLINK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include <string>

namespace lld {
namespace elf {

struct EJitCrossLinkResult {
  std::string tempPath;
  llvm::SmallVector<std::string, 4> consumedFiles;
  /// External symbols referenced by the generated registry. The driver uses
  /// these names to let lld extract any matching lazy archive members before
  /// accepting this result as final.
  llvm::SmallVector<std::string, 8> requiredSymbols;
};

/// Merge the .ejit_cross bitcode sections carried by \p SelectedObjects, inline
/// cross-TU callees into each ejit_entry, and write a single registry bitcode
/// module to a temporary file whose path is returned.
///
/// \p SelectedObjects must be the object files lld actually selected for the
/// link (ctx.objectFiles after symbol resolution), so archive members, -l
/// inputs and linker-script inputs that were *not* pulled into the link are
/// never merged. Each buffer's identifier must be the canonical
/// ObjFile::getName(), so the returned consumedFiles match lld's key for
/// discarding the consumed .ejit_cross sections from the output.
///
/// Three-state result:
///   * an Error         - a .ejit_cross section was found but processing failed
///                        (the link must fail; never silently fall back to
///                        AOT);
///   * an empty result  - no selected object carried a .ejit_cross section
///                        (normal skip);
///   * a populated result - the temporary bitcode to add plus the exact input
///                          files whose sections were consumed. The caller owns
///                          cleanup of the temporary file.
llvm::Expected<EJitCrossLinkResult>
runEJitCrossLink(llvm::ArrayRef<llvm::MemoryBufferRef> SelectedObjects,
                 llvm::StringRef TargetTriple,
                 llvm::StringRef SaveTempsPrefix = {});

} // namespace elf
} // namespace lld

#endif
