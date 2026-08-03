//===--- CompileCommands.h - Manipulation of compile flags -------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPILECOMMANDS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPILECOMMANDS_H

#include "GlobalCompilationDatabase.h"
#include "support/Threading.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

// CommandMangler transforms compile commands from some external source
// for use in clangd. This means:
//  - running the frontend only, stripping args regarding output files etc
//  - forcing the use of clangd's builtin headers rather than clang's
//  - resolving argv0 as cc1 expects
//  - injecting -isysroot flags on mac as the system clang does
struct CommandMangler {
  // Absolute path to clang.
  std::optional<std::string> ClangPath;
  // Directory containing builtin headers.
  std::optional<std::string> ResourceDir;
  // Root for searching for standard library (passed to -isysroot).
  std::optional<std::string> Sysroot;
  SystemIncludeExtractorFn SystemIncludeExtractor;

  // A command-mangler that doesn't know anything about the system.
  // This is hermetic for unit-tests, but won't work well in production.
  static CommandMangler forTests();
  // Probe the system and build a command-mangler that knows the toolchain.
  //  - try to find clang on $PATH, otherwise fake a path near clangd
  //  - find the resource directory installed near clangd
  //  - on mac, find clang and isysroot by querying the `xcrun` launcher
  static CommandMangler detect();

  // `Cmd` may describe compilation of a different file, and will be updated
  // for parsing `TargetFile`.
  void operator()(tooling::CompileCommand &Cmd,
                  llvm::StringRef TargetFile) const;
};

/// Fingerprints the resolved compiler driver path and its exact bytes.
///
/// This is deliberately recomputed when commands are requested: compiler
/// wrappers and symlink targets can change without changing compile_commands,
/// and a compiler-owned graph must not reuse the old toolchain universe.
std::string
compileCommandDriverFingerprint(const tooling::CompileCommand &Command);

/// Deduplicates exact driver hashing within one bounded operation.
///
/// A cache instance is intentionally scoped by its caller (for example, one
/// graph validation or one TU/configuration batch). The first command for a
/// resolved driver reads its exact bytes; later commands reuse that result.
class CompileCommandDriverFingerprintCache {
public:
  CompileCommandDriverFingerprintCache();
  ~CompileCommandDriverFingerprintCache();
  CompileCommandDriverFingerprintCache(CompileCommandDriverFingerprintCache &&);
  CompileCommandDriverFingerprintCache &
  operator=(CompileCommandDriverFingerprintCache &&);
  CompileCommandDriverFingerprintCache(
      const CompileCommandDriverFingerprintCache &) = delete;
  CompileCommandDriverFingerprintCache &
  operator=(const CompileCommandDriverFingerprintCache &) = delete;

  std::string get(const tooling::CompileCommand &Command);

private:
  struct Impl;
  std::unique_ptr<Impl> State;
};

/// Fingerprints the exact driver together with the effective command that
/// determines its target and include universe.
std::string compileCommandToolchainFingerprint(
    const tooling::CompileCommand &Command,
    CompileCommandDriverFingerprintCache *DriverCache = nullptr);

// Removes args from a command-line in a semantically-aware way.
//
// Internally this builds a large (0.5MB) table of clang options on first use.
// Both strip() and process() are fairly cheap after that.
//
// FIXME: this reimplements much of OptTable, it might be nice to expose more.
// The table-building strategy may not make sense outside clangd.
class ArgStripper {
public:
  ArgStripper() = default;
  ArgStripper(ArgStripper &&) = default;
  ArgStripper(const ArgStripper &) = delete;
  ArgStripper &operator=(ArgStripper &&) = default;
  ArgStripper &operator=(const ArgStripper &) = delete;

  // Adds the arg to the set which should be removed.
  //
  // Recognized clang flags are stripped semantically. When "-I" is stripped:
  //  - so is its value (either as -Ifoo or -I foo)
  //  - aliases like --include-directory=foo are also stripped
  //  - CL-style /Ifoo will be removed if the args indicate MS-compatible mode
  // Compile args not recognized as flags are removed literally, except:
  //  - strip("ABC*") will remove any arg with an ABC prefix.
  //
  // In either case, the -Xclang prefix will be dropped if present.
  void strip(llvm::StringRef Arg);
  // Remove the targets from a compile command, in-place.
  void process(std::vector<std::string> &Args) const;

private:
  // Deletion rules, to be checked for each arg.
  struct Rule {
    llvm::StringRef Text;    // Rule applies only if arg begins with Text.
    unsigned char Modes = 0; // Rule applies only in specified driver modes.
    uint16_t Priority = 0;   // Lower is better.
    uint16_t ExactArgs = 0;  // Num args consumed when Arg == Text.
    uint16_t PrefixArgs = 0; // Num args consumed when Arg starts with Text.
  };
  static llvm::ArrayRef<Rule> rulesFor(llvm::StringRef Arg);
  const Rule *matchingRule(llvm::StringRef Arg, unsigned Mode,
                           unsigned &ArgCount) const;
  llvm::SmallVector<Rule> Rules;
  std::deque<std::string> Storage; // Store strings not found in option table.
};

// Renders an argv list, with arguments separated by spaces.
// Where needed, arguments are "quoted" and escaped.
std::string printArgv(llvm::ArrayRef<llvm::StringRef> Args);
std::string printArgv(llvm::ArrayRef<std::string> Args);

} // namespace clangd
} // namespace clang

#endif
