//===-- Background.cpp - Build an index in a background thread ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "index/Background.h"
#include "CompileCommands.h"
#include "Compiler.h"
#include "Config.h"
#include "FS.h"
#include "Headers.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "URI.h"
#include "index/BackgroundIndexLoader.h"
#include "index/FileIndex.h"
#include "index/Index.h"
#include "index/IndexAction.h"
#include "index/MemIndex.h"
#include "index/Ref.h"
#include "index/Relation.h"
#include "index/Serialization.h"
#include "index/Symbol.h"
#include "index/SymbolCollector.h"
#include "support/Cancellation.h"
#include "support/Context.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "support/Threading.h"
#include "support/ThreadsafeFS.h"
#include "support/Trace.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Stack.h"
#include "clang/Basic/Version.h"
#include "clang/Driver/Types.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {
namespace {

// We cannot use vfs->makeAbsolute because Cmd.FileName is either absolute or
// relative to Cmd.Directory, which might not be the same as current working
// directory.
llvm::SmallString<128> getAbsolutePath(const tooling::CompileCommand &Cmd) {
  llvm::SmallString<128> AbsolutePath;
  if (llvm::sys::path::is_absolute(Cmd.Filename)) {
    AbsolutePath = Cmd.Filename;
  } else {
    AbsolutePath = Cmd.Directory;
    llvm::sys::path::append(AbsolutePath, Cmd.Filename);
    llvm::sys::path::remove_dots(AbsolutePath, true);
  }
  return AbsolutePath;
}

bool shardIsStale(const LoadedShard &LS, llvm::vfs::FileSystem *FS) {
  auto Buf = FS->getBufferForFile(LS.AbsolutePath);
  if (!Buf) {
    vlog("Background-index: Couldn't read {0} to validate stored index: {1}",
         LS.AbsolutePath, Buf.getError().message());
    // There is no point in indexing an unreadable file.
    return false;
  }
  return digest(Buf->get()->getBuffer()) != LS.Digest;
}

class GraphDiagnosticConsumer : public IgnoreDiagnostics {
public:
  void BeginSourceFile(const LangOptions &Options,
                       const Preprocessor *) override {
    Lang = Options;
  }

  void HandleDiagnostic(DiagnosticsEngine::Level Level,
                        const clang::Diagnostic &Info) override {
    IgnoreDiagnostics::HandleDiagnostic(Level, Info);
    if (Level == DiagnosticsEngine::Ignored)
      return;
    GraphDiagnostic Result;
    llvm::SmallString<128> Message;
    Info.FormatDiagnostic(Message);
    Result.Message = Message.str().str();
    Result.Code = "clang:" + std::to_string(Info.getID());
    switch (Level) {
    case DiagnosticsEngine::Fatal:
    case DiagnosticsEngine::Error:
      Result.Severity = "error";
      break;
    case DiagnosticsEngine::Warning:
      Result.Severity = "warning";
      break;
    case DiagnosticsEngine::Remark:
    case DiagnosticsEngine::Note:
      Result.Severity = "info";
      break;
    case DiagnosticsEngine::Ignored:
      llvm_unreachable("ignored diagnostic was filtered");
    }
    if (Info.hasSourceManager() && Info.getLocation().isValid()) {
      const auto &SM = Info.getSourceManager();
      SourceLocation Location = SM.getFileLoc(Info.getLocation());
      FileID FID = SM.getFileID(Location);
      if (auto File = SM.getFileEntryRefForID(FID)) {
        auto Canonical = getCanonicalPath(*File, SM.getFileManager());
        if (Canonical)
          Result.Range.FileURI = URI::create(*Canonical).toString();
        WithContextValue FixedEncoding(kCurrentOffsetEncoding,
                                       OffsetEncoding::UTF16);
        Position Begin = sourceLocToPosition(SM, Location);
        unsigned Length =
            Lang ? Lexer::MeasureTokenLength(Location, SM, *Lang) : 1;
        Position End = sourceLocToPosition(
            SM, Location.getLocWithOffset(std::max(Length, 1u)));
        Result.Range.StartLine = Begin.line;
        Result.Range.StartColumn = Begin.character;
        Result.Range.EndLine = End.line;
        Result.Range.EndColumn = End.character;
      }
    }
    Diagnostics.push_back(std::move(Result));
  }

  std::vector<GraphDiagnostic> take() { return std::move(Diagnostics); }

private:
  std::optional<LangOptions> Lang;
  std::vector<GraphDiagnostic> Diagnostics;
};

template <typename... Args>
llvm::Error contentModified(const char *Format, Args &&...Arguments) {
  return llvm::make_error<LSPError>(
      llvm::formatv(Format, std::forward<Args>(Arguments)...).str(),
      ErrorCode::ContentModified);
}

std::string graphMainKey(llvm::StringRef Path) {
  return maybeCaseFoldPath(llvm::sys::path::convert_to_slash(removeDots(Path)));
}

uint64_t elapsedMillis(std::chrono::steady_clock::time_point Started) {
  auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - Started)
                     .count();
  return static_cast<uint64_t>(std::max<int64_t>(Elapsed, 1));
}

std::vector<std::string> graphMissingIncludeCandidates(
    const GraphTU &Graph, llvm::ArrayRef<std::string> ParsedIncludeRoots,
    llvm::ArrayRef<std::string> ParsedForcedIncludes) {
  std::set<std::string> IncludeRoots;
  IncludeRoots.insert(Graph.Directory);
  for (llvm::StringRef Root : ParsedIncludeRoots) {
    if (!Root.empty()) {
      llvm::SmallString<128> Absolute(Root);
      if (!llvm::sys::path::is_absolute(Absolute)) {
        Absolute = Graph.Directory;
        llvm::sys::path::append(Absolute, Root);
      }
      llvm::sys::path::remove_dots(Absolute, true);
      IncludeRoots.insert(Absolute.str().str());
    }
  }

  std::set<std::string> Candidates;
  auto AddCandidate = [&](llvm::StringRef Spelling,
                          std::optional<llvm::StringRef> IncludingFile) {
    if (Spelling.empty())
      return;
    if (llvm::sys::path::is_absolute(Spelling)) {
      Candidates.insert(removeDots(Spelling));
      return;
    }
    if (IncludingFile) {
      llvm::SmallString<128> Candidate(
          llvm::sys::path::parent_path(*IncludingFile));
      llvm::sys::path::append(Candidate, Spelling);
      llvm::sys::path::remove_dots(Candidate, true);
      Candidates.insert(Candidate.str().str());
    }
    for (const auto &Root : IncludeRoots) {
      llvm::SmallString<128> Candidate(Root);
      llvm::sys::path::append(Candidate, Spelling);
      llvm::sys::path::remove_dots(Candidate, true);
      Candidates.insert(Candidate.str().str());
    }
  };
  for (const auto &Missing : Graph.MissingIncludes) {
    std::optional<std::string> Source;
    if (!Missing.Angled) {
      auto Resolved = URI::resolve(Missing.SourceURI, Graph.MainFile);
      if (Resolved)
        Source = std::move(*Resolved);
      else
        llvm::consumeError(Resolved.takeError());
    }
    AddCandidate(Missing.Spelling, Source
                                       ? std::optional<llvm::StringRef>(*Source)
                                       : std::nullopt);
  }

  // These spellings and roots come from CompilerInvocation, after Clang's
  // option table has decoded joined/separate GNU and clang-cl forms. Forced
  // includes originate from synthetic <built-in>, so PPCallbacks alone cannot
  // identify the concrete file that should unblock this failed graph.
  for (llvm::StringRef Spelling : ParsedForcedIncludes)
    AddCandidate(Spelling, Graph.MainFile);
  return {Candidates.begin(), Candidates.end()};
}

} // namespace

