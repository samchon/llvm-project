//===--- SystemIncludeExtractor.cpp ------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Some compiler drivers have implicit search mechanism for system headers.
// This compilation database implementation tries to extract that information by
// executing the driver in verbose mode. gcc-compatible drivers print something
// like:
// ....
// ....
// #include <...> search starts here:
//  /usr/lib/gcc/x86_64-linux-gnu/7/include
//  /usr/local/include
//  /usr/lib/gcc/x86_64-linux-gnu/7/include-fixed
//  /usr/include/x86_64-linux-gnu
//  /usr/include
// End of search list.
// ....
// ....
// This component parses that output and adds each path to command line args
// provided by Base, after prepending them with -isystem. Therefore current
// implementation would not work with a driver that is not gcc-compatible.
//
// First argument of the command line received from underlying compilation
// database is used as compiler driver path. Due to this arbitrary binary
// execution, this mechanism is not used by default and only executes binaries
// in the paths that are explicitly included by the user.

#include "CompileCommands.h"
#include "Config.h"
#include "GlobalCompilationDatabase.h"
#include "support/Cancellation.h"
#include "support/Logger.h"
#include "support/Threading.h"
#include "support/Trace.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Driver/Types.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace clang::clangd {
namespace {

thread_local uint64_t ActiveRefreshScope = 0;
thread_local uint64_t NextRefreshScope = 0;
thread_local bool RefreshQueriesInActiveScope = false;
thread_local CompileCommandDriverFingerprintCache *ActiveDriverFingerprints =
    nullptr;

struct DriverInfo {
  std::vector<std::string> SystemIncludes;
  std::string Target;
};

enum class DriverQueryStatus { Success, Failed, Cancelled, TimedOut };

template <typename T> struct DriverQueryResult {
  DriverQueryStatus Status;
  std::optional<T> Value;
};

struct DriverArgs {
  // Name of the driver program to execute or absolute path to it.
  std::string Driver;
  // Exact resolved driver identity. A replaced wrapper must miss the query
  // cache even when its path and language are unchanged.
  std::string DriverFingerprint;
  // Whether certain includes should be part of query.
  bool StandardIncludes = true;
  bool StandardCXXIncludes = true;
  // Language to use while querying.
  std::string Lang;
  std::string Sysroot;
  std::string ISysroot;
  std::string Target;
  std::string Stdlib;
  llvm::SmallVector<std::string> Specs;

  bool operator==(const DriverArgs &RHS) const {
    return std::tie(Driver, DriverFingerprint, StandardIncludes,
                    StandardCXXIncludes, Lang, Sysroot, ISysroot, Target,
                    Stdlib, Specs) ==
           std::tie(RHS.Driver, RHS.DriverFingerprint, RHS.StandardIncludes,
                    RHS.StandardCXXIncludes, RHS.Lang, RHS.Sysroot,
                    RHS.ISysroot, RHS.Target, RHS.Stdlib, RHS.Specs);
  }

