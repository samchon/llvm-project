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
};

bool fromJSON(const llvm::json::Value &Value, GraphSnapshotParams &Params,
              llvm::json::Path Path);

/// Source range retained for graph export. Positions are zero-based UTF-8 byte
/// offsets within their line, matching clangd's background-index coordinates.
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

struct GraphModule {
  std::string Name;
  uint32_t Roles = 0;
  GraphRange Evidence;
};

struct GraphSource {
  std::string URI;
  std::string Digest;
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
  std::string MainFileURI;
  std::string MainFile;
  std::string Directory;
  std::vector<std::string> CommandLine;
  std::string Output;
  std::string CommandDigest;
  std::string TargetTriple;
  std::string Language;
  bool HadErrors = false;
  std::vector<GraphSource> Sources;
  std::vector<GraphSymbol> Symbols;
  std::vector<GraphOccurrence> Occurrences;
  std::vector<GraphRelation> Relations;
  std::vector<GraphMacro> Macros;
  std::vector<GraphInclude> Includes;
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
std::string graphCommandDigest(const tooling::CompileCommand &Command);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_GRAPH_H