BackgroundIndex::BackgroundIndex(
    const ThreadsafeFS &TFS, const GlobalCompilationDatabase &CDB,
    BackgroundIndexStorage::Factory IndexStorageFactory, Options Opts)
    : SwapIndex(std::make_unique<MemIndex>()), TFS(TFS), CDB(CDB),
      IndexingPriority(Opts.IndexingPriority),
      ContextProvider(std::move(Opts.ContextProvider)),
      IndexedSymbols(IndexContents::All, Opts.SupportContainedRefs),
      Rebuilder(this, &IndexedSymbols, Opts.ThreadPoolSize),
      IndexStorageFactory(std::move(IndexStorageFactory)),
      Queue(std::move(Opts.OnProgress)),
      CommandsChanged(
          CDB.watch([&](const std::vector<std::string> &ChangedFiles) {
            enqueue(ChangedFiles);
          })) {
  assert(Opts.ThreadPoolSize > 0 && "Thread pool size can't be zero.");
  assert(this->IndexStorageFactory && "Storage factory can not be null!");
  for (unsigned I = 0; I < Opts.ThreadPoolSize; ++I) {
    ThreadPool.runAsync("background-worker-" + llvm::Twine(I + 1),
                        [this, Ctx(Context::current().clone())]() mutable {
                          clang::noteBottomOfStack();
                          WithContext BGContext(std::move(Ctx));
                          Queue.work([&] { Rebuilder.idle(); });
                        });
  }
}

BackgroundIndex::~BackgroundIndex() {
  stop();
  ThreadPool.wait();
}

void BackgroundIndex::enqueue(const std::vector<std::string> &ChangedFiles) {
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    ++GraphDiscoveryPending;
  }
  Queue.push(changedFilesTask(ChangedFiles));
}

BackgroundQueue::Task BackgroundIndex::changedFilesTask(
    const std::vector<std::string> &ChangedFiles) {
  BackgroundQueue::Task T([this, ChangedFiles] {
    trace::Span Tracer("BackgroundIndexEnqueue");

    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(/*Path=*/""));

    // We're doing this asynchronously, because we'll read shards here too.
    log("Enqueueing {0} commands for indexing", ChangedFiles.size());
    SPAN_ATTACH(Tracer, "files", int64_t(ChangedFiles.size()));

    auto DriverFingerprints =
        std::make_shared<CompileCommandDriverFingerprintCache>();
    SystemIncludeExtractorScope SystemIncludes(/*RefreshQueries=*/false,
                                               DriverFingerprints);
    auto NeedsReIndexing = loadProject(std::move(ChangedFiles));
    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      assert(GraphDiscoveryPending > 0);
      --GraphDiscoveryPending;
      for (const auto &File : NeedsReIndexing)
        GraphPending.insert(graphMainKey(File));
    }
    // Run indexing for files that need to be updated.
    std::shuffle(NeedsReIndexing.begin(), NeedsReIndexing.end(),
                 std::mt19937(std::random_device{}()));
    std::vector<BackgroundQueue::Task> Tasks;
    Tasks.reserve(NeedsReIndexing.size());
    for (const auto &File : NeedsReIndexing)
      Tasks.push_back(indexFileTask(File, DriverFingerprints));
    Queue.append(std::move(Tasks));
  });

  T.QueuePri = LoadShards;
  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
}

void BackgroundIndex::enqueueGraphReindex(
    llvm::ArrayRef<std::string> MainFiles) {
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    for (const auto &File : MainFiles)
      GraphPending.insert(graphMainKey(File));
  }
  std::vector<BackgroundQueue::Task> Tasks;
  Tasks.reserve(MainFiles.size());
  auto DriverFingerprints =
      std::make_shared<CompileCommandDriverFingerprintCache>();
  for (const auto &File : MainFiles)
    Tasks.push_back(indexFileTask(File, DriverFingerprints));
  Queue.append(std::move(Tasks));
}

static llvm::StringRef filenameWithoutExtension(llvm::StringRef Path) {
  Path = llvm::sys::path::filename(Path);
  return Path.drop_back(llvm::sys::path::extension(Path).size());
}

static bool isGraphTranslationUnit(llvm::StringRef Path) {
  namespace types = clang::driver::types;
  llvm::StringRef Extension = llvm::sys::path::extension(Path);
  if (Extension.consume_front(".")) {
    const auto Type = types::lookupTypeForExtension(Extension);
    return Type != types::TY_INVALID && types::isDerivedFromC(Type) &&
           !types::isObjC(Type) && !types::isCuda(Type) &&
           !types::isHIP(Type) && !types::isOpenCL(Type) &&
           !types::isHLSL(Type) && !types::onlyPrecompileType(Type);
  }
  return false;
}

BackgroundQueue::Task BackgroundIndex::indexFileTask(
    std::string Path,
    std::shared_ptr<CompileCommandDriverFingerprintCache> DriverFingerprints) {
  std::string Tag = filenameWithoutExtension(Path).str();
  uint64_t Key = llvm::xxh3_64bits(graphMainKey(Path));
  BackgroundQueue::Task T([this, Path(std::move(Path)),
                           DriverFingerprints(std::move(DriverFingerprints))] {
    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(Path));
    SystemIncludeExtractorScope SystemIncludes(/*RefreshQueries=*/false,
                                               DriverFingerprints);
    auto Commands = CDB.getCompileCommands(Path);
    std::optional<std::string> LegacyCommandDigest;
    std::optional<tooling::CompileCommand> UnsupportedLegacyCommand;
    if (!Commands.empty())
      LegacyCommandDigest = graphCommandDigest(Commands.front());
    if (!Commands.empty() && !graphCommandIsCOrCXX(Commands.front()))
      UnsupportedLegacyCommand = Commands.front();
    std::map<std::string, tooling::CompileCommand> Configurations;
    for (auto &Command : Commands)
      if (graphCommandIsCOrCXX(Command))
        Configurations.try_emplace(graphCommandDigest(Command),
                                   std::move(Command));
    const std::string GraphKey = graphMainKey(Path);

    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      GraphPending.insert(GraphKey);
      GraphFailures.erase(GraphKey);
      GraphFailureTUs.erase(GraphKey);
      GraphFailureInputs.erase(GraphKey);
    }

    auto Fail = [&](llvm::StringRef Message,
                    const std::vector<std::string> &MissingInputs) {
      std::lock_guard<std::mutex> Lock(GraphMu);
      GraphPending.erase(GraphKey);
      GraphFailures[GraphKey] = Message.str();
      GraphFailureTUs[GraphKey] = Path;
      if (MissingInputs.empty())
        GraphFailureInputs.erase(GraphKey);
      else
        GraphFailureInputs[GraphKey] = MissingInputs;
      ++GraphRevision;
    };

    if (Configurations.empty()) {
      if (UnsupportedLegacyCommand) {
        auto Legacy = index(std::move(*UnsupportedLegacyCommand),
                            &SystemIncludes.driverFingerprints());
        if (Legacy) {
          Legacy->Index.Graphs.clear();
          update(Path, std::move(Legacy->Index), Legacy->ShardVersionsSnapshot,
                 Legacy->HadErrors);
          Rebuilder.indexedTU();
        } else {
          elog("Indexing unsupported graph language {0} failed: {1}", Path,
               llvm::toString(Legacy.takeError()));
        }
      }
      std::lock_guard<std::mutex> Lock(GraphMu);
      GraphPending.erase(GraphKey);
      GraphFailures.erase(GraphKey);
      GraphFailureTUs.erase(GraphKey);
      GraphFailureInputs.erase(GraphKey);
      if (Graphs.erase(GraphKey))
        ++GraphRevision;
      GraphSemanticMillis.erase(GraphKey);
      return;
    }

    std::optional<std::string> Failure;
    std::vector<IndexResult> Results;
    Results.reserve(Configurations.size() + (UnsupportedLegacyCommand ? 1 : 0));
    if (UnsupportedLegacyCommand) {
      auto Legacy = index(std::move(*UnsupportedLegacyCommand),
                          &SystemIncludes.driverFingerprints());
      if (Legacy) {
        Legacy->Index.Graphs.clear();
        Results.push_back(std::move(*Legacy));
      } else {
        elog("Indexing unsupported graph language {0} failed: {1}", Path,
             llvm::toString(Legacy.takeError()));
      }
    }
    for (auto &Configuration : Configurations) {
      auto Result = index(std::move(Configuration.second),
                          &SystemIncludes.driverFingerprints());
      if (!Result) {
        std::string Message = llvm::toString(Result.takeError());
        elog("Indexing {0} failed: {1}", Path, Message);
        if (!Failure)
          Failure = std::move(Message);
        continue;
      }
      if (!Result->Index.Graphs.empty() &&
          (Result->HadErrors ||
           !Result->Index.Graphs.front().MissingIncludes.empty())) {
        std::string Message = Result->HadErrors
                                  ? "compiler diagnostics contain errors"
                                  : "compiler left includes unresolved";
        elog("Indexing {0} did not produce a publishable graph: {1}", Path,
             Message);
        if (!Failure)
          Failure = std::move(Message);
      }
      Results.push_back(std::move(*Result));
    }

    std::set<std::string> MissingInputs;
    if (Failure)
      for (const auto &Result : Results)
        for (const auto &Graph : Result.Index.Graphs) {
          auto FS = TFS.view(Graph.Directory);
          for (auto &Candidate : Result.MissingInputs)
            if (!FS->exists(Candidate))
              MissingInputs.insert(std::move(Candidate));
        }

    std::vector<GraphTU> CompleteGraphs;
    std::map<std::string, uint64_t> CompleteTimings;
    if (!Failure) {
      CompleteGraphs.reserve(Results.size());
      for (const auto &Result : Results) {
        assert(Result.Index.Graphs.size() <= 1);
        if (!Result.Index.Graphs.empty()) {
          CompleteGraphs.push_back(Result.Index.Graphs.front());
          CompleteTimings[Result.Index.Graphs.front().CommandDigest] =
              Result.SemanticMillis;
        }
      }
    } else {
      // An erroneous or incomplete batch must not replace the last complete
      // graph generation, including in the persisted main-file shard.
      std::lock_guard<std::mutex> Lock(GraphMu);
      auto Published = Graphs.find(GraphKey);
      if (Published != Graphs.end())
        for (const auto &Configuration : Published->second)
          CompleteGraphs.push_back(Configuration.second);
    }

    // Preserve clangd's pre-graph behavior: one representative command owns
    // the ordinary slabs, and even a diagnostic result updates them. All
    // configurations are analyzed only for the complete graph generation.
    auto Legacy = llvm::find_if(Results, [&](const IndexResult &Result) {
      return LegacyCommandDigest && Result.Index.Cmd &&
             graphCommandDigest(*Result.Index.Cmd) == *LegacyCommandDigest;
    });
    if (Legacy != Results.end()) {
      Legacy->Index.Graphs = CompleteGraphs;
      update(Path, std::move(Legacy->Index), Legacy->ShardVersionsSnapshot,
             Legacy->HadErrors);
      Rebuilder.indexedTU();
    }

    if (Failure) {
      Fail(*Failure, std::vector<std::string>(MissingInputs.begin(),
                                              MissingInputs.end()));
      return;
    }

    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      Graphs.erase(GraphKey);
      GraphSemanticMillis.erase(GraphKey);
      if (!CompleteGraphs.empty()) {
        auto &Published = Graphs[GraphKey];
        auto &Timings = GraphSemanticMillis[GraphKey];
        for (auto &Graph : CompleteGraphs) {
          Timings[Graph.CommandDigest] = CompleteTimings[Graph.CommandDigest];
          Published.emplace(Graph.CommandDigest, std::move(Graph));
        }
      }
      GraphPending.erase(GraphKey);
      GraphFailures.erase(GraphKey);
      GraphFailureTUs.erase(GraphKey);
      GraphFailureInputs.erase(GraphKey);
      ++GraphRevision;
    }
  });
  T.QueuePri = IndexFile;
  T.ThreadPri = IndexingPriority;
  T.Tag = std::move(Tag);
  T.Key = Key;
  T.Repeatable = true;
  return T;
}