  DriverArgs(const tooling::CompileCommand &Cmd, llvm::StringRef File,
             std::string DriverFingerprint)
      : DriverFingerprint(std::move(DriverFingerprint)) {
    llvm::SmallString<128> Driver(Cmd.CommandLine.front());
    // Driver is a not a single executable name but instead a path (either
    // relative or absolute).
    if (llvm::any_of(Driver,
                     [](char C) { return llvm::sys::path::is_separator(C); })) {
      llvm::sys::path::make_absolute(Cmd.Directory, Driver);
    }
    this->Driver = Driver.str().str();
    for (size_t I = 0, E = Cmd.CommandLine.size(); I < E; ++I) {
      llvm::StringRef Arg = Cmd.CommandLine[I];

      // Look for Language related flags.
      if (Arg.consume_front("-x")) {
        if (Arg.empty() && I + 1 < E)
          Lang = Cmd.CommandLine[I + 1];
        else
          Lang = Arg.str();
      }
      // Look for standard/builtin includes.
      else if (Arg == "-nostdinc" || Arg == "--no-standard-includes")
        StandardIncludes = false;
      else if (Arg == "-nostdinc++")
        StandardCXXIncludes = false;
      // Figure out sysroot
      else if (Arg.consume_front("--sysroot")) {
        if (Arg.consume_front("="))
          Sysroot = Arg.str();
        else if (Arg.empty() && I + 1 < E)
          Sysroot = Cmd.CommandLine[I + 1];
      } else if (Arg.consume_front("-isysroot")) {
        if (Arg.empty() && I + 1 < E)
          ISysroot = Cmd.CommandLine[I + 1];
        else
          ISysroot = Arg.str();
      } else if (Arg.consume_front("--target=")) {
        Target = Arg.str();
      } else if (Arg.consume_front("-target")) {
        if (Arg.empty() && I + 1 < E)
          Target = Cmd.CommandLine[I + 1];
      } else if (Arg.consume_front("--stdlib")) {
        if (Arg.consume_front("="))
          Stdlib = Arg.str();
        else if (Arg.empty() && I + 1 < E)
          Stdlib = Cmd.CommandLine[I + 1];
      } else if (Arg.consume_front("-stdlib=")) {
        Stdlib = Arg.str();
      } else if (Arg.starts_with("-specs=")) {
        // clang requires a single token like `-specs=file` or `--specs=file`,
        // but gcc will accept two tokens like `--specs file`. Since the
        // compilation database is presumably correct, we just forward the flags
        // as-is.
        Specs.push_back(Arg.str());
      } else if (Arg.starts_with("--specs=")) {
        Specs.push_back(Arg.str());
      } else if (Arg == "--specs" && I + 1 < E) {
        Specs.push_back(Arg.str());
        Specs.push_back(Cmd.CommandLine[I + 1]);
      }
    }

    // Downgrade objective-c++-header (used in clangd's fallback flags for .h
    // files) to c++-header, as some drivers may fail to run the extraction
    // command if it contains `-xobjective-c++-header` and objective-c++ support
    // is not installed.
    // In practice, we don't see different include paths for the two on
    // clang+mac, which is the most common objectve-c compiler.
    if (Lang == "objective-c++-header") {
      Lang = "c++-header";
    }

    // If language is not explicit in the flags, infer from the file.
    // This is important as we want to cache each language separately.
    if (Lang.empty()) {
      llvm::StringRef Ext = llvm::sys::path::extension(File).trim('.');
      auto Type = driver::types::lookupTypeForExtension(Ext);
      if (Type == driver::types::TY_INVALID) {
        elog("System include extraction: invalid file type for {0}", Ext);
      } else {
        Lang = driver::types::getTypeName(Type);
      }
    }
  }
  llvm::SmallVector<llvm::StringRef> render() const {
    // FIXME: Don't treat lang specially?
    assert(!Lang.empty());
    llvm::SmallVector<llvm::StringRef> Args = {"-x", Lang};
    if (!StandardIncludes)
      Args.push_back("-nostdinc");
    if (!StandardCXXIncludes)
      Args.push_back("-nostdinc++");
    if (!Sysroot.empty())
      Args.append({"--sysroot", Sysroot});
    if (!ISysroot.empty())
      Args.append({"-isysroot", ISysroot});
    if (!Target.empty())
      Args.append({"-target", Target});
    if (!Stdlib.empty())
      Args.append({"--stdlib", Stdlib});

    for (llvm::StringRef Spec : Specs) {
      Args.push_back(Spec);
    }

    return Args;
  }

