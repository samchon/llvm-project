//===--- Graph.h - Complete translation-unit graph facts --------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_GRAPH_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_GRAPH_H

#include "clang/Index/IndexSymbol.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

struct GraphSnapshotParams {
  std::optional<std::string> KnownGeneration;
  std::optional<std::string> Cursor;
  std::optional<uint32_t> MaxShards;
};

bool fromJSON(const llvm::json::Value &Value, GraphSnapshotParams &Params,
              llvm::json::Path Path);

/// Source range retained for graph export. Positions are zero-based UTF-16
/// code-unit offsets within their line, independent of the LSP session's
/// negotiated offset encoding.
struct GraphRange {
  std::string FileURI;
  uint32_t StartLine = 0;
  uint32_t StartColumn = 0;
  uint32_t EndLine = 0;
  uint32_t EndColumn = 0;

  bool valid() const { return !FileURI.empty(); }
};

struct GraphAttribute {
  std::string Name;
  GraphRange Range;
};

/// One declaration in one translation-unit/configuration view.
struct GraphSymbol {
  /// Raw Clang USR. Exporters add the TU/configuration coordinates that make
  /// local, internal-linkage, macro, and header-view identities unambiguous.
  std::string USR;
  /// TU-local endpoint key. It is the raw USR for externally visible symbols
  /// and adds the declaring file/owner coordinate where Clang's USR alone can
  /// collide.
  std::string ID;
  std::string Name;
  std::string QualifiedName;
  std::string OwnerUSR;
  std::string Signature;
  uint32_t Kind = 0;
  uint32_t SubKind = 0;
  uint32_t Properties = 0;
  bool Local = false;
  bool Internal = false;
  bool Anonymous = false;
  bool Exported = false;
  GraphRange Declaration;
  GraphRange Definition;
  std::vector<GraphAttribute> Attributes;
};

/// One semantic occurrence, retaining every Clang SymbolRole bit as well as
/// spelling and expansion evidence.
struct GraphOccurrence {
  std::string USR;
  std::string ID;
  std::string ContainerID;
  uint32_t Roles = 0;
  uint32_t TargetKind = 0;
  GraphRange Spelling;
  GraphRange Expansion;
};

/// A relation attached to a declaration/reference occurrence. Roles are the
/// original Clang relation-role mask; no lossy clangd RelationKind projection
/// is performed here.
struct GraphRelation {
  std::string SubjectID;
  std::string ObjectID;
  uint32_t Roles = 0;
  GraphRange Evidence;
};

struct GraphMacro {
  std::string USR;
  std::string ID;
  std::string Name;
  uint32_t Roles = 0;
  GraphRange Definition;
  GraphRange Spelling;
  GraphRange Expansion;
};

struct GraphInclude {
  std::string SourceURI;
  std::string TargetURI;
  std::string Spelling;
  bool Angled = false;
  bool ModuleImported = false;
  GraphRange Evidence;
};

/// An include that the compiler could not resolve. These rows are retained on
/// failed analyses only so the resident producer can notice generated inputs
/// appearing and retry their owning translation unit.
struct GraphMissingInclude {
  std::string SourceURI;
  std::string Spelling;
  bool Angled = false;
};

struct GraphModule {
  std::string Name;
  uint32_t Roles = 0;
  GraphRange Evidence;
};

struct GraphSource {
  std::string URI;
  /// SHA-256 of the exact bytes consumed by Clang.
  std::string Digest;
  /// SHA-256 of the same canonical file's native on-disk bytes at collection
  /// time, or empty when the source has no readable disk identity.
  std::string DiskDigest;
  uint32_t Flags = 0;
};

struct GraphDiagnostic {
  std::string Message;
  std::string Code;
  std::string Severity;
  GraphRange Range;
};

/// Complete facts produced by one existing clangd background-index analysis.
/// This object is persisted only in the main-file shard, so header facts retain
/// the TU/configuration that gave them meaning.
struct GraphTU {
  /// Fingerprint of the exact compiler/fork/schema that produced these facts.
  /// Persisted shards are accepted only when this matches the running binary.
  std::string ProducerFingerprint;
  std::string MainFileURI;
  std::string MainFile;
  std::string Directory;
  std::vector<std::string> CommandLine;
  std::string Output;
  std::string CommandDigest;
  /// SHA-256 of the resolved compiler driver path and exact bytes.
  std::string ToolchainFingerprint;
  std::string TargetTriple;
  std::string Language;
  bool HadErrors = false;
  std::vector<GraphSource> Sources;
  std::vector<GraphSymbol> Symbols;
  std::vector<GraphOccurrence> Occurrences;
  std::vector<GraphRelation> Relations;
  std::vector<GraphMacro> Macros;
  std::vector<GraphInclude> Includes;
  std::vector<GraphMissingInclude> MissingIncludes;
  std::vector<GraphModule> Modules;
  std::vector<GraphDiagnostic> Diagnostics;
};

llvm::json::Value toJSON(const GraphTU &Graph);
bool fromJSON(const llvm::json::Value &Value, GraphRange &Range,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphAttribute &Attribute,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphSymbol &Symbol,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphOccurrence &Occurrence,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphRelation &Relation,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphMacro &Macro,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphInclude &Include,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphMissingInclude &Include,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphModule &Module,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphSource &Source,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphDiagnostic &Diagnostic,
              llvm::json::Path Path);
bool fromJSON(const llvm::json::Value &Value, GraphTU &Graph,
              llvm::json::Path Path);

/// SHA-256 used by graph protocol identities and manifests.
std::string graphDigest(llvm::StringRef Bytes);

/// Splits one translation unit's facts into the files they were found in.
///
/// A translation unit sees every header it includes, so a header's facts are
/// in the body of every unit that includes it. Published by content digest,
/// a header's piece is then written once however many units saw it, instead
/// of once per unit.
///
/// The pieces partition the unit: every fact belongs to exactly one of them,
/// and reassembling all of them gives the unit back fact for fact. A body's
/// digest counts the facts it was made from, so a fact filed twice or filed
/// nowhere makes a body that no longer answers to its own name.
///
/// Only the main file's piece carries the unit's identity and source list,
/// and it is the piece reassembly starts from. A header's piece carries
/// neither, which is what lets two units that include it agree byte for byte.
/// Keyed by file URI.
llvm::StringMap<GraphTU> graphPiecesByFile(const GraphTU &Graph);
std::string graphCommandDigest(const tooling::CompileCommand &Command);
std::string graphProducerFingerprint();
bool graphCommandIsCOrCXX(const tooling::CompileCommand &Command);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_GRAPH_H