void BackgroundIndex::boostRelated(llvm::StringRef Path) {
  if (isHeaderFile(Path))
    Queue.boost(filenameWithoutExtension(Path), IndexBoostedFile);
}

/// Given index results from a TU, only update symbols coming from files that
/// are different or missing from than \p ShardVersionsSnapshot. Also stores new
/// index information on IndexStorage.
void BackgroundIndex::update(
    llvm::StringRef MainFile, IndexFileIn Index,
    const llvm::StringMap<ShardVersion> &ShardVersionsSnapshot,
    bool HadErrors) {
  // Keys are URIs.
  llvm::StringMap<std::pair<Path, FileDigest>> FilesToUpdate;
  // Note that sources do not contain any information regarding missing headers,
  // since we don't even know what absolute path they should fall in.
  for (const auto &IndexIt : *Index.Sources) {
    const auto &IGN = IndexIt.getValue();
    auto AbsPath = URI::resolve(IGN.URI, MainFile);
    if (!AbsPath) {
      elog("Failed to resolve URI: {0}", AbsPath.takeError());
      continue;
    }
    const auto DigestIt = ShardVersionsSnapshot.find(*AbsPath);
    const bool OwnsCompleteGraph =
        llvm::any_of(Index.Graphs, [&](const GraphTU &Graph) {
          return Graph.MainFileURI == IGN.URI;
        });
    // File has different contents, or indexing was successful this time.
    // A main-file shard also owns all complete compile-command views. Persist
    // it even when source bytes are unchanged, as configuration and toolchain
    // changes can still replace those views.
    if (OwnsCompleteGraph || DigestIt == ShardVersionsSnapshot.end() ||
        DigestIt->getValue().Digest != IGN.Digest ||
        (DigestIt->getValue().HadErrors && !HadErrors))
      FilesToUpdate[IGN.URI] = {std::move(*AbsPath), IGN.Digest};
  }

  // Shard slabs into files.
  FileShardedIndex ShardedIndex(std::move(Index));

  // Build and store new slabs for each updated file.
  for (const auto &FileIt : FilesToUpdate) {
    auto Uri = FileIt.first();
    auto IF = ShardedIndex.getShard(Uri);
    assert(IF && "no shard for file in Index.Sources?");
    PathRef Path = FileIt.getValue().first;

    // Only store command line hash for main files of the TU, since our
    // current model keeps only one version of a header file.
    if (Path != MainFile)
      IF->Cmd.reset();

    // We need to store shards before updating the index, since the latter
    // consumes slabs.
    // FIXME: Also skip serializing the shard if it is already up-to-date.
    if (auto Error = IndexStorageFactory(Path)->storeShard(Path, *IF))
      elog("Failed to write background-index shard for file {0}: {1}", Path,
           std::move(Error));

    {
      std::lock_guard<std::mutex> Lock(ShardVersionsMu);
      const auto &Hash = FileIt.getValue().second;
      auto DigestIt = ShardVersions.try_emplace(Path);
      ShardVersion &SV = DigestIt.first->second;
      // Skip if file is already up to date, unless previous index was broken
      // and this one is not.
      if (!DigestIt.second && SV.Digest == Hash && SV.HadErrors && !HadErrors)
        continue;
      SV.Digest = Hash;
      SV.HadErrors = HadErrors;

      // This can override a newer version that is added in another thread, if
      // this thread sees the older version but finishes later. This should be
      // rare in practice.
      IndexedSymbols.update(
          Uri, std::make_unique<SymbolSlab>(std::move(*IF->Symbols)),
          std::make_unique<RefSlab>(std::move(*IF->Refs)),
          std::make_unique<RelationSlab>(std::move(*IF->Relations)),
          Path == MainFile);
    }
  }
}