  static DriverArgs getEmpty() { return {}; }

private:
  DriverArgs() = default;
};
} // namespace
} // namespace clang::clangd
namespace llvm {
using DriverArgs = clang::clangd::DriverArgs;
template <> struct DenseMapInfo<DriverArgs> {
  static DriverArgs getEmptyKey() {
    auto Driver = DriverArgs::getEmpty();
    Driver.Driver = "EMPTY_KEY";
    return Driver;
  }
  static DriverArgs getTombstoneKey() {
    auto Driver = DriverArgs::getEmpty();
    Driver.Driver = "TOMBSTONE_KEY";
    return Driver;
  }
  static unsigned getHashValue(const DriverArgs &Val) {
    unsigned FixedFieldsHash = llvm::hash_value(std::tuple{
        Val.Driver,
        Val.DriverFingerprint,
        Val.StandardIncludes,
        Val.StandardCXXIncludes,
        Val.Lang,
        Val.Sysroot,
        Val.ISysroot,
        Val.Target,
        Val.Stdlib,
    });

    unsigned SpecsHash = llvm::hash_combine_range(Val.Specs);

    return llvm::hash_combine(FixedFieldsHash, SpecsHash);
  }
  static bool isEqual(const DriverArgs &LHS, const DriverArgs &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm
namespace clang::clangd {
namespace {
class SuccessfulDriverQueryCache {
public:
  std::optional<DriverInfo> get(const DriverArgs &Args) const {
    std::lock_guard<std::mutex> Lock(Mu);
    auto Existing = Queries.find(Args);
    return Existing == Queries.end() ? std::nullopt
                                     : std::optional(Existing->second);
  }

  std::optional<DriverInfo> publish(const DriverArgs &Args,
                                    DriverQueryResult<DriverInfo> Result,
                                    bool Replace) {
    if (Result.Status != DriverQueryStatus::Success || !Result.Value)
      return std::nullopt;
    std::lock_guard<std::mutex> Lock(Mu);
    if (Replace) {
      Queries[Args] = std::move(*Result.Value);
      return Queries.lookup(Args);
    }
    auto Inserted = Queries.try_emplace(Args, std::move(*Result.Value));
    return Inserted.first->second;
  }

private:
  mutable std::mutex Mu;
  llvm::DenseMap<DriverArgs, DriverInfo> Queries;
};

class ScopedDriverQueryCache {
public:
  const DriverQueryResult<DriverInfo> *get(const DriverArgs &Args) const {
    auto Existing = Queries.find(Args);
    return Existing == Queries.end() ? nullptr : &Existing->second;
  }

  void publish(const DriverArgs &Args, DriverQueryResult<DriverInfo> Result) {
    Queries.try_emplace(Args, std::move(Result));
  }

  void clear() { Queries.clear(); }

private:
  llvm::DenseMap<DriverArgs, DriverQueryResult<DriverInfo>> Queries;
};

bool isValidTarget(llvm::StringRef Triple) {
  std::shared_ptr<TargetOptions> TargetOpts(new TargetOptions);
  TargetOpts->Triple = Triple.str();
  DiagnosticOptions DiagOpts;
  DiagnosticsEngine Diags(DiagnosticIDs::create(), DiagOpts,
                          new IgnoringDiagConsumer);
  llvm::IntrusiveRefCntPtr<TargetInfo> Target =
      TargetInfo::CreateTargetInfo(Diags, *TargetOpts);
  return bool(Target);
}

std::optional<DriverInfo> parseDriverOutput(llvm::StringRef Output) {
  DriverInfo Info;
  const char SIS[] = "#include <...> search starts here:";
  const char SIE[] = "End of search list.";
  const char TS[] = "Target: ";
  llvm::SmallVector<llvm::StringRef> Lines;
  Output.split(Lines, '\n', /*MaxSplit=*/-1, /*KeepEmpty=*/false);

  enum {
    Initial,            // Initial state: searching for target or includes list.
    IncludesExtracting, // Includes extracting.
    Done                // Includes and target extraction done.
  } State = Initial;
  bool SeenIncludes = false;
  bool SeenTarget = false;
  for (auto *It = Lines.begin(); State != Done && It != Lines.end(); ++It) {
    auto Line = *It;
    switch (State) {
    case Initial:
      if (!SeenIncludes && Line.trim() == SIS) {
        SeenIncludes = true;
        State = IncludesExtracting;
      } else if (!SeenTarget && Line.trim().starts_with(TS)) {
        SeenTarget = true;
        llvm::StringRef TargetLine = Line.trim();
        TargetLine.consume_front(TS);
        // Only detect targets that clang understands
        if (!isValidTarget(TargetLine)) {
          elog("System include extraction: invalid target \"{0}\", ignoring",
               TargetLine);
        } else {
          Info.Target = TargetLine.str();
          vlog("System include extraction: target extracted: \"{0}\"",
               TargetLine);
        }
      }
      break;
    case IncludesExtracting:
      if (Line.trim() == SIE) {
        State = SeenTarget ? Done : Initial;
      } else {
        Info.SystemIncludes.push_back(Line.trim().str());
        vlog("System include extraction: adding {0}", Line);
      }
      break;
    default:
      llvm_unreachable("Impossible state of the driver output parser");
      break;
    }
  }
  if (!SeenIncludes) {
    elog("System include extraction: start marker not found: {0}", Output);
    return std::nullopt;
  }
  if (State == IncludesExtracting) {
    elog("System include extraction: end marker missing: {0}", Output);
    return std::nullopt;
  }
  return std::move(Info);
}

DriverQueryResult<std::string>
run(llvm::ArrayRef<llvm::StringRef> Argv, bool OutputIsStderr,
    std::chrono::milliseconds QueryTimeout = std::chrono::seconds(30)) {
  llvm::SmallString<128> OutputPath;
  if (auto EC = llvm::sys::fs::createTemporaryFile("system-includes", "clangd",
                                                   OutputPath)) {
    elog("System include extraction: failed to create temporary file with "
         "error {0}",
         EC.message());
    return {DriverQueryStatus::Failed, std::nullopt};
  }
  llvm::scope_exit CleanUp(
      [&OutputPath]() { llvm::sys::fs::remove(OutputPath); });

  std::optional<llvm::StringRef> Redirects[] = {{""}, {""}, {""}};
  Redirects[OutputIsStderr ? 2 : 1] = OutputPath.str();

  std::string ErrMsg;
  bool ExecutionFailed = false;
  llvm::sys::ProcessInfo Process = llvm::sys::ExecuteNoWait(
      Argv.front(), Argv, /*Env=*/std::nullopt, Redirects,
      /*MemoryLimit=*/0, &ErrMsg, &ExecutionFailed,
      /*AffinityMask=*/nullptr, /*DetachProcess=*/false,
      /*OwnProcessTree=*/true);
  if (ExecutionFailed || Process.Pid == llvm::sys::ProcessInfo::InvalidPid) {
    elog("System include extraction: failed to start driver: {0}. Args: [{1}]",
         ErrMsg, printArgv(Argv));
    return {DriverQueryStatus::Failed, std::nullopt};
  }
  constexpr auto PollInterval = std::chrono::milliseconds(10);
  const auto Deadline = std::chrono::steady_clock::now() + QueryTimeout;
  int RC = 0;
  for (;;) {
    llvm::sys::ProcessInfo Result = llvm::sys::Wait(
        Process, /*SecondsToWait=*/0, &ErrMsg, /*ProcStat=*/nullptr,
        /*Polling=*/true);
    if (Result.Pid != llvm::sys::ProcessInfo::InvalidPid) {
      RC = Result.ReturnCode;
      break;
    }
    const bool Cancelled = isCancelled() != 0;
    const bool TimedOut = std::chrono::steady_clock::now() >= Deadline;
    if (Cancelled || TimedOut) {
      std::string StopError;
      llvm::sys::TerminateProcessTree(Process, &StopError);
      elog("System include extraction: driver execution {0}{1}{2}. Args: [{3}]",
           Cancelled ? "cancelled" : "timed out", StopError.empty() ? "" : ": ",
           StopError.empty() ? "" : StopError, printArgv(Argv));
      return {Cancelled ? DriverQueryStatus::Cancelled
                        : DriverQueryStatus::TimedOut,
              std::nullopt};
    }
    std::this_thread::sleep_for(PollInterval);
  }
  if (RC != 0) {
    elog("System include extraction: driver execution failed with return code: "
         "{0} - '{1}'. Args: [{2}]",
         llvm::to_string(RC), ErrMsg, printArgv(Argv));
    return {DriverQueryStatus::Failed, std::nullopt};
  }

  auto BufOrError = llvm::MemoryBuffer::getFile(OutputPath);
  if (!BufOrError) {
    elog("System include extraction: failed to read {0} with error {1}",
         OutputPath, BufOrError.getError().message());
    return {DriverQueryStatus::Failed, std::nullopt};
  }
  return {DriverQueryStatus::Success,
          BufOrError.get().get()->getBuffer().str()};
}

std::optional<std::string>
resolveQueryDriver(llvm::StringRef InputDriver,
                   const llvm::Regex &QueryDriverRegex) {
  std::string Driver = InputDriver.str();
  if (!llvm::sys::path::is_absolute(Driver)) {
    auto DriverProgram = llvm::sys::findProgramByName(Driver);
    if (DriverProgram) {
      vlog("System include extraction: driver {0} expanded to {1}", Driver,
           *DriverProgram);
      Driver = *DriverProgram;
    } else {
      elog("System include extraction: driver {0} not found in PATH", Driver);
      return std::nullopt;
    }
  }

  // If driver was "../foo" then having to allowlist "/path/a/../foo" rather
  // than "/path/foo" is absurd. Allow either spelling to match, preserving the
  // historical path for execution.
  llvm::SmallString<256> NoDots(Driver);
  llvm::sys::path::remove_dots(NoDots, /*remove_dot_dot=*/true);
  if (!QueryDriverRegex.match(Driver) && !QueryDriverRegex.match(NoDots)) {
    vlog("System include extraction: not allowed driver {0}", Driver);
    return std::nullopt;
  }
  return Driver;
}

DriverQueryResult<DriverInfo>
extractSystemIncludesAndTarget(const DriverArgs &InputArgs) {
  trace::Span Tracer("Extract system includes and target");

  const std::string &Driver = InputArgs.Driver;

  SPAN_ATTACH(Tracer, "driver", Driver);
  SPAN_ATTACH(Tracer, "lang", InputArgs.Lang);

  llvm::SmallVector<llvm::StringRef> Args = {Driver, "-E", "-v"};
  Args.append(InputArgs.render());
  // Input needs to go after Lang flags.
  Args.push_back("-");
  auto Output = run(Args, /*OutputIsStderr=*/true);
  if (Output.Status != DriverQueryStatus::Success)
    return {Output.Status, std::nullopt};

  std::optional<DriverInfo> Info = parseDriverOutput(*Output.Value);
  if (!Info)
    return {DriverQueryStatus::Failed, std::nullopt};

  switch (Config::current().CompileFlags.BuiltinHeaders) {
  case Config::BuiltinHeaderPolicy::Clangd: {
    // The built-in headers are tightly coupled to parser builtins.
    // (These are clang's "resource dir", GCC's GCC_INCLUDE_DIR.)
    // We should keep using clangd's versions, so exclude the queried
    // builtins. They're not specially marked in the -v output, but we can
    // get the path with `$DRIVER -print-file-name=include`.
    auto BuiltinHeaders = run({Driver, "-print-file-name=include"},
                              /*OutputIsStderr=*/false);
    if (BuiltinHeaders.Status != DriverQueryStatus::Success)
      return {BuiltinHeaders.Status, std::nullopt};
    auto Path = llvm::StringRef(*BuiltinHeaders.Value).trim();
    if (!Path.empty() && llvm::sys::path::is_absolute(Path)) {
      auto Size = Info->SystemIncludes.size();
      llvm::erase(Info->SystemIncludes, Path);
      vlog("System includes extractor: builtin headers {0} {1}", Path,
           (Info->SystemIncludes.size() != Size)
               ? "excluded"
               : "not found in driver's response");
    }
    break;
  }
  case Config::BuiltinHeaderPolicy::QueryDriver:
    vlog("System includes extractor: Using builtin headers from query driver.");
    break;
  }

  log("System includes extractor: successfully executed {0}\n\tgot includes: "
      "\"{1}\"\n\tgot target: \"{2}\"",
      Driver, llvm::join(Info->SystemIncludes, ", "), Info->Target);
  return {DriverQueryStatus::Success, std::move(Info)};
}

tooling::CompileCommand &
addSystemIncludes(tooling::CompileCommand &Cmd,
                  llvm::ArrayRef<std::string> SystemIncludes) {
  std::vector<std::string> ToAppend;
  for (llvm::StringRef Include : SystemIncludes) {
    // FIXME(kadircet): This doesn't work when we have "--driver-mode=cl"
    ToAppend.push_back("-isystem");
    ToAppend.push_back(Include.str());
  }
  if (!ToAppend.empty()) {
    // Just append when `--` isn't present.
    auto InsertAt = llvm::find(Cmd.CommandLine, "--");
    Cmd.CommandLine.insert(InsertAt, std::make_move_iterator(ToAppend.begin()),
                           std::make_move_iterator(ToAppend.end()));
  }
  return Cmd;
}

tooling::CompileCommand &setTarget(tooling::CompileCommand &Cmd,
                                   const std::string &Target) {
  if (!Target.empty()) {
    // We do not want to override existing target with extracted one.
    for (llvm::StringRef Arg : Cmd.CommandLine) {
      if (Arg == "-target" || Arg.starts_with("--target="))
        return Cmd;
    }
    // Just append when `--` isn't present.
    auto InsertAt = llvm::find(Cmd.CommandLine, "--");
    Cmd.CommandLine.insert(InsertAt, "--target=" + Target);
  }
  return Cmd;
}

/// Converts a glob containing only ** or * into a regex.
std::string convertGlobToRegex(llvm::StringRef Glob) {
  std::string RegText;
  llvm::raw_string_ostream RegStream(RegText);
  RegStream << '^';
  for (size_t I = 0, E = Glob.size(); I < E; ++I) {
    if (Glob[I] == '*') {
      if (I + 1 < E && Glob[I + 1] == '*') {
        // Double star, accept any sequence.
        RegStream << ".*";
        // Also skip the second star.
        ++I;
      } else {
        // Single star, accept any sequence without a slash.
        RegStream << "[^/]*";
      }
    } else if (llvm::sys::path::is_separator(Glob[I]) &&
               llvm::sys::path::is_separator('/') &&
               llvm::sys::path::is_separator('\\')) {
      RegStream << R"([/\\])"; // Accept either slash on windows.
    } else {
      RegStream << llvm::Regex::escape(Glob.substr(I, 1));
    }
  }
  RegStream << '$';
  return RegText;
}

/// Converts a glob containing only ** or * into a regex.
llvm::Regex convertGlobsToRegex(llvm::ArrayRef<std::string> Globs) {
  assert(!Globs.empty() && "Globs cannot be empty!");
  std::vector<std::string> RegTexts;
  RegTexts.reserve(Globs.size());
  for (llvm::StringRef Glob : Globs)
    RegTexts.push_back(convertGlobToRegex(Glob));

  // Tempting to pass IgnoreCase, but we don't know the FS sensitivity.
  llvm::Regex Reg(llvm::join(RegTexts, "|"));
  assert(Reg.isValid(RegTexts.front()) &&
         "Created an invalid regex from globs");
  return Reg;
}

/// Extracts system includes from a trusted driver by parsing the output of
/// include search path and appends them to the commands coming from underlying
/// compilation database.
class SystemIncludeExtractor {
public:
  SystemIncludeExtractor(llvm::ArrayRef<std::string> QueryDriverGlobs)
      : Shared(std::make_shared<State>()),
        QueryDriverRegex(convertGlobsToRegex(QueryDriverGlobs)) {}

  void operator()(tooling::CompileCommand &Cmd, llvm::StringRef File) const {
    if (Cmd.CommandLine.empty())
      return;

    struct ThreadCache {
      uint64_t Scope = 0;
      ScopedDriverQueryCache Queries;
    };
    thread_local std::map<const State *, ThreadCache> ScopedCaches;
    ThreadCache *Scoped = nullptr;
    if (ActiveRefreshScope) {
      Scoped = &ScopedCaches[Shared.get()];
      if (Scoped->Scope != ActiveRefreshScope) {
        Scoped->Scope = ActiveRefreshScope;
        Scoped->Queries.clear();
      }
    }
    DriverArgs Args(Cmd, File, /*DriverFingerprint=*/"");
    if (Args.Lang.empty())
      return;
    auto Driver = resolveQueryDriver(Args.Driver, QueryDriverRegex);
    if (!Driver)
      return;
    Args.Driver = std::move(*Driver);
    CompileCommandDriverFingerprintCache *Fingerprints =
        ActiveDriverFingerprints ? ActiveDriverFingerprints
                                 : &Shared->DriverFingerprints;
    Args.DriverFingerprint = Fingerprints->get(Cmd);
    if (ActiveDriverFingerprints)
      Shared->DriverFingerprints.update(Cmd, Args.DriverFingerprint);
    std::optional<DriverInfo> Info;
    if (Scoped && RefreshQueriesInActiveScope) {
      if (const auto *Existing = Scoped->Queries.get(Args)) {
        Info = Existing->Value;
      } else {
        auto Queried = extractSystemIncludesAndTarget(Args);
        if (Queried.Status == DriverQueryStatus::Success)
          Info = Shared->Queries.publish(Args, Queried, /*Replace=*/true);
        Scoped->Queries.publish(Args, std::move(Queried));
      }
    } else {
      Info = Shared->Queries.get(Args);
      if (!Info)
        Info =
            Shared->Queries.publish(Args, extractSystemIncludesAndTarget(Args),
                                    /*Replace=*/false);
    }
    if (Info) {
      setTarget(addSystemIncludes(Cmd, Info->SystemIncludes), Info->Target);
    }
  }

private:
  struct State {
    SuccessfulDriverQueryCache Queries;
    CompileCommandDriverFingerprintCache DriverFingerprints;
  };
  std::shared_ptr<State> Shared;
  llvm::Regex QueryDriverRegex;
};
} // namespace

std::optional<std::string>
runSystemIncludeExtractorForTest(llvm::ArrayRef<llvm::StringRef> Argv,
                                 bool OutputIsStderr,
                                 std::chrono::milliseconds Timeout) {
  return run(Argv, OutputIsStderr, Timeout).Value;
}

unsigned
runSystemIncludeExtractorCacheForTest(llvm::ArrayRef<unsigned> QueryStatuses,
                                      bool OperationLocal) {
  SuccessfulDriverQueryCache Cache;
  ScopedDriverQueryCache ScopedCache;
  DriverArgs Args = DriverArgs::getEmpty();
  Args.Driver = "test-driver";
  Args.Lang = "c++";
  unsigned Queries = 0;
  for (unsigned RawStatus : QueryStatuses) {
    if (OperationLocal ? ScopedCache.get(Args) != nullptr
                       : Cache.get(Args).has_value())
      continue;
    ++Queries;
    assert(RawStatus <= unsigned(DriverQueryStatus::TimedOut));
    const auto Status = static_cast<DriverQueryStatus>(RawStatus);
    DriverQueryResult<DriverInfo> Result{Status,
                                         Status == DriverQueryStatus::Success
                                             ? std::optional(DriverInfo{})
                                             : std::nullopt};
    if (OperationLocal)
      ScopedCache.publish(Args, std::move(Result));
    else
      Cache.publish(Args, std::move(Result), /*Replace=*/false);
  }
  return Queries;
}

SystemIncludeExtractorScope::SystemIncludeExtractorScope(
    bool RefreshQueries,
    std::shared_ptr<CompileCommandDriverFingerprintCache> DriverFingerprints)
    : Previous(ActiveRefreshScope),
      PreviousRefresh(RefreshQueriesInActiveScope),
      PreviousDriverFingerprints(ActiveDriverFingerprints),
      DriverFingerprints(
          DriverFingerprints
              ? std::move(DriverFingerprints)
              : std::make_shared<CompileCommandDriverFingerprintCache>()) {
  ActiveRefreshScope = ++NextRefreshScope;
  RefreshQueriesInActiveScope = RefreshQueries;
  ActiveDriverFingerprints = DriverFingerprints.get();
}

SystemIncludeExtractorScope::~SystemIncludeExtractorScope() {
  ActiveRefreshScope = Previous;
  RefreshQueriesInActiveScope = PreviousRefresh;
  ActiveDriverFingerprints = PreviousDriverFingerprints;
}

CompileCommandDriverFingerprintCache &
SystemIncludeExtractorScope::driverFingerprints() {
  return *DriverFingerprints;
}

SystemIncludeExtractorFn
getSystemIncludeExtractor(llvm::ArrayRef<std::string> QueryDriverGlobs) {
  if (QueryDriverGlobs.empty())
    return nullptr;
  return SystemIncludeExtractor(QueryDriverGlobs);
}

} // namespace clang::clangd
