//===--- Graph.cpp - Complete translation-unit graph facts ------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Graph.h"
#include "support/Path.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Types.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
namespace clangd {
namespace {

llvm::json::Value rangeJSON(const GraphRange &Range) {
  return llvm::json::Object{{"file", Range.FileURI},
                            {"startLine", Range.StartLine},
                            {"startColumn", Range.StartColumn},
                            {"endLine", Range.EndLine},
                            {"endColumn", Range.EndColumn}};
}

bool rangeFromJSON(const llvm::json::Value &Value, GraphRange &Range,
                   llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("file", Range.FileURI) &&
         O.map("startLine", Range.StartLine) &&
         O.map("startColumn", Range.StartColumn) &&
         O.map("endLine", Range.EndLine) && O.map("endColumn", Range.EndColumn);
}

template <typename T, typename Encode>
llvm::json::Array arrayJSON(const std::vector<T> &Values, Encode E) {
  llvm::json::Array Result;
  Result.reserve(Values.size());
  for (const auto &Value : Values)
    Result.push_back(E(Value));
  return Result;
}

llvm::json::Value attributeJSON(const GraphAttribute &Attribute) {
  return llvm::json::Object{{"name", Attribute.Name},
                            {"range", rangeJSON(Attribute.Range)}};
}

bool attributeFromJSON(const llvm::json::Value &Value,
                       GraphAttribute &Attribute, llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("name", Attribute.Name) && O.map("range", Attribute.Range);
}

llvm::json::Value symbolJSON(const GraphSymbol &Symbol) {
  return llvm::json::Object{
      {"usr", Symbol.USR},
      {"id", Symbol.ID},
      {"name", Symbol.Name},
      {"qualifiedName", Symbol.QualifiedName},
      {"ownerUsr", Symbol.OwnerUSR},
      {"signature", Symbol.Signature},
      {"kind", Symbol.Kind},
      {"subKind", Symbol.SubKind},
      {"properties", Symbol.Properties},
      {"local", Symbol.Local},
      {"internal", Symbol.Internal},
      {"anonymous", Symbol.Anonymous},
      {"exported", Symbol.Exported},
      {"declaration", rangeJSON(Symbol.Declaration)},
      {"definition", rangeJSON(Symbol.Definition)},
      {"attributes", arrayJSON(Symbol.Attributes, attributeJSON)}};
}

bool symbolFromJSON(const llvm::json::Value &Value, GraphSymbol &Symbol,
                    llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("usr", Symbol.USR) && O.map("name", Symbol.Name) &&
         O.map("id", Symbol.ID) &&
         O.map("qualifiedName", Symbol.QualifiedName) &&
         O.map("ownerUsr", Symbol.OwnerUSR) &&
         O.map("signature", Symbol.Signature) && O.map("kind", Symbol.Kind) &&
         O.map("subKind", Symbol.SubKind) &&
         O.map("properties", Symbol.Properties) &&
         O.map("local", Symbol.Local) && O.map("internal", Symbol.Internal) &&
         O.map("anonymous", Symbol.Anonymous) &&
         O.map("exported", Symbol.Exported) &&
         O.map("declaration", Symbol.Declaration) &&
         O.map("definition", Symbol.Definition) &&
         O.map("attributes", Symbol.Attributes);
}

llvm::json::Value occurrenceJSON(const GraphOccurrence &Occurrence) {
  return llvm::json::Object{{"usr", Occurrence.USR},
                            {"id", Occurrence.ID},
                            {"containerId", Occurrence.ContainerID},
                            {"roles", Occurrence.Roles},
                            {"targetKind", Occurrence.TargetKind},
                            {"spelling", rangeJSON(Occurrence.Spelling)},
                            {"expansion", rangeJSON(Occurrence.Expansion)}};
}

bool occurrenceFromJSON(const llvm::json::Value &Value,
                        GraphOccurrence &Occurrence, llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("usr", Occurrence.USR) && O.map("id", Occurrence.ID) &&
         O.map("containerId", Occurrence.ContainerID) &&
         O.map("roles", Occurrence.Roles) &&
         O.map("targetKind", Occurrence.TargetKind) &&
         O.map("spelling", Occurrence.Spelling) &&
         O.map("expansion", Occurrence.Expansion);
}

llvm::json::Value relationJSON(const GraphRelation &Relation) {
  return llvm::json::Object{{"subjectId", Relation.SubjectID},
                            {"objectId", Relation.ObjectID},
                            {"roles", Relation.Roles},
                            {"evidence", rangeJSON(Relation.Evidence)}};
}

bool relationFromJSON(const llvm::json::Value &Value, GraphRelation &Relation,
                      llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("subjectId", Relation.SubjectID) &&
         O.map("objectId", Relation.ObjectID) &&
         O.map("roles", Relation.Roles) && O.map("evidence", Relation.Evidence);
}

llvm::json::Value macroJSON(const GraphMacro &Macro) {
  return llvm::json::Object{{"usr", Macro.USR},
                            {"id", Macro.ID},
                            {"name", Macro.Name},
                            {"roles", Macro.Roles},
                            {"definition", rangeJSON(Macro.Definition)},
                            {"spelling", rangeJSON(Macro.Spelling)},
                            {"expansion", rangeJSON(Macro.Expansion)}};
}

bool macroFromJSON(const llvm::json::Value &Value, GraphMacro &Macro,
                   llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("usr", Macro.USR) && O.map("name", Macro.Name) &&
         O.map("id", Macro.ID) && O.map("roles", Macro.Roles) &&
         O.map("definition", Macro.Definition) &&
         O.map("spelling", Macro.Spelling) &&
         O.map("expansion", Macro.Expansion);
}

llvm::json::Value includeJSON(const GraphInclude &Include) {
  return llvm::json::Object{{"source", Include.SourceURI},
                            {"target", Include.TargetURI},
                            {"spelling", Include.Spelling},
                            {"angled", Include.Angled},
                            {"moduleImported", Include.ModuleImported},
                            {"evidence", rangeJSON(Include.Evidence)}};
}

bool includeFromJSON(const llvm::json::Value &Value, GraphInclude &Include,
                     llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("source", Include.SourceURI) &&
         O.map("target", Include.TargetURI) &&
         O.map("spelling", Include.Spelling) &&
         O.map("angled", Include.Angled) &&
         O.map("moduleImported", Include.ModuleImported) &&
         O.map("evidence", Include.Evidence);
}

llvm::json::Value missingIncludeJSON(const GraphMissingInclude &Include) {
  return llvm::json::Object{{"source", Include.SourceURI},
                            {"spelling", Include.Spelling},
                            {"angled", Include.Angled}};
}

bool missingIncludeFromJSON(const llvm::json::Value &Value,
                            GraphMissingInclude &Include,
                            llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("source", Include.SourceURI) &&
         O.map("spelling", Include.Spelling) && O.map("angled", Include.Angled);
}

llvm::json::Value moduleJSON(const GraphModule &Module) {
  return llvm::json::Object{{"name", Module.Name},
                            {"roles", Module.Roles},
                            {"evidence", rangeJSON(Module.Evidence)}};
}

bool moduleFromJSON(const llvm::json::Value &Value, GraphModule &Module,
                    llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("name", Module.Name) && O.map("roles", Module.Roles) &&
         O.map("evidence", Module.Evidence);
}

llvm::json::Value sourceJSON(const GraphSource &Source) {
  return llvm::json::Object{{"uri", Source.URI},
                            {"digest", Source.Digest},
                            {"diskDigest", Source.DiskDigest},
                            {"flags", Source.Flags}};
}

bool sourceFromJSON(const llvm::json::Value &Value, GraphSource &Source,
                    llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("uri", Source.URI) && O.map("digest", Source.Digest) &&
         O.map("diskDigest", Source.DiskDigest) && O.map("flags", Source.Flags);
}

llvm::json::Value diagnosticJSON(const GraphDiagnostic &Diagnostic) {
  return llvm::json::Object{{"message", Diagnostic.Message},
                            {"code", Diagnostic.Code},
                            {"severity", Diagnostic.Severity},
                            {"range", rangeJSON(Diagnostic.Range)}};
}

bool diagnosticFromJSON(const llvm::json::Value &Value,
                        GraphDiagnostic &Diagnostic, llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("message", Diagnostic.Message) &&
         O.map("code", Diagnostic.Code) &&
         O.map("severity", Diagnostic.Severity) &&
         O.map("range", Diagnostic.Range);
}

} // namespace

bool fromJSON(const llvm::json::Value &Value, GraphSnapshotParams &Params,
              llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.mapOptional("knownGeneration", Params.KnownGeneration) &&
         O.mapOptional("cursor", Params.Cursor) &&
         O.mapOptional("maxShards", Params.MaxShards);
}

bool fromJSON(const llvm::json::Value &Value, GraphRange &Range,
              llvm::json::Path Path) {
  return rangeFromJSON(Value, Range, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphAttribute &Attribute,
              llvm::json::Path Path) {
  return attributeFromJSON(Value, Attribute, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphSymbol &Symbol,
              llvm::json::Path Path) {
  return symbolFromJSON(Value, Symbol, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphOccurrence &Occurrence,
              llvm::json::Path Path) {
  return occurrenceFromJSON(Value, Occurrence, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphRelation &Relation,
              llvm::json::Path Path) {
  return relationFromJSON(Value, Relation, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphMacro &Macro,
              llvm::json::Path Path) {
  return macroFromJSON(Value, Macro, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphInclude &Include,
              llvm::json::Path Path) {
  return includeFromJSON(Value, Include, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphMissingInclude &Include,
              llvm::json::Path Path) {
  return missingIncludeFromJSON(Value, Include, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphModule &Module,
              llvm::json::Path Path) {
  return moduleFromJSON(Value, Module, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphSource &Source,
              llvm::json::Path Path) {
  return sourceFromJSON(Value, Source, Path);
}

bool fromJSON(const llvm::json::Value &Value, GraphDiagnostic &Diagnostic,
              llvm::json::Path Path) {
  return diagnosticFromJSON(Value, Diagnostic, Path);
}

llvm::json::Value toJSON(const GraphTU &Graph) {
  return llvm::json::Object{
      {"producerFingerprint", Graph.ProducerFingerprint},
      {"mainFileUri", Graph.MainFileURI},
      {"mainFile", Graph.MainFile},
      {"directory", Graph.Directory},
      {"commandLine", llvm::json::Array(Graph.CommandLine)},
      {"output", Graph.Output},
      {"commandDigest", Graph.CommandDigest},
      {"toolchainFingerprint", Graph.ToolchainFingerprint},
      {"targetTriple", Graph.TargetTriple},
      {"language", Graph.Language},
      {"hadErrors", Graph.HadErrors},
      {"sources", arrayJSON(Graph.Sources, sourceJSON)},
      {"symbols", arrayJSON(Graph.Symbols, symbolJSON)},
      {"occurrences", arrayJSON(Graph.Occurrences, occurrenceJSON)},
      {"relations", arrayJSON(Graph.Relations, relationJSON)},
      {"macros", arrayJSON(Graph.Macros, macroJSON)},
      {"includes", arrayJSON(Graph.Includes, includeJSON)},
      {"missingIncludes", arrayJSON(Graph.MissingIncludes, missingIncludeJSON)},
      {"modules", arrayJSON(Graph.Modules, moduleJSON)},
      {"diagnostics", arrayJSON(Graph.Diagnostics, diagnosticJSON)}};
}

bool fromJSON(const llvm::json::Value &Value, GraphTU &Graph,
              llvm::json::Path Path) {
  llvm::json::ObjectMapper O(Value, Path);
  return O && O.map("producerFingerprint", Graph.ProducerFingerprint) &&
         O.map("mainFileUri", Graph.MainFileURI) &&
         O.map("mainFile", Graph.MainFile) &&
         O.map("directory", Graph.Directory) &&
         O.map("commandLine", Graph.CommandLine) &&
         O.map("output", Graph.Output) &&
         O.map("commandDigest", Graph.CommandDigest) &&
         O.map("toolchainFingerprint", Graph.ToolchainFingerprint) &&
         O.map("targetTriple", Graph.TargetTriple) &&
         O.map("language", Graph.Language) &&
         O.map("hadErrors", Graph.HadErrors) &&
         O.map("sources", Graph.Sources) && O.map("symbols", Graph.Symbols) &&
         O.map("occurrences", Graph.Occurrences) &&
         O.map("relations", Graph.Relations) && O.map("macros", Graph.Macros) &&
         O.map("includes", Graph.Includes) &&
         O.map("missingIncludes", Graph.MissingIncludes) &&
         O.map("modules", Graph.Modules) &&
         O.map("diagnostics", Graph.Diagnostics);
}

std::string graphDigest(llvm::StringRef Bytes) {
  return llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(Bytes)),
                     /*LowerCase=*/true);
}

std::string graphCommandDigest(const tooling::CompileCommand &Command) {
  std::string Canonical;
  llvm::raw_string_ostream OS(Canonical);
  auto Field = [&](llvm::StringRef Value) {
    OS << Value.size() << ':' << Value;
  };
  auto NormalizePath = [](llvm::StringRef Path) {
    return maybeCaseFoldPath(llvm::sys::path::convert_to_slash(Path));
  };
  const std::string NormalizedFilename = NormalizePath(Command.Filename);
  Field(NormalizePath(Command.Directory));
  Field(NormalizedFilename);
  Field(NormalizePath(Command.Output));
  OS << Command.CommandLine.size() << ':';
  for (const auto &Argument : Command.CommandLine) {
    // CommandMangler appends the requested file spelling. CDB notifications
    // can use native separators while later lookups use URI-style separators;
    // these name the same Windows input and must not create a new semantic
    // configuration. Other arguments remain byte-exact.
    const std::string NormalizedArgument = NormalizePath(Argument);
    Field(NormalizedArgument == NormalizedFilename ? NormalizedFilename
                                                   : Argument);
  }
  return graphDigest(OS.str());
}

std::string graphProducerFingerprint() {
  static const std::string Fingerprint = graphDigest(
      (llvm::Twine("samchon-graph-schema:1\nversion:") + getClangFullVersion() +
       "\nrepository:" + getClangFullRepositoryVersion())
          .str());
  return Fingerprint;
}

bool graphCommandIsCOrCXX(const tooling::CompileCommand &Command) {
  namespace types = clang::driver::types;
  types::ID Type = types::TY_INVALID;
  auto SetType = [&](llvm::StringRef Name) {
    if (Name == "none") {
      Type = types::TY_INVALID;
      return;
    }
    std::string Owned = Name.str();
    Type = types::lookupTypeForTypeSpecifier(Owned.c_str());
  };
  for (size_t I = 0; I < Command.CommandLine.size(); ++I) {
    llvm::StringRef Argument = Command.CommandLine[I];
    if (Argument == "-x" && I + 1 < Command.CommandLine.size()) {
      SetType(Command.CommandLine[++I]);
      continue;
    }
    if (Argument.starts_with("-x") && Argument.size() > 2) {
      SetType(Argument.drop_front(2));
      continue;
    }
    if (Argument.equals_insensitive("/TC") ||
        Argument.starts_with_insensitive("/Tc")) {
      SetType("c");
      continue;
    }
    if (Argument.equals_insensitive("/TP") ||
        Argument.starts_with_insensitive("/Tp"))
      SetType("c++");
  }
  if (Type == types::TY_INVALID) {
    llvm::StringRef Extension = llvm::sys::path::extension(Command.Filename);
    if (Extension.consume_front("."))
      Type = types::lookupTypeForExtension(Extension);
  }
  return Type != types::TY_INVALID && types::isDerivedFromC(Type) &&
         !types::isObjC(Type) && !types::isCuda(Type) && !types::isHIP(Type) &&
         !types::isOpenCL(Type) && !types::isHLSL(Type) &&
         !types::onlyPrecompileType(Type);
}

} // namespace clangd
} // namespace clang
