//===-- Background.cpp - Build an index in a background thread ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "index/Background.h"
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
#include "clang/Lex/Lexer.h"
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
        Result.Range.StartLine = SM.getSpellingLineNumber(Location) - 1;
        Result.Range.StartColumn = SM.getSpellingColumnNumber(Location) - 1;
        Result.Range.EndLine = Result.Range.StartLine;
        unsigned Length =
            Lang ? Lexer::MeasureTokenLength(Location, SM, *Lang) : 1;
        Result.Range.EndColumn =
            Result.Range.StartColumn + std::max(Length, 1u);
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
      Tasks.push_back(indexFileTask(std::move(File)));
    Queue.append(std::move(Tasks));
  });

  T.QueuePri = LoadShards;
  T.ThreadPri = llvm::ThreadPriority::Default;
  return T;
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
           !types::onlyPrecompileType(Type);
  }
  return false;
}

BackgroundQueue::Task BackgroundIndex::indexFileTask(std::string Path) {
  std::string Tag = filenameWithoutExtension(Path).str();
  uint64_t Key = llvm::xxh3_64bits(graphMainKey(Path));
  BackgroundQueue::Task T([this, Path(std::move(Path))] {
    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(Path));
    auto Commands = CDB.getCompileCommands(Path);
    std::optional<std::string> LegacyCommandDigest;
    if (!Commands.empty())
      LegacyCommandDigest = graphCommandDigest(Commands.front());
    std::map<std::string, tooling::CompileCommand> Configurations;
    for (auto &Command : Commands)
      Configurations.try_emplace(graphCommandDigest(Command),
                                 std::move(Command));
    const std::string GraphKey = graphMainKey(Path);

    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      GraphPending.insert(GraphKey);
      GraphFailures.erase(GraphKey);
    }

    auto Fail = [&](llvm::StringRef Message) {
      std::lock_guard<std::mutex> Lock(GraphMu);
      GraphPending.erase(GraphKey);
      GraphFailures[GraphKey] = Message.str();
      ++GraphRevision;
    };

    if (Configurations.empty()) {
      std::lock_guard<std::mutex> Lock(GraphMu);
      GraphPending.erase(GraphKey);
      GraphFailures.erase(GraphKey);
      if (Graphs.erase(GraphKey))
        ++GraphRevision;
      return;
    }

    std::optional<std::string> Failure;
    std::vector<IndexResult> Results;
    Results.reserve(Configurations.size());
    for (auto &Configuration : Configurations) {
      auto Result = index(std::move(Configuration.second));
      if (!Result) {
        std::string Message = llvm::toString(Result.takeError());
        elog("Indexing {0} failed: {1}", Path, Message);
        if (!Failure)
          Failure = std::move(Message);
        continue;
      }
      if (Result->HadErrors) {
        std::string Message = "compiler diagnostics contain errors";
        elog("Indexing {0} did not produce a publishable graph: {1}", Path,
             Message);
        if (!Failure)
          Failure = std::move(Message);
      }
      Results.push_back(std::move(*Result));
    }

    std::vector<GraphTU> CompleteGraphs;
    if (!Failure) {
      CompleteGraphs.reserve(Results.size());
      for (const auto &Result : Results) {
        assert(Result.Index.Graphs.size() == 1);
        CompleteGraphs.push_back(Result.Index.Graphs.front());
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
      return LegacyCommandDigest && Result.Index.Graphs.size() == 1 &&
             Result.Index.Graphs.front().CommandDigest == *LegacyCommandDigest;
    });
    if (Legacy != Results.end()) {
      Legacy->Index.Graphs = CompleteGraphs;
      update(Path, std::move(Legacy->Index), Legacy->ShardVersionsSnapshot,
             Legacy->HadErrors);
      Rebuilder.indexedTU();
    }

    if (Failure) {
      Fail(*Failure);
      return;
    }

    {
      std::lock_guard<std::mutex> Lock(GraphMu);
      auto &Published = Graphs[GraphKey];
      Published.clear();
      for (auto &Graph : CompleteGraphs)
        Published.emplace(Graph.CommandDigest, std::move(Graph));
      GraphPending.erase(GraphKey);
      GraphFailures.erase(GraphKey);
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
    // File has different contents, or indexing was successful this time.
    if (DigestIt == ShardVersionsSnapshot.end() ||
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

llvm::Expected<BackgroundIndex::IndexResult>
BackgroundIndex::index(tooling::CompileCommand Cmd) {
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
  if (llvm::Error Err = Action->Execute())
    return Err;

  Action->EndSourceFile();

  Index.Cmd = Inputs.CompileCommand;
  assert(Index.Graphs.size() == 1 && "graph collector did not produce one TU");
  Index.Graphs.front().MainFile = AbsolutePath.str().str();
  Index.Graphs.front().Directory = Inputs.CompileCommand.Directory;
  Index.Graphs.front().CommandLine = Inputs.CompileCommand.CommandLine;
  Index.Graphs.front().Output = Inputs.CompileCommand.Output;
  Index.Graphs.front().CommandDigest =
      graphCommandDigest(Inputs.CompileCommand);
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
  Index.Graphs.front().HadErrors = HadErrors;
  Index.Graphs.front().Diagnostics = Diagnostics.take();
  for (auto &Diagnostic : Index.Graphs.front().Diagnostics)
    if (!Diagnostic.Range.valid())
      Diagnostic.Range.FileURI = Index.Graphs.front().MainFileURI;
  IndexResult Result;
  Result.Index = std::move(Index);
  Result.ShardVersionsSnapshot = std::move(ShardVersionsSnapshot);
  Result.HadErrors = HadErrors;
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
      Graphs[graphMainKey(Graph.MainFile)][Graph.CommandDigest] =
          std::move(Graph);
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
  for (const auto &File : ChangedFiles)
    Changed.insert(graphMainKey(File));

  std::map<std::string, std::string> TUs;
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    for (const auto &Main : Graphs) {
      bool Affected = Changed.contains(graphMainKey(Main.getKey()));
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
BackgroundIndex::graphSnapshot(const GraphSnapshotParams &Params) const {
  auto Started = std::chrono::steady_clock::now();
  std::vector<GraphTU> Frozen;
  uint64_t Revision = 0;
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
    Revision = GraphRevision;
    for (const auto &Main : Graphs)
      for (const auto &Configuration : Main.getValue())
        Frozen.push_back(Configuration.second);
  }
  llvm::sort(Frozen, [](const GraphTU &Left, const GraphTU &Right) {
    return std::tie(Left.MainFile, Left.CommandDigest) <
           std::tie(Right.MainFile, Right.CommandDigest);
  });

  // Validate commands and source checkers outside GraphMu. Rechecking the
  // revision below makes this optimistic freeze atomic without blocking an
  // index worker on filesystem or compilation-database I/O.
  std::map<std::string, llvm::StringSet<>> ActualConfigurations;
  for (const auto &Graph : Frozen) {
    if (Graph.HadErrors)
      return error("graph snapshot contains an erroneous configuration: {0}",
                   Graph.MainFile);
    ActualConfigurations[Graph.MainFile].insert(Graph.CommandDigest);
  }
  for (const auto &Main : ActualConfigurations) {
    std::optional<WithContext> WithProvidedContext;
    if (ContextProvider)
      WithProvidedContext.emplace(ContextProvider(Main.first));
    llvm::StringSet<> Expected;
    for (const auto &Command : CDB.getCompileCommands(Main.first))
      Expected.insert(graphCommandDigest(Command));
    if (Expected.size() != Main.second.size() ||
        llvm::any_of(Expected, [&](const auto &Entry) {
          return !Main.second.contains(Entry.getKey());
        }))
      return contentModified(
          "graph snapshot compile commands moved during indexing: {0}",
          Main.first);
  }

  llvm::StringMap<std::string> CheckedSources;
  for (const auto &Graph : Frozen) {
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
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    if (Revision != GraphRevision || GraphDiscoveryPending != 0 ||
        !GraphPending.empty() || !GraphFailures.empty())
      return contentModified(
          "graph snapshot state moved while it was being frozen");
  }

  auto JSONText = [](llvm::json::Value Value) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    OS << Value;
    return OS.str();
  };
  const std::vector<std::pair<const char *, const char *>> Coverage = {
      {"contains", "complete"},   {"exports", "partial"},
      {"imports", "complete"},    {"calls", "partial"},
      {"accesses", "complete"},   {"instantiates", "partial"},
      {"type_ref", "complete"},   {"extends", "complete"},
      {"implements", "partial"},  {"overrides", "complete"},
      {"dispatches", "partial"},  {"decorates", "unsupported"},
      {"renders", "unsupported"}, {"tests", "unsupported"},
      {"references", "complete"}};

  struct EncodedShard {
    std::string Key;
    std::string Digest;
    llvm::json::Object JSON;
  };
  std::vector<EncodedShard> Shards;
  std::set<std::string> Targets;
  std::set<std::string> Configurations;
  std::set<std::string> WorkspaceRoots;
  for (const auto &Graph : Frozen) {
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
    llvm::json::Array CoverageJSON;
    for (const auto &Row : Coverage)
      CoverageJSON.push_back(
          llvm::json::Object{{"family", Row.first}, {"state", Row.second}});
    std::string GraphText = JSONText(toJSON(Graph));
    std::string ShardDigest =
        graphDigest(Key + "\n" + CheckerDigest + "\n" + InterfaceFingerprint +
                    "\n" + GraphText);
    llvm::json::Object Shard{{"key", Key},
                             {"source", Graph.MainFile},
                             {"configuration", Graph.CommandDigest},
                             {"checkerDigest", CheckerDigest},
                             {"interfaceFingerprint", InterfaceFingerprint},
                             {"digest", ShardDigest},
                             {"graph", toJSON(Graph)},
                             {"coverage", std::move(CoverageJSON)}};
    Shards.push_back(
        EncodedShard{std::move(Key), std::move(ShardDigest), std::move(Shard)});
    Targets.insert(Graph.TargetTriple);
    Configurations.insert(Graph.CommandDigest);
    if (auto Project = CDB.getProjectInfo(Graph.MainFile))
      WorkspaceRoots.insert(Project->SourceRoot);
  }

  llvm::json::Array Manifest;
  llvm::StringMap<std::string> CurrentManifest;
  std::string GenerationMaterial;
  llvm::raw_string_ostream GenerationOS(GenerationMaterial);
  for (const auto &Shard : Shards) {
    Manifest.push_back(
        llvm::json::Object{{"key", Shard.Key}, {"digest", Shard.Digest}});
    CurrentManifest[Shard.Key] = Shard.Digest;
    GenerationOS << Shard.Key.size() << ':' << Shard.Key << Shard.Digest;
  }
  for (const auto &Target : Targets)
    GenerationOS << "target:" << Target;
  std::string UniverseDigest = graphDigest(GenerationOS.str());
  std::string Generation = graphDigest(UniverseDigest + GenerationMaterial);

  llvm::json::Array Upserts;
  llvm::json::Array Deletes;
  std::optional<std::string> BaseGeneration;
  uint64_t Sequence;
  {
    std::lock_guard<std::mutex> Lock(GraphMu);
    if (Params.KnownGeneration && *Params.KnownGeneration == Generation) {
      BaseGeneration = Generation;
    } else if (Params.KnownGeneration &&
               *Params.KnownGeneration == PublishedGeneration) {
      BaseGeneration = PublishedGeneration;
      for (auto &Shard : Shards) {
        auto Old = PublishedManifest.find(Shard.Key);
        if (Old == PublishedManifest.end() || Old->getValue() != Shard.Digest)
          Upserts.push_back(std::move(Shard.JSON));
      }
      std::vector<std::string> DeletedKeys;
      for (const auto &Old : PublishedManifest)
        if (!CurrentManifest.contains(Old.getKey()))
          DeletedKeys.push_back(Old.getKey().str());
      llvm::sort(DeletedKeys);
      for (auto &Key : DeletedKeys)
        Deletes.push_back(std::move(Key));
    } else {
      for (auto &Shard : Shards)
        Upserts.push_back(std::move(Shard.JSON));
    }
    PublishedGeneration = Generation;
    PublishedManifest = CurrentManifest;
    Sequence = ++GraphSequence;
  }

  llvm::json::Array TargetJSON;
  for (const auto &Target : Targets)
    TargetJSON.push_back(Target);
  llvm::json::Array ConfigurationJSON;
  for (const auto &Configuration : Configurations)
    ConfigurationJSON.push_back(Configuration);
  llvm::json::Array RootJSON;
  for (const auto &Root : WorkspaceRoots)
    RootJSON.push_back(Root);
  llvm::json::Array Toolchains;
  Toolchains.push_back(getClangFullVersion());
  auto Elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - Started)
                     .count();
  return llvm::json::Object{
      {"protocolVersion", 1},
      {"schemaVersion", 1},
      {"producer",
       llvm::json::Object{{"name", "samchon-clangd"},
                          {"version", getClangFullVersion()},
                          {"commit", getClangFullRepositoryVersion()}}},
      {"universe",
       llvm::json::Object{{"digest", UniverseDigest},
                          {"targets", std::move(TargetJSON)},
                          {"workspaceRoots", std::move(RootJSON)},
                          {"toolchains", std::move(Toolchains)},
                          {"configurations", std::move(ConfigurationJSON)}}},
      {"sequence", static_cast<int64_t>(Sequence)},
      {"generation", Generation},
      {"baseGeneration", BaseGeneration ? llvm::json::Value(*BaseGeneration)
                                        : llvm::json::Value(nullptr)},
      {"upserts", std::move(Upserts)},
      {"deletes", std::move(Deletes)},
      {"manifest", std::move(Manifest)},
      {"phases",
       llvm::json::Object{{"semanticMillis", 0},
                          {"shardMillis", 0},
                          {"encodeMillis", static_cast<int64_t>(Elapsed)},
                          {"totalMillis", static_cast<int64_t>(Elapsed)},
                          {"cacheHit", BaseGeneration.has_value()}}}};
}

void BackgroundIndex::profile(MemoryTree &MT) const {
  IndexedSymbols.profile(MT.child("slabs"));
  // We don't want to mix memory used by index and symbols, so call base class.
  MT.child("index").addUsage(SwapIndex::estimateMemoryUsage());
}
} // namespace clangd
} // namespace clang