llvm::Expected<BackgroundIndex::IndexResult> BackgroundIndex::index(
    tooling::CompileCommand Cmd,
    CompileCommandDriverFingerprintCache *DriverFingerprints) {
  trace::Span Tracer("BackgroundIndex");
  SPAN_ATTACH(Tracer, "file", Cmd.Filename);
  auto AbsolutePath = getAbsolutePath(Cmd);

  auto FS = TFS.view(Cmd.Directory);
  auto Buf = FS->getBufferForFile(AbsolutePath);
  if (!Buf)
    return llvm::errorCodeToError(Buf.getError());
  auto Hash = digest(Buf->get()->getBuffer());

  // Take a snapshot of the versions to avoid locking for each file in the TU.
  llvm::StringMap<ShardVersion> ShardVersionsSnapshot;
  {
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    ShardVersionsSnapshot = ShardVersions;
  }

  vlog("Indexing {0} (digest:={1})", Cmd.Filename, llvm::toHex(Hash));
  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(Cmd);
  GraphDiagnosticConsumer Diagnostics;
  auto CI = buildCompilerInvocation(Inputs, Diagnostics);
  if (!CI)
    return error("Couldn't build compiler invocation");

  auto Clang =
      prepareCompilerInstance(std::move(CI), /*Preamble=*/nullptr,
                              std::move(*Buf), std::move(FS), Diagnostics);
  if (!Clang)
    return error("Couldn't build compiler instance");

  SymbolCollector::Options IndexOpts;
  // Creates a filter to not collect index results from files with unchanged
  // digests.
  IndexOpts.FileFilter = [&ShardVersionsSnapshot](const SourceManager &SM,
                                                  FileID FID) {
    const auto F = SM.getFileEntryRefForID(FID);
    if (!F)
      return false; // Skip invalid files.
    auto AbsPath = getCanonicalPath(*F, SM.getFileManager());
    if (!AbsPath)
      return false; // Skip files without absolute path.
    auto Digest = digestFile(SM, FID);
    if (!Digest)
      return false;
    auto D = ShardVersionsSnapshot.find(*AbsPath);
    if (D != ShardVersionsSnapshot.end() && D->second.Digest == Digest &&
        !D->second.HadErrors)
      return false; // Skip files that haven't changed, without errors.
    return true;
  };
  IndexOpts.CollectMainFileRefs = true;
  IndexOpts.CollectGraph = true;

  IndexFileIn Index;
  auto Action = createStaticIndexingAction(
      IndexOpts, [&](SymbolSlab S) { Index.Symbols = std::move(S); },
      [&](RefSlab R) { Index.Refs = std::move(R); },
      [&](RelationSlab R) { Index.Relations = std::move(R); },
      [&](IncludeGraph IG) { Index.Sources = std::move(IG); },
      [&](GraphTU Graph) { Index.Graphs.push_back(std::move(Graph)); });

  // We're going to run clang here, and it could potentially crash.
  // We could use CrashRecoveryContext to try to make indexing crashes nonfatal,
  // but the leaky "recovery" is pretty scary too in a long-running process.
  // If crashes are a real problem, maybe we should fork a child process.

  const FrontendInputFile &Input = Clang->getFrontendOpts().Inputs.front();
  if (!Action->BeginSourceFile(*Clang, Input))
    return error("BeginSourceFile() failed");
  auto SemanticStarted = std::chrono::steady_clock::now();
  if (llvm::Error Err = Action->Execute())
    return Err;

  Action->EndSourceFile();

  Index.Cmd = Inputs.CompileCommand;
  assert(Index.Graphs.size() == 1 && "graph collector did not produce one TU");
  if (Index.Graphs.front().Language.empty())
    Index.Graphs.clear();
  if (!Index.Graphs.empty()) {
    Index.Graphs.front().ProducerFingerprint = graphProducerFingerprint();
    Index.Graphs.front().MainFile = AbsolutePath.str().str();
    Index.Graphs.front().Directory = Inputs.CompileCommand.Directory;
    Index.Graphs.front().CommandLine = Inputs.CompileCommand.CommandLine;
    Index.Graphs.front().Output = Inputs.CompileCommand.Output;
    Index.Graphs.front().CommandDigest =
        graphCommandDigest(Inputs.CompileCommand);
    Index.Graphs.front().ToolchainFingerprint =
        compileCommandToolchainFingerprint(Inputs.CompileCommand,
                                           DriverFingerprints);
  }
  assert(Index.Symbols && Index.Refs && Index.Sources &&
         "Symbols, Refs and Sources must be set.");

  log("Indexed {0} ({1} symbols, {2} refs, {3} files)",
      Inputs.CompileCommand.Filename, Index.Symbols->size(),
      Index.Refs->numRefs(), Index.Sources->size());
  SPAN_ATTACH(Tracer, "symbols", int(Index.Symbols->size()));
  SPAN_ATTACH(Tracer, "refs", int(Index.Refs->numRefs()));
  SPAN_ATTACH(Tracer, "sources", int(Index.Sources->size()));

  bool HadErrors = Clang->hasDiagnostics() &&
                   Clang->getDiagnostics().hasUncompilableErrorOccurred();
  if (HadErrors) {
    log("Failed to compile {0}, index may be incomplete", AbsolutePath);
    for (auto &It : *Index.Sources)
      It.second.Flags |= IncludeGraphNode::SourceFlag::HadErrors;
  }
  if (!Index.Graphs.empty()) {
    Index.Graphs.front().HadErrors = HadErrors;
    Index.Graphs.front().Diagnostics = Diagnostics.take();
    for (auto &Diagnostic : Index.Graphs.front().Diagnostics)
      if (!Diagnostic.Range.valid())
        Diagnostic.Range.FileURI = Index.Graphs.front().MainFileURI;
  }
  IndexResult Result;
  if (!Index.Graphs.empty()) {
    std::vector<std::string> IncludeRoots;
    for (const auto &Entry : Clang->getHeaderSearchOpts().UserEntries)
      IncludeRoots.push_back(Entry.Path);
    std::vector<std::string> ForcedIncludes =
        Clang->getPreprocessorOpts().Includes;
    llvm::append_range(ForcedIncludes,
                       Clang->getPreprocessorOpts().MacroIncludes);
    if (!Clang->getPreprocessorOpts().ImplicitPCHInclude.empty())
      ForcedIncludes.push_back(Clang->getPreprocessorOpts().ImplicitPCHInclude);
    Result.MissingInputs = graphMissingIncludeCandidates(
        Index.Graphs.front(), IncludeRoots, ForcedIncludes);
  }
  Result.Index = std::move(Index);
  Result.ShardVersionsSnapshot = std::move(ShardVersionsSnapshot);
  Result.HadErrors = HadErrors;
  Result.SemanticMillis = elapsedMillis(SemanticStarted);
  return Result;
}

// Restores shards for \p MainFiles from index storage. Then checks staleness of
// those shards and returns a list of TUs that needs to be indexed to update
// staleness.
std::vector<std::string>
BackgroundIndex::loadProject(std::vector<std::string> MainFiles) {
  // Drop files where background indexing is disabled in config.
  if (ContextProvider)
    llvm::erase_if(MainFiles, [&](const std::string &TU) {
      // Load the config for each TU, as indexing may be selectively enabled.
      WithContext WithProvidedContext(ContextProvider(TU));
      return Config::current().Index.Background ==
             Config::BackgroundPolicy::Skip;
    });
  Rebuilder.startLoading();
  // Load shards for all of the mainfiles.
  const std::vector<LoadedShard> Result =
      loadIndexShards(MainFiles, IndexStorageFactory, CDB);
  size_t LoadedShards = 0;
  std::vector<GraphTU> LoadedGraphs;
  {
    // Update in-memory state.
    std::lock_guard<std::mutex> Lock(ShardVersionsMu);
    for (auto &LS : Result) {
      if (!LS.Shard)
        continue;
      for (const auto &Graph : LS.Shard->Graphs)
        if (Graph.ProducerFingerprint == graphProducerFingerprint() &&
            (Graph.Language == "c" || Graph.Language == "cpp"))
          LoadedGraphs.push_back(Graph);
      auto SS =
          LS.Shard->Symbols
              ? std::make_unique<SymbolSlab>(std::move(*LS.Shard->Symbols))
              : nullptr;
      auto RS = LS.Shard->Refs
                    ? std::make_unique<RefSlab>(std::move(*LS.Shard->Refs))
                    : nullptr;
      auto RelS =
          LS.Shard->Relations
              ? std::make_unique<RelationSlab>(std::move(*LS.Shard->Relations))
              : nullptr;
      ShardVersion &SV = ShardVersions[LS.AbsolutePath];
      SV.Digest = LS.Digest;
      SV.HadErrors = LS.HadErrors;
      ++LoadedShards;

      IndexedSymbols.update(URI::create(LS.AbsolutePath).toString(),
                            std::move(SS), std::move(RS), std::move(RelS),
                            LS.CountReferences);
    }
  }
  if (!LoadedGraphs.empty()) {
    std::lock_guard<std::mutex> Lock(GraphMu);
    for (auto &Graph : LoadedGraphs) {
      if (Graph.MainFile.empty() || Graph.CommandDigest.empty())
        continue;
      const std::string MainKey = graphMainKey(Graph.MainFile);
      const std::string CommandDigest = Graph.CommandDigest;
      Graphs[MainKey][CommandDigest] = std::move(Graph);
      GraphSemanticMillis[MainKey][CommandDigest] = 0;
    }
    ++GraphRevision;
  }
  Rebuilder.loadedShard(LoadedShards);
  Rebuilder.doneLoading();

  auto FS = TFS.view(/*CWD=*/std::nullopt);
  llvm::DenseSet<PathRef> TUsToIndex;
  // We'll accept data from stale shards, but ensure the files get reindexed
  // soon.
  for (auto &LS : Result) {
    if (!shardIsStale(LS, FS.get()))
      continue;
    PathRef TUForFile = LS.DependentTU;
    assert(!TUForFile.empty() && "File without a TU!");

    // FIXME: Currently, we simply schedule indexing on a TU whenever any of
    // its dependencies needs re-indexing. We might do it smarter by figuring
    // out a minimal set of TUs that will cover all the stale dependencies.
    // FIXME: Try looking at other TUs if no compile commands are available
    // for this TU, i.e TU was deleted after we performed indexing.
    TUsToIndex.insert(TUForFile);
  }

  // Content digests alone cannot detect a compile-command/configuration move.
  // Require the persisted set of TU views to match every current command.
  for (const auto &MainFile : MainFiles) {
    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(MainFile));
    llvm::StringSet<> Expected;
    for (const auto &Command : CDB.getCompileCommands(MainFile))
      if (graphCommandIsCOrCXX(Command))
        Expected.insert(graphCommandDigest(Command));
    llvm::StringSet<> Actual;
    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      auto It = Graphs.find(graphMainKey(MainFile));
      if (It != Graphs.end())
        for (const auto &Configuration : It->second)
          Actual.insert(Configuration.first);
    }
    if (Expected.size() != Actual.size() ||
        llvm::any_of(Expected, [&](const auto &Entry) {
          return !Actual.contains(Entry.getKey());
        }))
      TUsToIndex.insert(MainFile);
  }

  return {TUsToIndex.begin(), TUsToIndex.end()};
}

void BackgroundIndex::enqueueGraphDependents(
    llvm::ArrayRef<std::string> ChangedFiles) {
  llvm::StringSet<> Changed;
  bool ConfigurationChanged = false;
  for (const auto &File : ChangedFiles)
    Changed.insert(graphMainKey(File));
  for (const auto &File : ChangedFiles) {
    llvm::StringRef Name = llvm::sys::path::filename(File);
    ConfigurationChanged |= Name.equals_insensitive("compile_commands.json") ||
                            Name.equals_insensitive("compile_flags.txt") ||
                            Name == ".clangd";
  }

  std::map<std::string, std::string> TUs;
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    for (const auto &Main : Graphs) {
      bool Affected =
          ConfigurationChanged || Changed.contains(graphMainKey(Main.getKey()));
      for (const auto &Configuration : Main.getValue()) {
        if (Affected)
          break;
        for (const auto &Source : Configuration.second.Sources) {
          auto Path = URI::resolve(Source.URI, Configuration.second.MainFile);
          if (Path && Changed.contains(graphMainKey(*Path))) {
            Affected = true;
            break;
          }
          if (!Path)
            llvm::consumeError(Path.takeError());
        }
      }
      if (Affected && !Main.getValue().empty())
        TUs.try_emplace(Main.getKey().str(),
                        Main.getValue().begin()->second.MainFile);
    }
    // A failed graph has no reliable dependency set. Any later watched
    // workspace change may be the generated input that makes it publishable.
    if (!ChangedFiles.empty())
      for (const auto &Failure : GraphFailureTUs)
        TUs.try_emplace(Failure.getKey().str(), Failure.getValue());
  }
  for (const auto &File : ChangedFiles)
    if (isGraphTranslationUnit(File) && !CDB.getCompileCommands(File).empty())
      TUs.try_emplace(graphMainKey(File), File);
  std::vector<std::string> Work;
  Work.reserve(TUs.size());
  for (const auto &TU : TUs)
    Work.push_back(TU.second);
  if (!Work.empty())
    enqueue(Work);
}

llvm::Expected<llvm::json::Value>
BackgroundIndex::graphSnapshot(const GraphSnapshotParams &Params) {
  const auto RequestStarted = std::chrono::steady_clock::now();
  // Query-driver output is part of the graph's semantic universe. Refresh it
  // once per distinct query in this request so arbitrary wrappers and specs
  // files cannot remain hidden behind the ordinary process cache.
  SystemIncludeExtractorScope RefreshSystemIncludes(/*RefreshQueries=*/true);
  auto CheckCancellation = []() -> llvm::Error {
    if (auto Reason = isCancelled())
      return llvm::make_error<CancelledError>(Reason);
    return llvm::Error::success();
  };
  if (llvm::Error Err = CheckCancellation())
    return std::move(Err);
  std::vector<std::pair<std::string, std::vector<std::string>>> FailedInputs;
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    if (GraphDiscoveryPending == 0 && GraphPending.empty())
      for (const auto &Failure : GraphFailureInputs) {
        auto TU = GraphFailureTUs.find(Failure.getKey());
        if (TU != GraphFailureTUs.end())
          FailedInputs.emplace_back(TU->getValue(), Failure.getValue());
      }
  }
  std::vector<std::string> Retry;
  auto FailureFS = TFS.view(/*CWD=*/std::nullopt);
  for (const auto &Failure : FailedInputs)
    if (llvm::any_of(Failure.second, [&](const std::string &Candidate) {
          return FailureFS->exists(Candidate);
        }))
      Retry.push_back(Failure.first);
  if (!Retry.empty()) {
    enqueue(Retry);
    return contentModified(
        "graph snapshot is retrying translation units whose generated inputs "
        "appeared");
  }

  constexpr uint32_t DefaultPageSize = 32;
  constexpr uint32_t MaximumPageSize = 128;
  const uint32_t PageSize = Params.MaxShards.value_or(DefaultPageSize);
  if (PageSize == 0 || PageSize > MaximumPageSize)
    return error("graph snapshot maxShards must be between 1 and {0}",
                 MaximumPageSize);

  const std::vector<std::pair<const char *, const char *>> Coverage = {
      {"contains", "complete"},   {"exports", "partial"},
      {"imports", "complete"},    {"calls", "partial"},
      {"accesses", "complete"},   {"instantiates", "partial"},
      {"type_ref", "complete"},   {"extends", "complete"},
      {"implements", "partial"},  {"overrides", "complete"},
      {"dispatches", "partial"},  {"decorates", "unsupported"},
      {"renders", "unsupported"}, {"tests", "unsupported"},
      {"references", "complete"}};

  auto JSONText = [](const llvm::json::Value &Value) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    OS << Value;
    return OS.str();
  };
  auto CopyPageLocked =
      [&](const GraphSnapshotCache &Cache, const GraphSnapshotPlan &Plan,
          size_t Offset) -> llvm::Expected<std::vector<GraphTU>> {
    if (Offset > Plan.Upserts.size())
      return error("graph snapshot cursor offset is out of range");
    const size_t End = std::min(Plan.Upserts.size(), Offset + PageSize);
    std::vector<GraphTU> Page;
    Page.reserve(End - Offset);
    for (size_t I = Offset; I < End; ++I) {
      const auto &Shard = Cache.Shards[Plan.Upserts[I]];
      auto Main = Graphs.find(Shard.MainKey);
      if (Main == Graphs.end())
        return contentModified("graph snapshot changed between pages");
      auto Configuration = Main->getValue().find(Shard.CommandDigest);
      if (Configuration == Main->getValue().end())
        return contentModified("graph snapshot changed between pages");
      Page.push_back(Configuration->second);
    }
    return Page;
  };
  auto EncodePage =
      [&](const GraphSnapshotCache &Cache, const GraphSnapshotPlan &Plan,
          size_t Offset, std::vector<GraphTU> PageGraphs,
          uint64_t ValidationMillis) -> llvm::Expected<llvm::json::Value> {
    auto EncodeStarted = std::chrono::steady_clock::now();
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    const size_t End = std::min(Plan.Upserts.size(), Offset + PageSize);
    if (PageGraphs.size() != End - Offset)
      return error("graph snapshot page does not match its plan");
    llvm::json::Array Upserts;
    for (size_t I = Offset; I < End; ++I) {
      if (llvm::Error Err = CheckCancellation())
        return std::move(Err);
      const auto &Shard = Cache.Shards[Plan.Upserts[I]];
      const auto &Graph = PageGraphs[I - Offset];
      llvm::json::Value GraphJSON = toJSON(Graph);
      std::string ShardDigest =
          graphDigest(Shard.Key + "\n" + Shard.CheckerDigest + "\n" +
                      Shard.InterfaceFingerprint + "\n" + JSONText(GraphJSON));
      if (ShardDigest != Shard.Digest)
        return contentModified("graph snapshot shard changed while paging: {0}",
                               Shard.Key);
      llvm::json::Array CoverageJSON;
      for (const auto &Row : Coverage)
        CoverageJSON.push_back(
            llvm::json::Object{{"family", Row.first}, {"state", Row.second}});
      Upserts.push_back(llvm::json::Object{
          {"key", Shard.Key},
          {"source", Graph.MainFile},
          {"configuration", Graph.CommandDigest},
          {"checkerDigest", Shard.CheckerDigest},
          {"interfaceFingerprint", Shard.InterfaceFingerprint},
          {"digest", Shard.Digest},
          {"graph", std::move(GraphJSON)},
          {"coverage", std::move(CoverageJSON)}});
    }

    llvm::json::Array Manifest;
    if (Offset == 0 && !Plan.CacheHit)
      for (const auto &Shard : Cache.Shards)
        Manifest.push_back(
            llvm::json::Object{{"key", Shard.Key}, {"digest", Shard.Digest}});
    llvm::json::Array Deletes;
    if (Offset == 0)
      for (const auto &Key : Plan.Deletes)
        Deletes.push_back(Key);
    auto Strings = [](const std::vector<std::string> &Values) {
      llvm::json::Array Result;
      for (const auto &Value : Values)
        Result.push_back(Value);
      return Result;
    };
    llvm::json::Value NextCursor = llvm::json::Value(nullptr);
    if (End < Plan.Upserts.size())
      NextCursor = Plan.Token + ":" + std::to_string(End);
    const uint64_t SemanticMillis = Offset == 0 ? Plan.SemanticMillis : 0;
    const uint64_t ShardMillis = Offset == 0 ? Plan.ShardMillis : 0;
    const uint64_t EncodeMillis = elapsedMillis(EncodeStarted);
    const uint64_t TotalMillis =
        ValidationMillis + SemanticMillis + ShardMillis + EncodeMillis;
    return llvm::json::Object{
        {"protocolVersion", 1},
        {"schemaVersion", 1},
        {"producer",
         llvm::json::Object{{"name", "samchon-clangd"},
                            {"version", getClangFullVersion()},
                            {"commit", getClangFullRepositoryVersion()}}},
        {"universe",
         llvm::json::Object{{"digest", Cache.UniverseDigest},
                            {"targets", Strings(Cache.Targets)},
                            {"workspaceRoots", Strings(Cache.WorkspaceRoots)},
                            {"toolchains", Strings(Cache.Toolchains)},
                            {"configurations", Strings(Cache.Configurations)}}},
        {"sequence", static_cast<int64_t>(Plan.Sequence)},
        {"generation", Cache.Generation},
        {"baseGeneration", Plan.BaseGeneration
                               ? llvm::json::Value(*Plan.BaseGeneration)
                               : llvm::json::Value(nullptr)},
        {"upserts", std::move(Upserts)},
        {"deletes", std::move(Deletes)},
        {"manifest", std::move(Manifest)},
        {"page",
         llvm::json::Object{
             {"offset", static_cast<int64_t>(Offset)},
             {"count", static_cast<int64_t>(End - Offset)},
             {"total", static_cast<int64_t>(Plan.Upserts.size())},
             {"nextCursor", std::move(NextCursor)}}},
        {"phases",
         llvm::json::Object{
             {"validationMillis", static_cast<int64_t>(ValidationMillis)},
             {"semanticMillis", static_cast<int64_t>(SemanticMillis)},
             {"shardMillis", static_cast<int64_t>(ShardMillis)},
             {"encodeMillis", static_cast<int64_t>(EncodeMillis)},
             {"totalMillis", static_cast<int64_t>(TotalMillis)},
             {"cacheHit", Plan.CacheHit}}}};
  };
  auto CreatePlanLocked = [&](const GraphSnapshotCache &Cache,
                              bool CacheWasBuilt) {
    GraphSnapshotPlan Plan;
    Plan.Revision = Cache.Revision;
    Plan.Generation = Cache.Generation;
    Plan.Sequence = ++GraphSequence;
    llvm::StringMap<std::string> CurrentManifest;
    for (const auto &Shard : Cache.Shards)
      CurrentManifest[Shard.Key] = Shard.Digest;
    if (Params.KnownGeneration && *Params.KnownGeneration == Cache.Generation) {
      Plan.BaseGeneration = Cache.Generation;
      Plan.CacheHit = true;
    } else if (Params.KnownGeneration &&
               *Params.KnownGeneration == PublishedGeneration) {
      Plan.BaseGeneration = PublishedGeneration;
      for (size_t I = 0; I < Cache.Shards.size(); ++I) {
        const auto &Shard = Cache.Shards[I];
        auto Old = PublishedManifest.find(Shard.Key);
        if (Old == PublishedManifest.end() || Old->getValue() != Shard.Digest)
          Plan.Upserts.push_back(I);
      }
      for (const auto &Old : PublishedManifest)
        if (!CurrentManifest.contains(Old.getKey()))
          Plan.Deletes.push_back(Old.getKey().str());
      llvm::sort(Plan.Deletes);
    } else {
      Plan.Upserts.resize(Cache.Shards.size());
      std::iota(Plan.Upserts.begin(), Plan.Upserts.end(), 0);
    }
    for (size_t I : Plan.Upserts)
      Plan.SemanticMillis += Cache.Shards[I].SemanticMillis;
    Plan.ShardMillis = CacheWasBuilt ? Cache.ShardMillis : 0;
    Plan.Token =
        graphDigest(Cache.Generation + ":" + std::to_string(Plan.Sequence) +
                    ":" + Params.KnownGeneration.value_or("full"));
    return Plan;
  };
  auto PublishPlanLocked = [&](const GraphSnapshotCache &Cache,
                               const GraphSnapshotPlan &Plan) {
    llvm::StringMap<std::string> CurrentManifest;
    for (const auto &Shard : Cache.Shards)
      CurrentManifest[Shard.Key] = Shard.Digest;
    PublishedGeneration = Cache.Generation;
    PublishedManifest = std::move(CurrentManifest);
    for (auto I = ActiveGraphSnapshotPlans.begin();
         I != ActiveGraphSnapshotPlans.end();) {
      auto Current = I++;
      if (Current->getValue().Revision != Cache.Revision ||
          Current->getValue().Generation != Cache.Generation)
        ActiveGraphSnapshotPlans.erase(Current);
    }
    constexpr size_t MaximumActivePlans = 64;
    while (ActiveGraphSnapshotPlans.size() >= MaximumActivePlans) {
      auto Oldest = llvm::min_element(
          ActiveGraphSnapshotPlans, [](const auto &Left, const auto &Right) {
            return Left.getValue().Sequence < Right.getValue().Sequence;
          });
      ActiveGraphSnapshotPlans.erase(Oldest);
    }
    ActiveGraphSnapshotPlans[Plan.Token] = Plan;
  };
  auto ValidateCachedSnapshot =
      [&](const GraphSnapshotCache &Cache) -> llvm::Error {
    std::map<std::string, llvm::StringMap<std::string>> ActualConfigurations;
    for (const auto &Shard : Cache.Shards)
      ActualConfigurations[Shard.MainFile][Shard.CommandDigest] =
          Shard.ToolchainFingerprint;
    std::vector<std::string> Mismatched;
    for (const auto &Main : ActualConfigurations) {
      if (llvm::Error Err = CheckCancellation())
        return Err;
      std::optional<WithContext> WithProvidedContext;
      if (ContextProvider)
        WithProvidedContext.emplace(ContextProvider(Main.first));
      llvm::StringMap<std::string> Expected;
      for (const auto &Command : CDB.getCompileCommands(Main.first))
        if (graphCommandIsCOrCXX(Command))
          Expected[graphCommandDigest(Command)] =
              compileCommandToolchainFingerprint(
                  Command, &RefreshSystemIncludes.driverFingerprints());
      if (llvm::Error Err = CheckCancellation())
        return Err;
      if (Expected.size() != Main.second.size() ||
          llvm::any_of(Expected, [&](const auto &Entry) {
            auto Actual = Main.second.find(Entry.getKey());
            return Actual == Main.second.end() ||
                   Actual->getValue() != Entry.getValue();
          })) {
        Mismatched.push_back(Main.first);
      }
    }
    if (!Mismatched.empty()) {
      if (llvm::Error Err = CheckCancellation())
        return Err;
      enqueueGraphReindex(Mismatched);
      return contentModified(
          "graph snapshot is reindexing {0} translation units with moved "
          "compile commands/toolchains",
          Mismatched.size());
    }
    llvm::StringMap<std::string> CheckedSources;
    for (const auto &Shard : Cache.Shards) {
      if (llvm::Error Err = CheckCancellation())
        return Err;
      auto FS = TFS.view(Shard.Directory);
      for (const auto &Source : Shard.Sources) {
        auto Resolved = URI::resolve(Source.URI, Shard.MainFile);
        if (!Resolved)
          return Resolved.takeError();
        auto Known = CheckedSources.find(*Resolved);
        if (Known != CheckedSources.end()) {
          if (Known->getValue() != Source.Digest)
            return error("graph snapshot has contradictory TU views for {0}",
                         *Resolved);
          continue;
        }
        auto Buffer = FS->getBufferForFile(*Resolved);
        if (!Buffer)
          return contentModified(
              "graph snapshot source cannot be read: {0}: {1}", *Resolved,
              Buffer.getError().message());
        std::string Current = graphDigest(Buffer.get()->getBuffer());
        if (Current != Source.Digest)
          return contentModified(
              "graph snapshot source moved during indexing: {0}", *Resolved);
        CheckedSources[*Resolved] = std::move(Current);
      }
    }
    return llvm::Error::success();
  };

  size_t Offset = 0;
  if (Params.Cursor) {
    llvm::StringRef Cursor = *Params.Cursor;
    auto Split = Cursor.rsplit(':');
    uint64_t ParsedOffset = 0;
    if (Split.first.empty() || Split.second.empty() ||
        Split.second.getAsInteger(10, ParsedOffset))
      return error("graph snapshot cursor is malformed");
    Offset = static_cast<size_t>(ParsedOffset);
    GraphSnapshotCache Cache;
    GraphSnapshotPlan Plan;
    std::vector<GraphTU> PageGraphs;
    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      auto Active = ActiveGraphSnapshotPlans.find(Split.first);
      if (!CachedGraphSnapshot || Active == ActiveGraphSnapshotPlans.end() ||
          CachedGraphSnapshot->Revision != GraphRevision ||
          Active->getValue().Revision != CachedGraphSnapshot->Revision ||
          Active->getValue().Generation != CachedGraphSnapshot->Generation)
        return contentModified("graph snapshot cursor is stale");
      if (GraphDiscoveryPending != 0 || !GraphPending.empty() ||
          !GraphFailures.empty())
        return contentModified("graph snapshot state moved between pages");
      Cache = *CachedGraphSnapshot;
      Plan = Active->getValue();
      auto Page = CopyPageLocked(Cache, Plan, Offset);
      if (!Page)
        return Page.takeError();
      PageGraphs = std::move(*Page);
    }
    return EncodePage(Cache, Plan, Offset, std::move(PageGraphs),
                      elapsedMillis(RequestStarted));
  }

  {
    GraphSnapshotCache Cache;
    bool Reused = false;
    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      if (GraphDiscoveryPending != 0)
        return contentModified(
            "graph snapshot is not ready: project changes are still being "
            "discovered");
      if (!GraphPending.empty())
        return contentModified("graph snapshot is not ready: {0} translation "
                               "units are still indexing",
                               GraphPending.size());
      if (!GraphFailures.empty()) {
        const auto &Failure = *GraphFailures.begin();
        return error("graph snapshot is not publishable: {0}: {1}",
                     Failure.getKey(), Failure.getValue());
      }
      if (Graphs.empty() && PublishedGeneration.empty())
        return contentModified(
            "graph snapshot is not ready: initial indexing has not completed");
      if (CachedGraphSnapshot &&
          CachedGraphSnapshot->Revision == GraphRevision) {
        Cache = *CachedGraphSnapshot;
        Reused = true;
      }
    }
    if (Reused) {
      if (llvm::Error Err = ValidateCachedSnapshot(Cache))
        return std::move(Err);
      GraphSnapshotPlan Plan;
      std::vector<GraphTU> PageGraphs;
      {
        std::lock_guard<std::mutex> Lock(GraphMu);
        if (!CachedGraphSnapshot ||
            CachedGraphSnapshot->Revision != GraphRevision ||
            GraphDiscoveryPending != 0 || !GraphPending.empty() ||
            !GraphFailures.empty())
          return contentModified(
              "graph snapshot state moved while its cache was validated");
        Plan = CreatePlanLocked(Cache, false);
        auto Page = CopyPageLocked(Cache, Plan, 0);
        if (!Page)
          return Page.takeError();
        PageGraphs = std::move(*Page);
      }
      auto Encoded = EncodePage(Cache, Plan, 0, std::move(PageGraphs),
                                elapsedMillis(RequestStarted));
      if (!Encoded)
        return Encoded.takeError();
      if (llvm::Error Err = CheckCancellation())
        return std::move(Err);
      {
        std::lock_guard<std::mutex> Lock(GraphMu);
        if (!CachedGraphSnapshot ||
            CachedGraphSnapshot->Revision != Cache.Revision ||
            CachedGraphSnapshot->Generation != Cache.Generation ||
            GraphDiscoveryPending != 0 || !GraphPending.empty() ||
            !GraphFailures.empty())
          return contentModified(
              "graph snapshot state moved before publication");
        PublishPlanLocked(Cache, Plan);
      }
      return Encoded;
    }
  }

  struct FrozenGraph {
    GraphTU Graph;
    std::string MainKey;
    uint64_t SemanticMillis = 0;
  };
  std::vector<FrozenGraph> Frozen;
  uint64_t Revision = 0;
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    Revision = GraphRevision;
    for (const auto &Main : Graphs)
      for (const auto &Configuration : Main.getValue()) {
        uint64_t SemanticMillis = 0;
        auto Timings = GraphSemanticMillis.find(Main.getKey());
        if (Timings != GraphSemanticMillis.end()) {
          auto Timing = Timings->getValue().find(Configuration.first);
          if (Timing != Timings->getValue().end())
            SemanticMillis = Timing->second;
        }
        Frozen.push_back(FrozenGraph{Configuration.second, Main.getKey().str(),
                                     SemanticMillis});
      }
  }
  if (llvm::Error Err = CheckCancellation())
    return std::move(Err);
  llvm::sort(Frozen, [](const FrozenGraph &Left, const FrozenGraph &Right) {
    return std::tie(Left.Graph.MainFile, Left.Graph.CommandDigest) <
           std::tie(Right.Graph.MainFile, Right.Graph.CommandDigest);
  });

  std::map<std::string, llvm::StringMap<std::string>> ActualConfigurations;
  for (const auto &Entry : Frozen) {
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    const auto &Graph = Entry.Graph;
    if (Graph.HadErrors)
      return error("graph snapshot contains an erroneous configuration: {0}",
                   Graph.MainFile);
    if (Graph.ProducerFingerprint != graphProducerFingerprint())
      return contentModified("graph snapshot compiler identity moved: {0}",
                             Graph.MainFile);
    ActualConfigurations[Graph.MainFile][Graph.CommandDigest] =
        Graph.ToolchainFingerprint;
  }
  std::vector<std::string> Mismatched;
  for (const auto &Main : ActualConfigurations) {
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(Main.first));
    llvm::StringMap<std::string> Expected;
    for (const auto &Command : CDB.getCompileCommands(Main.first))
      if (graphCommandIsCOrCXX(Command))
        Expected[graphCommandDigest(Command)] =
            compileCommandToolchainFingerprint(
                Command, &RefreshSystemIncludes.driverFingerprints());
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    if (Expected.size() != Main.second.size() ||
        llvm::any_of(Expected, [&](const auto &Entry) {
          auto Actual = Main.second.find(Entry.getKey());
          return Actual == Main.second.end() ||
                 Actual->getValue() != Entry.getValue();
        })) {
      Mismatched.push_back(Main.first);
    }
  }
  if (!Mismatched.empty()) {
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    enqueueGraphReindex(Mismatched);
    return contentModified(
        "graph snapshot is reindexing {0} translation units with moved "
        "compile commands/toolchains",
        Mismatched.size());
  }

  llvm::StringMap<std::string> CheckedSources;
  for (const auto &Entry : Frozen) {
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    const auto &Graph = Entry.Graph;
    auto FS = TFS.view(Graph.Directory);
    for (const auto &Source : Graph.Sources) {
      auto Resolved = URI::resolve(Source.URI, Graph.MainFile);
      if (!Resolved)
        return Resolved.takeError();
      auto Known = CheckedSources.find(*Resolved);
      if (Known != CheckedSources.end()) {
        if (Known->getValue() != Source.Digest)
          return error("graph snapshot has contradictory TU views for {0}",
                       *Resolved);
        continue;
      }
      auto Buffer = FS->getBufferForFile(*Resolved);
      if (!Buffer)
        return contentModified("graph snapshot source cannot be read: {0}: {1}",
                               *Resolved, Buffer.getError().message());
      std::string Current = graphDigest(Buffer.get()->getBuffer());
      if (Current != Source.Digest)
        return contentModified(
            "graph snapshot source moved during indexing: {0}", *Resolved);
      CheckedSources[*Resolved] = std::move(Current);
    }
  }

  const uint64_t ValidationMillis = elapsedMillis(RequestStarted);
  auto ShardStarted = std::chrono::steady_clock::now();
  GraphSnapshotCache Built;
  Built.Revision = Revision;
  std::set<std::string> Targets;
  std::set<std::string> Configurations;
  std::set<std::string> WorkspaceRoots;
  std::set<std::string> Toolchains;
  for (const auto &Entry : Frozen) {
    if (llvm::Error Err = CheckCancellation())
      return std::move(Err);
    const auto &Graph = Entry.Graph;
    std::string Key = Graph.MainFileURI + "#" + Graph.CommandDigest;
    std::string CheckerDigest;
    for (const auto &Source : Graph.Sources)
      if (Source.URI == Graph.MainFileURI) {
        CheckerDigest = Source.Digest;
        break;
      }
    if (CheckerDigest.empty())
      return error("graph snapshot has no main-file checker: {0}",
                   Graph.MainFile);
    std::string Interface;
    llvm::raw_string_ostream InterfaceOS(Interface);
    for (const auto &Symbol : Graph.Symbols)
      if (Symbol.Exported)
        InterfaceOS << Symbol.ID.size() << ':' << Symbol.ID
                    << Symbol.Signature.size() << ':' << Symbol.Signature;
    std::string InterfaceFingerprint = graphDigest(InterfaceOS.str());
    llvm::json::Value GraphJSON = toJSON(Graph);
    std::string ShardDigest =
        graphDigest(Key + "\n" + CheckerDigest + "\n" + InterfaceFingerprint +
                    "\n" + JSONText(GraphJSON));
    GraphSnapshotShardCache Cached;
    Cached.MainKey = Entry.MainKey;
    Cached.MainFile = Graph.MainFile;
    Cached.Directory = Graph.Directory;
    Cached.CommandDigest = Graph.CommandDigest;
    Cached.ToolchainFingerprint = Graph.ToolchainFingerprint;
    Cached.Key = std::move(Key);
    Cached.Digest = std::move(ShardDigest);
    Cached.CheckerDigest = std::move(CheckerDigest);
    Cached.InterfaceFingerprint = std::move(InterfaceFingerprint);
    Cached.Sources = Graph.Sources;
    Cached.SemanticMillis = Entry.SemanticMillis;
    Built.Shards.push_back(std::move(Cached));
    Targets.insert(Graph.TargetTriple);
    Configurations.insert(Graph.CommandDigest);
    Toolchains.insert(Graph.ToolchainFingerprint);
    if (auto Project = CDB.getProjectInfo(Graph.MainFile))
      WorkspaceRoots.insert(graphMainKey(Project->SourceRoot));
  }
  Built.Targets.assign(Targets.begin(), Targets.end());
  Built.Configurations.assign(Configurations.begin(), Configurations.end());
  Built.WorkspaceRoots.assign(WorkspaceRoots.begin(), WorkspaceRoots.end());
  Built.Toolchains.assign(Toolchains.begin(), Toolchains.end());

  std::string UniverseMaterial;
  llvm::raw_string_ostream UniverseOS(UniverseMaterial);
  auto Coordinate = [&](llvm::StringRef Label, llvm::StringRef Value) {
    UniverseOS << Label << ':' << Value.size() << ':' << Value;
  };
  Coordinate("producer", graphProducerFingerprint());
  for (const auto &Target : Built.Targets)
    Coordinate("target", Target);
  for (const auto &Root : Built.WorkspaceRoots)
    Coordinate("root", Root);
  for (const auto &Toolchain : Built.Toolchains)
    Coordinate("toolchain", Toolchain);
  for (const auto &Configuration : Built.Configurations)
    Coordinate("configuration", Configuration);
  Built.UniverseDigest = graphDigest(UniverseOS.str());
  std::string GenerationMaterial;
  llvm::raw_string_ostream GenerationOS(GenerationMaterial);
  for (const auto &Shard : Built.Shards)
    GenerationOS << Shard.Key.size() << ':' << Shard.Key << Shard.Digest;
  Built.Generation = graphDigest(Built.UniverseDigest + GenerationOS.str());
  Built.ShardMillis = elapsedMillis(ShardStarted);

  GraphSnapshotCache Cache;
  GraphSnapshotPlan Plan;
  std::vector<GraphTU> PageGraphs;
  if (llvm::Error Err = CheckCancellation())
    return std::move(Err);
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    if (Revision != GraphRevision || GraphDiscoveryPending != 0 ||
        !GraphPending.empty() || !GraphFailures.empty())
      return contentModified(
          "graph snapshot state moved while it was being frozen");
    CachedGraphSnapshot = std::move(Built);
    Cache = *CachedGraphSnapshot;
    Plan = CreatePlanLocked(Cache, true);
    auto Page = CopyPageLocked(Cache, Plan, 0);
    if (!Page)
      return Page.takeError();
    PageGraphs = std::move(*Page);
  }
  auto Encoded =
      EncodePage(Cache, Plan, 0, std::move(PageGraphs), ValidationMillis);
  if (!Encoded)
    return Encoded.takeError();
  if (llvm::Error Err = CheckCancellation())
    return std::move(Err);
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    if (!CachedGraphSnapshot ||
        CachedGraphSnapshot->Revision != Cache.Revision ||
        CachedGraphSnapshot->Generation != Cache.Generation ||
        GraphDiscoveryPending != 0 || !GraphPending.empty() ||
        !GraphFailures.empty())
      return contentModified("graph snapshot state moved before publication");
    PublishPlanLocked(Cache, Plan);
  }
  return Encoded;
}

void BackgroundIndex::profile(MemoryTree &MT) const {
  IndexedSymbols.profile(MT.child("slabs"));
  // We don't want to mix memory used by index and symbols, so call base class.
  MT.child("index").addUsage(SwapIndex::estimateMemoryUsage());
}
} // namespace clangd
} // namespace clang
