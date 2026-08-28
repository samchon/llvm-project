#include "Annotations.h"
#include "CompileCommands.h"
#include "Config.h"
#include "Headers.h"
#include "Protocol.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "TestTU.h"
#include "index/Background.h"
#include "index/BackgroundRebuild.h"
#include "index/MemIndex.h"
#include "support/Cancellation.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

using ::testing::_;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Not;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

namespace clang {
namespace clangd {

MATCHER_P(named, N, "") { return arg.Name == N; }
MATCHER_P(qName, N, "") { return (arg.Scope + arg.Name).str() == N; }
MATCHER(declared, "") {
  return !StringRef(arg.CanonicalDeclaration.FileURI).empty();
}
MATCHER(defined, "") { return !StringRef(arg.Definition.FileURI).empty(); }
MATCHER_P(fileURI, F, "") { return StringRef(arg.Location.FileURI) == F; }
::testing::Matcher<const RefSlab &>
refsAre(std::vector<::testing::Matcher<Ref>> Matchers) {
  return ElementsAre(::testing::Pair(_, UnorderedElementsAreArray(Matchers)));
}
// URI cannot be empty since it references keys in the IncludeGraph.
MATCHER(emptyIncludeNode, "") {
  return arg.Flags == IncludeGraphNode::SourceFlag::None && !arg.URI.empty() &&
         arg.Digest == FileDigest{{0}} && arg.DirectIncludes.empty();
}

MATCHER(hadErrors, "") {
  return arg.Flags & IncludeGraphNode::SourceFlag::HadErrors;
}

MATCHER_P(numReferences, N, "") { return arg.References == N; }

void expectContentModified(llvm::Expected<llvm::json::Value> Result) {
  ASSERT_FALSE(Result);
  bool Matched = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const LSPError &Error) {
        Matched = true;
        EXPECT_EQ(ErrorCode::ContentModified, Error.Code);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        ADD_FAILURE() << "expected ContentModified, got: " << Error.message();
      });
  EXPECT_TRUE(Matched);
}

bool updateEnvironment(llvm::StringRef Name,
                       const std::optional<std::string> &Value) {
#if defined(_WIN32)
  return ::_putenv_s(Name.str().c_str(), Value ? Value->c_str() : "") == 0;
#else
  return Value ? ::setenv(Name.str().c_str(), Value->c_str(), 1) == 0
               : ::unsetenv(Name.str().c_str()) == 0;
#endif
}

class ScopedEnvironmentUnset {
public:
  explicit ScopedEnvironmentUnset(llvm::StringRef Name)
      : Name(Name.str()), Previous(llvm::sys::Process::GetEnv(Name)),
        Cleared(updateEnvironment(Name, std::nullopt)) {}
  ~ScopedEnvironmentUnset() { updateEnvironment(Name, Previous); }

  explicit operator bool() const { return Cleared; }

private:
  std::string Name;
  std::optional<std::string> Previous;
  bool Cleared;
};

class MemoryShardStorage : public BackgroundIndexStorage {
  mutable std::mutex StorageMu;
  llvm::StringMap<std::string> &Storage;
  size_t &CacheHits;

public:
  MemoryShardStorage(llvm::StringMap<std::string> &Storage, size_t &CacheHits)
      : Storage(Storage), CacheHits(CacheHits) {}
  llvm::Error storeShard(llvm::StringRef ShardIdentifier,
                         IndexFileOut Shard) const override {
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    Storage[ShardIdentifier] = llvm::to_string(Shard);
    return llvm::Error::success();
  }
  std::unique_ptr<IndexFileIn>
  loadShard(llvm::StringRef ShardIdentifier) const override {
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    if (!Storage.contains(ShardIdentifier)) {
      return nullptr;
    }
    auto IndexFile =
        readIndexFile(Storage[ShardIdentifier], SymbolOrigin::Background);
    if (!IndexFile) {
      ADD_FAILURE() << "Error while reading " << ShardIdentifier << ':'
                    << IndexFile.takeError();
      return nullptr;
    }
    CacheHits++;
    return std::make_unique<IndexFileIn>(std::move(*IndexFile));
  }

  // In-memory storage has no path a separate process could read, so it never
  // offers one and every snapshot it serves carries its bodies inline.
  std::string storeGraphBody(llvm::StringRef, llvm::StringRef) const override {
    return std::string();
  }

  mutable llvm::StringSet<> AccessedPaths;
};

class BackgroundIndexTest : public ::testing::Test {
protected:
  BackgroundIndexTest() { BackgroundQueue::preventThreadStarvationInTests(); }
};

class MultiCommandCDB : public GlobalCompilationDatabase {
public:
  std::optional<tooling::CompileCommand>
  getCompileCommand(PathRef File) const override {
    auto All = getCompileCommands(File);
    if (All.empty())
      return std::nullopt;
    return All.front();
  }

  std::vector<tooling::CompileCommand>
  getCompileCommands(PathRef File) const override {
    auto It = Commands.find(File);
    return It == Commands.end() ? std::vector<tooling::CompileCommand>()
                                : It->second;
  }

  llvm::StringMap<std::vector<tooling::CompileCommand>> Commands;
};

class InferringCDB : public MultiCommandCDB {
public:
  std::vector<tooling::CompileCommand>
  getCompileCommands(PathRef File) const override {
    auto Exact = MultiCommandCDB::getCompileCommands(File);
    if (!Exact.empty() || Commands.empty())
      return Exact;
    tooling::CompileCommand Inferred = Commands.begin()->second.front();
    Inferred.Filename = File.str();
    Inferred.CommandLine.back() = File.str();
    return {std::move(Inferred)};
  }
};

TEST_F(BackgroundIndexTest, NoCrashOnErrorFile) {
  MockFS FS;
  FS.Files[testPath("root/A.cc")] = "error file";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "-DA=1", testPath("root/A.cc")};
  CDB.setCompileCommand(testPath("root/A.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
}

TEST_F(BackgroundIndexTest, Config) {
  MockFS FS;
  // Set up two identical TUs, foo and bar.
  // They define foo::one and bar::one.
  std::vector<tooling::CompileCommand> Cmds;
  for (std::string Name : {"foo", "bar", "baz"}) {
    std::string Filename = Name + ".cpp";
    std::string Header = Name + ".h";
    FS.Files[Filename] = "#include \"" + Header + "\"";
    FS.Files[Header] = "namespace " + Name + " { int one; }";
    tooling::CompileCommand Cmd;
    Cmd.Filename = Filename;
    Cmd.Directory = testRoot();
    Cmd.CommandLine = {"clang++", Filename};
    Cmds.push_back(std::move(Cmd));
  }
  // Context provider that installs a configuration mutating foo's command.
  // This causes it to define foo::two instead of foo::one.
  // It also disables indexing of baz entirely.
  BackgroundIndex::Options Opts;
  Opts.ContextProvider = [](PathRef P) {
    Config C;
    if (P.ends_with("foo.cpp"))
      C.CompileFlags.Edits.push_back([](std::vector<std::string> &Argv) {
        Argv = tooling::getInsertArgumentAdjuster("-Done=two")(Argv, "");
      });
    if (P.ends_with("baz.cpp"))
      C.Index.Background = Config::BackgroundPolicy::Skip;
    return Context::current().derive(Config::Key, std::move(C));
  };
  // Create the background index.
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  // We need the CommandMangler, because that applies the config we're testing.
  OverlayCDB CDB(/*Base=*/nullptr, /*FallbackFlags=*/{},
                 CommandMangler::forTests());

  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  // Index the two files.
  for (auto &Cmd : Cmds) {
    std::string FullPath = testPath(Cmd.Filename);
    CDB.setCompileCommand(FullPath, std::move(Cmd));
  }
  // Wait for both files to be indexed.
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT(runFuzzyFind(Idx, ""),
              UnorderedElementsAre(qName("foo"), qName("foo::two"),
                                   qName("bar"), qName("bar::one")));
}

TEST_F(BackgroundIndexTest, IndexTwoFiles) {
  MockFS FS;
  // a.h yields different symbols when included by A.cc vs B.cc.
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      #if A
        class A_CC {};
      #else
        class B_CC{};
      #endif
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nstatic void g() { (void)common; }";
  FS.Files[testPath("root/B.cc")] =
      R"cpp(
      #define A 0
      #include "A.h"
      void f_b() {
        (void)common;
        (void)common;
        (void)common;
        (void)common;
      })cpp";
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "-DA=1", testPath("root/A.cc")};
  CDB.setCompileCommand(testPath("root/A.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT(runFuzzyFind(Idx, ""),
              UnorderedElementsAre(AllOf(named("common"), numReferences(1U)),
                                   AllOf(named("A_CC"), numReferences(0U)),
                                   AllOf(named("g"), numReferences(1U)),
                                   AllOf(named("f_b"), declared(),
                                         Not(defined()), numReferences(0U))));

  Cmd.Filename = testPath("root/B.cc");
  Cmd.CommandLine = {"clang++", Cmd.Filename};
  CDB.setCompileCommand(testPath("root/B.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  // B_CC is dropped as we don't collect symbols from A.h in this compilation.
  EXPECT_THAT(runFuzzyFind(Idx, ""),
              UnorderedElementsAre(AllOf(named("common"), numReferences(5U)),
                                   AllOf(named("A_CC"), numReferences(0U)),
                                   AllOf(named("g"), numReferences(1U)),
                                   AllOf(named("f_b"), declared(), defined(),
                                         numReferences(1U))));

  auto Syms = runFuzzyFind(Idx, "common");
  EXPECT_THAT(Syms, UnorderedElementsAre(named("common")));
  auto Common = *Syms.begin();
  EXPECT_THAT(getRefs(Idx, Common.ID),
              refsAre({fileURI("unittest:///root/A.h"),
                       fileURI("unittest:///root/A.cc"),
                       fileURI("unittest:///root/B.cc"),
                       fileURI("unittest:///root/B.cc"),
                       fileURI("unittest:///root/B.cc"),
                       fileURI("unittest:///root/B.cc")}));
}

TEST_F(BackgroundIndexTest, ConstructorForwarding) {
  Annotations Header(R"cpp(
    namespace std {
    template <class T> T &&forward(T &t);
    template <class T, class... Args> T *make_unique(Args &&...args) {
      return new T(std::forward<Args>(args)...);
    }
    }
    struct Test {
      [[Test]](){}
    };
  )cpp");
  Annotations Main(R"cpp(
    #include "header.hpp"
    int main() {
      auto a = std::[[make_unique]]<Test>();
    }
  )cpp");

  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  FS.Files[testPath("root/header.hpp")] = Header.code();
  FS.Files[testPath("root/test.cpp")] = Main.code();

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/test.cpp");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/test.cpp")};
  CDB.setCompileCommand(testPath("root/test.cpp"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Syms = runFuzzyFind(Idx, "Test");
  auto Constructor =
      std::find_if(Syms.begin(), Syms.end(), [](const Symbol &S) {
        return S.SymInfo.Kind == index::SymbolKind::Constructor;
      });
  ASSERT_TRUE(Constructor != Syms.end());
  EXPECT_THAT(getRefs(Idx, Constructor->ID),
              refsAre({fileURI("unittest:///root/header.hpp"),
                       fileURI("unittest:///root/test.cpp")}));
}

TEST_F(BackgroundIndexTest, ConstructorForwardingMultiFile) {
  // If a forwarding function like `make_unique` is defined in a header its body
  // used to be skipped on the second encounter. This meant in practise we could
  // only find constructors indirectly called by these type of functions in the
  // first indexed file (and all files that were indexed at the same time,
  // before a flag to skip it was set).
  Annotations Header(R"cpp(
    namespace std {
    template <class T> T &&forward(T &t);
    template <class T, class... Args> T *make_unique(Args &&...args) {
      return new T(std::forward<Args>(args)...);
    }
    }
    struct Test {
      [[Test]](){}
    };
  )cpp");
  Annotations First(R"cpp(
    #include "header.hpp"
    int main() {
      auto a = std::[[make_unique]]<Test>();
    }
  )cpp");
  Annotations Second(R"cpp(
    #include "header.hpp"
    void test() {
      auto a = std::[[make_unique]]<Test>();
    }
  )cpp");

  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  FS.Files[testPath("root/header.hpp")] = Header.code();
  FS.Files[testPath("root/first.cpp")] = First.code();
  FS.Files[testPath("root/second.cpp")] = Second.code();

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/first.cpp");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/first.cpp")};
  CDB.setCompileCommand(testPath("root/first.cpp"), Cmd);

  // Make sure the first file is done indexing to make sure the flag for the
  // header is set.
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  Cmd.Filename = testPath("root/second.cpp");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/second.cpp")};
  CDB.setCompileCommand(testPath("root/second.cpp"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Syms = runFuzzyFind(Idx, "Test");
  auto Constructor =
      std::find_if(Syms.begin(), Syms.end(), [](const Symbol &S) {
        return S.SymInfo.Kind == index::SymbolKind::Constructor;
      });
  ASSERT_TRUE(Constructor != Syms.end());
  EXPECT_THAT(getRefs(Idx, Constructor->ID),
              refsAre({fileURI("unittest:///root/header.hpp"),
                       fileURI("unittest:///root/first.cpp"),
                       fileURI("unittest:///root/second.cpp")}));
}

TEST_F(BackgroundIndexTest, MainFileRefs) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void header_sym();
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nstatic void main_sym() { (void)header_sym; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex::Options Opts;
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, Opts);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  CDB.setCompileCommand(testPath("root/A.cc"), Cmd);

  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  EXPECT_THAT(
      runFuzzyFind(Idx, ""),
      UnorderedElementsAre(AllOf(named("header_sym"), numReferences(1U)),
                           AllOf(named("main_sym"), numReferences(1U))));
}

TEST_F(BackgroundIndexTest, ShardStorageTest) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/A.cc")] = R"cpp(
      #include "A.h"
      void g() { (void)common; }
      class B_CC : public A_CC {};
      )cpp";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  // Check nothing is loaded from Storage, but A.cc and A.h has been stored.
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 0U);
  EXPECT_EQ(Storage.size(), 2U);

  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 2U); // Check both A.cc and A.h loaded from cache.
  EXPECT_EQ(Storage.size(), 2U);

  auto ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(
      *ShardHeader->Symbols,
      UnorderedElementsAre(named("common"), named("A_CC"),
                           AllOf(named("f_b"), declared(), Not(defined()))));
  for (const auto &Ref : *ShardHeader->Refs)
    EXPECT_THAT(Ref.second,
                UnorderedElementsAre(fileURI("unittest:///root/A.h")));

  auto ShardSource = MSS.loadShard(testPath("root/A.cc"));
  EXPECT_NE(ShardSource, nullptr);
  EXPECT_THAT(*ShardSource->Symbols,
              UnorderedElementsAre(named("g"), named("B_CC")));
  for (const auto &Ref : *ShardSource->Refs)
    EXPECT_THAT(Ref.second,
                UnorderedElementsAre(fileURI("unittest:///root/A.cc")));

  // The BaseOf relationship between A_CC and B_CC is stored in both the file
  // containing the definition of the subject (A_CC) and the file containing
  // the definition of the object (B_CC).
  SymbolID A = findSymbol(*ShardHeader->Symbols, "A_CC").ID;
  SymbolID B = findSymbol(*ShardSource->Symbols, "B_CC").ID;
  EXPECT_THAT(*ShardHeader->Relations,
              UnorderedElementsAre(Relation{A, RelationKind::BaseOf, B}));
  EXPECT_THAT(*ShardSource->Relations,
              UnorderedElementsAre(Relation{A, RelationKind::BaseOf, B}));
}

TEST_F(BackgroundIndexTest, DirectIncludesTest) {
  MockFS FS;
  FS.Files[testPath("root/B.h")] = "";
  FS.Files[testPath("root/A.h")] = R"cpp(
      #include "B.h"
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nvoid g() { (void)common; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  auto ShardSource = MSS.loadShard(testPath("root/A.cc"));
  EXPECT_TRUE(ShardSource->Sources);
  EXPECT_EQ(ShardSource->Sources->size(), 2U); // A.cc, A.h
  EXPECT_THAT(
      ShardSource->Sources->lookup("unittest:///root/A.cc").DirectIncludes,
      UnorderedElementsAre("unittest:///root/A.h"));
  EXPECT_NE(ShardSource->Sources->lookup("unittest:///root/A.cc").Digest,
            FileDigest{{0}});
  EXPECT_THAT(ShardSource->Sources->lookup("unittest:///root/A.h"),
              emptyIncludeNode());

  auto ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_TRUE(ShardHeader->Sources);
  EXPECT_EQ(ShardHeader->Sources->size(), 2U); // A.h, B.h
  EXPECT_THAT(
      ShardHeader->Sources->lookup("unittest:///root/A.h").DirectIncludes,
      UnorderedElementsAre("unittest:///root/B.h"));
  EXPECT_NE(ShardHeader->Sources->lookup("unittest:///root/A.h").Digest,
            FileDigest{{0}});
  EXPECT_THAT(ShardHeader->Sources->lookup("unittest:///root/B.h"),
              emptyIncludeNode());
}

TEST_F(BackgroundIndexTest, ShardStorageLoad) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nvoid g() { (void)common; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  // Check nothing is loaded from Storage, but A.cc and A.h has been stored.
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }

  // Change header.
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      class A_CCnew {};
      )cpp";
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 2U); // Check both A.cc and A.h loaded from cache.

  // Check if the new symbol has arrived.
  auto ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(*ShardHeader->Symbols, Contains(named("A_CCnew")));

  // Change source.
  FS.Files[testPath("root/A.cc")] =
      "#include \"A.h\"\nvoid g() { (void)common; }\nvoid f_b() {}";
  {
    CacheHits = 0;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 2U); // Check both A.cc and A.h loaded from cache.

  // Check if the new symbol has arrived.
  ShardHeader = MSS.loadShard(testPath("root/A.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(*ShardHeader->Symbols, Contains(named("A_CCnew")));
  auto ShardSource = MSS.loadShard(testPath("root/A.cc"));
  EXPECT_NE(ShardSource, nullptr);
  EXPECT_THAT(*ShardSource->Symbols,
              Contains(AllOf(named("f_b"), declared(), defined())));
}

TEST_F(BackgroundIndexTest, ShardStorageEmptyFile) {
  MockFS FS;
  FS.Files[testPath("root/A.h")] = R"cpp(
      void common();
      void f_b();
      class A_CC {};
      )cpp";
  FS.Files[testPath("root/B.h")] = R"cpp(
      #include "A.h"
      )cpp";
  FS.Files[testPath("root/A.cc")] =
      "#include \"B.h\"\nvoid g() { (void)common; }";

  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);

  tooling::CompileCommand Cmd;
  Cmd.Filename = testPath("root/A.cc");
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", testPath("root/A.cc")};
  // Check that A.cc, A.h and B.h has been stored.
  {
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_THAT(Storage.keys(),
              UnorderedElementsAre(testPath("root/A.cc"), testPath("root/A.h"),
                                   testPath("root/B.h")));
  auto ShardHeader = MSS.loadShard(testPath("root/B.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_TRUE(ShardHeader->Symbols->empty());

  // Check that A.cc, A.h and B.h has been loaded.
  {
    CacheHits = 0;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 3U);

  // Update B.h to contain some symbols.
  FS.Files[testPath("root/B.h")] = R"cpp(
      #include "A.h"
      void new_func();
      )cpp";
  // Check that B.h has been stored with new contents.
  {
    CacheHits = 0;
    OverlayCDB CDB(/*Base=*/nullptr);
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                        /*Opts=*/{});
    CDB.setCompileCommand(testPath("root/A.cc"), Cmd);
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  EXPECT_EQ(CacheHits, 3U);
  ShardHeader = MSS.loadShard(testPath("root/B.h"));
  EXPECT_NE(ShardHeader, nullptr);
  EXPECT_THAT(*ShardHeader->Symbols,
              Contains(AllOf(named("new_func"), declared(), Not(defined()))));
}

TEST_F(BackgroundIndexTest, NoDotsInAbsPath) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  tooling::CompileCommand Cmd;
  FS.Files[testPath("root/A.cc")] = "";
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("root/build");
  Cmd.CommandLine = {"clang++", "../A.cc"};
  CDB.setCompileCommand(testPath("root/build/../A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  FS.Files[testPath("root/B.cc")] = "";
  Cmd.Filename = "./B.cc";
  Cmd.Directory = testPath("root");
  Cmd.CommandLine = {"clang++", "./B.cc"};
  CDB.setCompileCommand(testPath("root/./B.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  for (llvm::StringRef AbsPath : MSS.AccessedPaths.keys()) {
    EXPECT_FALSE(AbsPath.contains("./")) << AbsPath;
    EXPECT_FALSE(AbsPath.contains("../")) << AbsPath;
  }
}

TEST_F(BackgroundIndexTest, UncompilableFiles) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  tooling::CompileCommand Cmd;
  FS.Files[testPath("A.h")] = "void foo();";
  FS.Files[testPath("B.h")] = "#include \"C.h\"\nasdf;";
  FS.Files[testPath("C.h")] = "";
  FS.Files[testPath("A.cc")] = R"cpp(
  #include "A.h"
  #include "B.h"
  #include "not_found_header.h"

  void foo() {}
  )cpp";
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("build");
  Cmd.CommandLine = {"clang++", "../A.cc"};
  CDB.setCompileCommand(testPath("build/../A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_THAT(Storage.keys(),
              UnorderedElementsAre(testPath("A.cc"), testPath("A.h"),
                                   testPath("B.h"), testPath("C.h")));

  {
    auto Shard = MSS.loadShard(testPath("A.cc"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre(named("foo")));
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///A.cc", "unittest:///A.h",
                                     "unittest:///B.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///A.cc"), hadErrors());
  }

  {
    auto Shard = MSS.loadShard(testPath("A.h"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre(named("foo")));
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///A.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///A.h"), hadErrors());
  }

  {
    auto Shard = MSS.loadShard(testPath("B.h"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre(named("asdf")));
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///B.h", "unittest:///C.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///B.h"), hadErrors());
  }

  {
    auto Shard = MSS.loadShard(testPath("C.h"));
    EXPECT_THAT(*Shard->Symbols, UnorderedElementsAre());
    EXPECT_THAT(Shard->Sources->keys(),
                UnorderedElementsAre("unittest:///C.h"));
    EXPECT_THAT(Shard->Sources->lookup("unittest:///C.h"), hadErrors());
  }
}

TEST_F(BackgroundIndexTest, CmdLineHash) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  tooling::CompileCommand Cmd;
  FS.Files[testPath("A.cc")] = "#include \"A.h\"";
  FS.Files[testPath("A.h")] = "";
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("build");
  Cmd.CommandLine = {"clang++", "../A.cc", "-fsyntax-only"};
  CDB.setCompileCommand(testPath("build/../A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_THAT(Storage.keys(),
              UnorderedElementsAre(testPath("A.cc"), testPath("A.h")));
  // Make sure we only store the Cmd for main file.
  EXPECT_FALSE(MSS.loadShard(testPath("A.h"))->Cmd);

  tooling::CompileCommand CmdStored = *MSS.loadShard(testPath("A.cc"))->Cmd;
  EXPECT_EQ(CmdStored.CommandLine, Cmd.CommandLine);
  EXPECT_EQ(CmdStored.Directory, Cmd.Directory);
}

TEST_F(BackgroundIndexTest, Reindex) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; },
                      /*Opts=*/{});

  // Index a file.
  FS.Files[testPath("A.cc")] = "int theOldFunction();";
  tooling::CompileCommand Cmd;
  Cmd.Filename = "../A.cc";
  Cmd.Directory = testPath("build");
  Cmd.CommandLine = {"clang++", "../A.cc", "-fsyntax-only"};
  CDB.setCompileCommand(testPath("A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  // Verify the result is indexed and stored.
  EXPECT_EQ(1u, runFuzzyFind(Idx, "theOldFunction").size());
  EXPECT_EQ(0u, runFuzzyFind(Idx, "theNewFunction").size());
  std::string OldShard = Storage.lookup(testPath("A.cc"));
  EXPECT_NE("", OldShard);

  // Change the content and command, and notify to reindex it.
  Cmd.CommandLine.push_back("-DFOO");
  FS.Files[testPath("A.cc")] = "int theNewFunction();";
  CDB.setCompileCommand(testPath("A.cc"), Cmd);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  EXPECT_EQ(0u, runFuzzyFind(Idx, "theOldFunction").size());
  EXPECT_EQ(1u, runFuzzyFind(Idx, "theNewFunction").size());
  EXPECT_NE(OldShard, Storage.lookup(testPath("A.cc")));
}

TEST_F(BackgroundIndexTest, ColdGraphSnapshotIsRetryable) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  expectContentModified(Idx.graphSnapshot({}));
}

TEST_F(BackgroundIndexTest, GraphSnapshotHonorsRequestCancellation) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  auto Task = cancelableTask();
  Task.second();
  WithContext Cancelled(std::move(Task.first));
  auto Result = Idx.graphSnapshot({});
  ASSERT_FALSE(Result);
  auto Error = Result.takeError();
  EXPECT_TRUE(Error.isA<CancelledError>());
  llvm::consumeError(std::move(Error));
}

TEST(SystemIncludeExtractorSubprocess, DISABLED_Sleeps) {
  std::this_thread::sleep_for(std::chrono::seconds(60));
}

TEST(SystemIncludeExtractorSubprocess, DISABLED_Exits) {}

TEST(SystemIncludeExtractorSubprocess, DISABLED_WritesAfterDelay) {
  const auto Ready = llvm::sys::Process::GetEnv("CLANGD_QUERY_DRIVER_READY");
  const auto Done = llvm::sys::Process::GetEnv("CLANGD_QUERY_DRIVER_DONE");
  ASSERT_TRUE(Ready);
  ASSERT_TRUE(Done);
  std::error_code EC;
  llvm::raw_fd_ostream(*Ready, EC).close();
  ASSERT_FALSE(EC);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  llvm::raw_fd_ostream(*Done, EC).close();
  ASSERT_FALSE(EC);
}

TEST(SystemIncludeExtractorSubprocess, DISABLED_SpawnsGrandchild) {
  const std::string Executable =
      llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallVector<llvm::StringRef> Args = {
      Executable, "--gtest_also_run_disabled_tests",
      "--gtest_filter=SystemIncludeExtractorSubprocess."
      "DISABLED_WritesAfterDelay"};
  std::string Error;
  bool ExecutionFailed = false;
  auto Grandchild = llvm::sys::ExecuteNoWait(
      Executable, Args, /*Env=*/std::nullopt, /*Redirects=*/{},
      /*MemoryLimit=*/0, &Error, &ExecutionFailed);
  ASSERT_FALSE(ExecutionFailed) << Error;
  ASSERT_NE(Grandchild.Pid, llvm::sys::ProcessInfo::InvalidPid);
  std::this_thread::sleep_for(std::chrono::seconds(60));
}

TEST_F(BackgroundIndexTest, QueryDriverExecutionIsCancellable) {
  ScopedEnvironmentUnset TotalShards("GTEST_TOTAL_SHARDS");
  ScopedEnvironmentUnset ShardIndex("GTEST_SHARD_INDEX");
  ASSERT_TRUE(TotalShards);
  ASSERT_TRUE(ShardIndex);
  const std::string Executable =
      llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallVector<llvm::StringRef> Args = {
      Executable, "--gtest_also_run_disabled_tests",
      "--gtest_filter=SystemIncludeExtractorSubprocess.DISABLED_Sleeps"};
  auto Task = cancelableTask();
  std::thread Canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Task.second();
  });
  const auto Started = std::chrono::steady_clock::now();
  WithContext Cancellable(std::move(Task.first));
  EXPECT_FALSE(runSystemIncludeExtractorForTest(Args, /*OutputIsStderr=*/false,
                                                std::chrono::seconds(30)));
  Canceller.join();
  EXPECT_LT(std::chrono::steady_clock::now() - Started,
            std::chrono::seconds(5));
}

TEST_F(BackgroundIndexTest, QueryDriverExecutionHasFiniteTimeout) {
  ScopedEnvironmentUnset TotalShards("GTEST_TOTAL_SHARDS");
  ScopedEnvironmentUnset ShardIndex("GTEST_SHARD_INDEX");
  ASSERT_TRUE(TotalShards);
  ASSERT_TRUE(ShardIndex);
  const std::string Executable =
      llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallVector<llvm::StringRef> Args = {
      Executable, "--gtest_also_run_disabled_tests",
      "--gtest_filter=SystemIncludeExtractorSubprocess.DISABLED_Sleeps"};
  const auto Started = std::chrono::steady_clock::now();
  EXPECT_FALSE(runSystemIncludeExtractorForTest(
      Args, /*OutputIsStderr=*/false, std::chrono::milliseconds(100)));
  EXPECT_LT(std::chrono::steady_clock::now() - Started,
            std::chrono::seconds(5));
}

TEST_F(BackgroundIndexTest, QueryDriverExecutionIsConcurrentAndExact) {
  ScopedEnvironmentUnset TotalShards("GTEST_TOTAL_SHARDS");
  ScopedEnvironmentUnset ShardIndex("GTEST_SHARD_INDEX");
  ASSERT_TRUE(TotalShards);
  ASSERT_TRUE(ShardIndex);
  const std::string Executable =
      llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallVector<llvm::StringRef> SlowArgs = {
      Executable, "--gtest_also_run_disabled_tests",
      "--gtest_filter=SystemIncludeExtractorSubprocess.DISABLED_Sleeps"};
  llvm::SmallVector<llvm::StringRef> FastArgs = {
      Executable, "--gtest_also_run_disabled_tests",
      "--gtest_filter=SystemIncludeExtractorSubprocess.DISABLED_Exits"};
  std::optional<std::string> Slow;
  std::optional<std::string> Fast;
  std::thread SlowQuery([&] {
    Slow = runSystemIncludeExtractorForTest(SlowArgs, /*OutputIsStderr=*/false,
                                            std::chrono::milliseconds(100));
  });
  std::thread FastQuery([&] {
    Fast = runSystemIncludeExtractorForTest(FastArgs, /*OutputIsStderr=*/false,
                                            std::chrono::seconds(5));
  });
  SlowQuery.join();
  FastQuery.join();
  EXPECT_FALSE(Slow);
  EXPECT_TRUE(Fast);
}

TEST_F(BackgroundIndexTest, QueryDriverCachesOnlyCompleteSuccess) {
  EXPECT_EQ(4u, runSystemIncludeExtractorCacheForTest({1, 2, 3, 0, 1}));
}

TEST_F(BackgroundIndexTest, QueryDriverScopeDeduplicatesEveryStatus) {
  for (unsigned Status : {1u, 2u, 3u}) {
    EXPECT_EQ(1u,
              runSystemIncludeExtractorCacheForTest({Status, 0},
                                                    /*OperationLocal=*/true));
    EXPECT_EQ(1u, runSystemIncludeExtractorCacheForTest(
                      {Status, 0}, /*OperationLocal=*/true,
                      /*RefreshQueries=*/true));
  }
}

TEST_F(BackgroundIndexTest, QueryDriverTerminationOwnsDescendants) {
  ScopedEnvironmentUnset TotalShards("GTEST_TOTAL_SHARDS");
  ScopedEnvironmentUnset ShardIndex("GTEST_SHARD_INDEX");
  ASSERT_TRUE(TotalShards);
  ASSERT_TRUE(ShardIndex);
  llvm::SmallString<128> Ready;
  llvm::SmallString<128> Done;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("clangd-query-ready", "", Ready));
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("clangd-query-done", "", Done));
  ASSERT_FALSE(llvm::sys::fs::remove(Ready));
  ASSERT_FALSE(llvm::sys::fs::remove(Done));
  llvm::FileRemover RemoveReady(Ready);
  llvm::FileRemover RemoveDone(Done);
  const auto PreviousReady =
      llvm::sys::Process::GetEnv("CLANGD_QUERY_DRIVER_READY");
  const auto PreviousDone =
      llvm::sys::Process::GetEnv("CLANGD_QUERY_DRIVER_DONE");
  ASSERT_TRUE(
      updateEnvironment("CLANGD_QUERY_DRIVER_READY", Ready.str().str()));
  ASSERT_TRUE(updateEnvironment("CLANGD_QUERY_DRIVER_DONE", Done.str().str()));
  auto RestoreEnvironment = llvm::make_scope_exit([&] {
    updateEnvironment("CLANGD_QUERY_DRIVER_READY", PreviousReady);
    updateEnvironment("CLANGD_QUERY_DRIVER_DONE", PreviousDone);
  });
  const std::string Executable =
      llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallVector<llvm::StringRef> Args = {
      Executable, "--gtest_also_run_disabled_tests",
      "--gtest_filter=SystemIncludeExtractorSubprocess."
      "DISABLED_SpawnsGrandchild"};
  EXPECT_FALSE(runSystemIncludeExtractorForTest(Args, /*OutputIsStderr=*/false,
                                                std::chrono::seconds(1)));
  EXPECT_TRUE(llvm::sys::fs::exists(Ready));
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  EXPECT_FALSE(llvm::sys::fs::exists(Done));
}

TEST_F(BackgroundIndexTest, WatchedMetadataIsNotATranslationUnit) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  InferringCDB CDB;
  const std::string Source = testPath("main.cpp");
  const std::string Database = testPath("compile_commands.json");
  FS.Files[Source] = "int main() { return 0; }";
  FS.Files[Database] = "[]";
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = Source;
  Command.CommandLine = {"clang++", "-fsyntax-only", Source};
  CDB.Commands[Source] = {Command};
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({Source});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  Idx.enqueueGraphDependents(std::vector<std::string>{Database});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Snapshot = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  ASSERT_TRUE(Snapshot->getAsObject()->getArray("manifest"));
  EXPECT_EQ(1u, Snapshot->getAsObject()->getArray("manifest")->size());
}

TEST_F(BackgroundIndexTest, GraphSnapshotUsesTranslationUnitContext) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  const std::string Source = testPath("context.cpp");
  FS.Files[Source] = "int configured();";
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = Source;
  Command.CommandLine = {"clang++", "-fsyntax-only", Source};
  OverlayCDB CDB(/*Base=*/nullptr, /*FallbackFlags=*/{},
                 CommandMangler::forTests());
  CDB.setCompileCommand(Source, Command);
  std::atomic<bool> ChangedConfig = false;
  BackgroundIndex::Options Opts;
  Opts.ContextProvider = [&](PathRef Path) {
    Config C;
    if (!Path.empty())
      C.CompileFlags.Edits.push_back([Changed = ChangedConfig.load()](
                                         std::vector<std::string> &Arguments) {
        Arguments.insert(Arguments.begin() + 1,
                         Changed ? "-DGRAPH_CONTEXT_V2" : "-DGRAPH_CONTEXT");
      });
    return Context::current().derive(Config::Key, std::move(C));
  };
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  Idx.enqueue({Source});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Snapshot = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  ASSERT_TRUE(Snapshot->getAsObject()->getArray("manifest"));
  EXPECT_EQ(1u, Snapshot->getAsObject()->getArray("manifest")->size());
  const auto InitialGeneration =
      Snapshot->getAsObject()->getString("generation")->str();

  // A config-only move is discovered while validating the resident snapshot.
  // It must schedule the owning TU instead of returning ContentModified
  // forever with no work capable of advancing the graph.
  ChangedConfig = true;
  expectContentModified(Idx.graphSnapshot({}));
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Reconfigured = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Reconfigured)) << llvm::toString(Reconfigured.takeError());
  EXPECT_NE(Reconfigured->getAsObject()->getString("generation"),
            InitialGeneration);
  const auto *Graph = (*Reconfigured->getAsObject()->getArray("upserts"))[0]
                          .getAsObject()
                          ->getObject("graph");
  ASSERT_TRUE(Graph);
  EXPECT_TRUE(llvm::is_contained(*Graph->getArray("commandLine"),
                                 "-DGRAPH_CONTEXT_V2"));
  EXPECT_TRUE(llvm::any_of(*Graph->getArray("symbols"), [](const auto &Value) {
    const auto *Symbol = Value.getAsObject();
    return Symbol && Symbol->getString("name") == "configured";
  }));

  // Config-only reindexing must replace the persisted complete view as well as
  // the resident one, despite unchanged source bytes.
  auto Persisted = MSS.loadShard(Source);
  ASSERT_TRUE(Persisted);
  ASSERT_EQ(1u, Persisted->Graphs.size());
  EXPECT_TRUE(llvm::is_contained(Persisted->Graphs.front().CommandLine,
                                 "-DGRAPH_CONTEXT_V2"));
  EXPECT_TRUE(llvm::any_of(
      Persisted->Graphs.front().Symbols,
      [](const GraphSymbol &Symbol) { return Symbol.Name == "configured"; }));
}

TEST_F(BackgroundIndexTest, ReplacedCompilerDriverStartsANewUniverse) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  const std::string Source = testPath("toolchain.cpp");
  FS.Files[Source] = "int toolchain();";

  int FD = -1;
  llvm::SmallString<128> Driver;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("clangd-driver", "", FD, Driver));
  llvm::FileRemover Cleanup(Driver);
  {
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS << "first wrapper";
  }
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = Source;
  Command.CommandLine = {Driver.str().str(), "-fsyntax-only", Source};
  CDB.Commands[Source] = {Command};

  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({Source});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Initial = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Initial)) << llvm::toString(Initial.takeError());
  const auto InitialGeneration =
      Initial->getAsObject()->getString("generation")->str();
  const auto InitialUniverse =
      Initial->getAsObject()->getObject("universe")->getString("digest")->str();

  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(Driver, EC, llvm::sys::fs::OF_None);
    ASSERT_FALSE(EC);
    OS << "second wrapper";
  }
  expectContentModified(Idx.graphSnapshot({}));
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Reindexed = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Reindexed)) << llvm::toString(Reindexed.takeError());
  EXPECT_NE(Reindexed->getAsObject()->getString("generation"),
            InitialGeneration);
  EXPECT_NE(
      Reindexed->getAsObject()->getObject("universe")->getString("digest"),
      InitialUniverse);
  const auto *Graph = (*Reindexed->getAsObject()->getArray("upserts"))[0]
                          .getAsObject()
                          ->getObject("graph");
  ASSERT_TRUE(Graph);
  EXPECT_TRUE(llvm::any_of(*Graph->getArray("symbols"), [](const auto &Value) {
    const auto *Symbol = Value.getAsObject();
    return Symbol && Symbol->getString("name") == "toolchain";
  }));

  auto Persisted = MSS.loadShard(Source);
  ASSERT_TRUE(Persisted);
  ASSERT_EQ(1u, Persisted->Graphs.size());
  EXPECT_EQ(Persisted->Graphs.front().ToolchainFingerprint,
            compileCommandToolchainFingerprint(Command));
  EXPECT_TRUE(llvm::any_of(
      Persisted->Graphs.front().Symbols,
      [](const GraphSymbol &Symbol) { return Symbol.Name == "toolchain"; }));
}

TEST_F(BackgroundIndexTest, ValidationReindexesAllMovedTranslationUnits) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  std::vector<std::string> Files;
  for (const char *Name : {"first.cc", "second.cc"}) {
    std::string File = testPath(Name);
    FS.Files[File] =
        (llvm::Twine("int ") + llvm::sys::path::stem(Name) + "();").str();
    tooling::CompileCommand Command;
    Command.Directory = testRoot();
    Command.Filename = File;
    Command.CommandLine = {"clang++", "-fsyntax-only", File};
    CDB.Commands[File] = {Command};
    Files.push_back(std::move(File));
  }
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue(Files);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  ASSERT_TRUE(bool(Idx.graphSnapshot({})));

  CDB.Commands[Files[0]][0].CommandLine.insert(
      CDB.Commands[Files[0]][0].CommandLine.begin() + 1, "-DFIRST_MOVED");
  CDB.Commands[Files[1]][0].CommandLine.insert(
      CDB.Commands[Files[1]][0].CommandLine.begin() + 1, "-DSECOND_MOVED");
  expectContentModified(Idx.graphSnapshot({}));
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  auto Reindexed = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Reindexed)) << llvm::toString(Reindexed.takeError());
  const auto *Upserts = Reindexed->getAsObject()->getArray("upserts");
  ASSERT_TRUE(Upserts);
  ASSERT_EQ(2u, Upserts->size());
  std::set<std::string> Flags;
  for (const auto &Upsert : *Upserts)
    for (const auto &Argument :
         *Upsert.getAsObject()->getObject("graph")->getArray("commandLine"))
      if (auto Text = Argument.getAsString(); Text && Text->starts_with("-D"))
        Flags.insert(Text->str());
  EXPECT_EQ((std::set<std::string>{"-DFIRST_MOVED", "-DSECOND_MOVED"}), Flags);
}

TEST_F(BackgroundIndexTest, EquivalentMainFileSpellingsShareOneShard) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  InferringCDB CDB;
  const std::string Native = testPath("same.cpp");
  const std::string Slashes = llvm::sys::path::convert_to_slash(Native);
  FS.Files[Native] = "int same();";
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = Native;
  Command.CommandLine = {"clang++", "-fsyntax-only", Native};
  CDB.Commands[Native] = {Command};
  {
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
    Idx.enqueue({Native});
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
  }
  auto Persisted = MSS.loadShard(Native);
  ASSERT_TRUE(Persisted);
  ASSERT_EQ(1u, Persisted->Graphs.size());
  Persisted->Graphs.front().MainFile = Slashes;
  Storage[Native] = llvm::to_string(IndexFileOut(*Persisted));
  FS.Files[Native] = "int changed();";

  {
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
    Idx.enqueue({Native});
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
    auto Snapshot = Idx.graphSnapshot({});
    ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
    ASSERT_TRUE(Snapshot->getAsObject()->getArray("manifest"));
    EXPECT_EQ(1u, Snapshot->getAsObject()->getArray("manifest")->size());
  }
}

TEST_F(BackgroundIndexTest, QueuedDiscoveryIsRetryable) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  const std::string A = testPath("A.cc");
  const std::string B = testPath("B.cc");
  FS.Files[A] = "int a();";
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = A;
  Command.CommandLine = {"clang++", "-fsyntax-only", A};
  CDB.setCompileCommand(A, Command);

  std::mutex GateMu;
  std::condition_variable GateCV;
  bool BlockDiscovery = false;
  bool DiscoveryEntered = false;
  bool ReleaseDiscovery = false;
  BackgroundIndex::Options Opts;
  Opts.ThreadPoolSize = 1;
  Opts.ContextProvider = [&](PathRef Path) {
    if (Path.empty()) {
      std::unique_lock<std::mutex> Lock(GateMu);
      if (BlockDiscovery) {
        DiscoveryEntered = true;
        GateCV.notify_all();
        GateCV.wait(Lock, [&] { return ReleaseDiscovery; });
      }
    }
    return Context::current().clone();
  };
  BackgroundIndex Idx(
      FS, CDB, [&](llvm::StringRef) { return &MSS; }, std::move(Opts));
  Idx.enqueue({A});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  ASSERT_TRUE(bool(Idx.graphSnapshot({})));

  FS.Files[B] = "int b();";
  Command.Filename = B;
  Command.CommandLine.back() = B;
  CDB.setCompileCommand(B, Command);
  {
    std::lock_guard<std::mutex> Lock(GateMu);
    BlockDiscovery = true;
  }
  Idx.enqueue({B});
  bool Entered = false;
  {
    std::unique_lock<std::mutex> Lock(GateMu);
    Entered = GateCV.wait_for(Lock, std::chrono::seconds(5),
                              [&] { return DiscoveryEntered; });
  }
  if (Entered)
    expectContentModified(Idx.graphSnapshot({}));
  {
    std::lock_guard<std::mutex> Lock(GateMu);
    ReleaseDiscovery = true;
  }
  GateCV.notify_all();
  ASSERT_TRUE(Entered);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Snapshot = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  ASSERT_TRUE(Snapshot->getAsObject()->getArray("manifest"));
  EXPECT_EQ(2u, Snapshot->getAsObject()->getArray("manifest")->size());
}

TEST_F(BackgroundIndexTest, CompleteMultiConfigurationGraphSnapshot) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  const std::string File = testPath("dual.h");
  FS.Files[File] =
      "#ifdef LEGACY_FIRST\nint legacyFirst();\n#else\nint legacySecond();\n"
      "#endif\n#ifdef __cplusplus\nclass Dual {};\n#else\nstruct Dual;\n"
      "#endif";

  tooling::CompileCommand C;
  C.Filename = File;
  C.Directory = testRoot();
  C.CommandLine = {"clang", "-xc", "-DLEGACY_FIRST", "-fsyntax-only", File};
  tooling::CompileCommand CXX = C;
  CXX.CommandLine = {"clang++", "-xc++", "-fsyntax-only", File};
  CDB.Commands[File] = {C, CXX};

  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({File});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  // Multi-configuration graph indexing must not change the representative
  // command that owns clangd's ordinary symbol slabs.
  EXPECT_EQ(1u, runFuzzyFind(Idx, "legacyFirst").size());
  EXPECT_EQ(0u, runFuzzyFind(Idx, "legacySecond").size());

  auto MainShard = MSS.loadShard(File);
  ASSERT_TRUE(MainShard);
  ASSERT_EQ(2u, MainShard->Graphs.size());
  EXPECT_THAT(MainShard->Graphs,
              UnorderedElementsAre(testing::Field(&GraphTU::Language, "c"),
                                   testing::Field(&GraphTU::Language, "cpp")));
  EXPECT_NE(MainShard->Graphs[0].CommandDigest,
            MainShard->Graphs[1].CommandDigest);

  auto First = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  auto *FirstObject = First->getAsObject();
  ASSERT_TRUE(FirstObject);
  ASSERT_TRUE(FirstObject->getArray("manifest"));
  EXPECT_EQ(2u, FirstObject->getArray("manifest")->size());
  ASSERT_TRUE(FirstObject->getArray("upserts"));
  EXPECT_EQ(2u, FirstObject->getArray("upserts")->size());
  for (const auto &Encoded : *FirstObject->getArray("upserts")) {
    const auto *Shard = Encoded.getAsObject();
    ASSERT_TRUE(Shard);
    const auto *Coverage = Shard->getArray("coverage");
    ASSERT_TRUE(Coverage);
    ASSERT_EQ(15u, Coverage->size());
    std::map<std::string, std::string> States;
    for (const auto &EncodedRow : *Coverage) {
      const auto *Row = EncodedRow.getAsObject();
      ASSERT_TRUE(Row);
      ASSERT_TRUE(Row->getString("family"));
      ASSERT_TRUE(Row->getString("state"));
      States[Row->getString("family")->str()] = Row->getString("state")->str();
    }
    EXPECT_EQ((std::map<std::string, std::string>{
                  {"accesses", "complete"},
                  {"calls", "partial"},
                  {"contains", "complete"},
                  {"decorates", "unsupported"},
                  {"dispatches", "partial"},
                  {"exports", "partial"},
                  {"extends", "complete"},
                  {"implements", "partial"},
                  {"imports", "complete"},
                  {"instantiates", "partial"},
                  {"overrides", "complete"},
                  {"references", "complete"},
                  {"renders", "unsupported"},
                  {"tests", "unsupported"},
                  {"type_ref", "complete"},
              }),
              States);
  }
  auto Generation = FirstObject->getString("generation");
  ASSERT_TRUE(Generation);

  GraphSnapshotParams Params;
  Params.KnownGeneration = Generation->str();
  auto Noop = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(Noop)) << llvm::toString(Noop.takeError());
  ASSERT_TRUE(Noop->getAsObject()->getArray("upserts"));
  EXPECT_TRUE(Noop->getAsObject()->getArray("upserts")->empty());
  EXPECT_EQ(Noop->getAsObject()->getString("baseGeneration"), Generation);

  CDB.Commands[File][1].CommandLine.push_back("-DCOMMAND_MOVED");
  expectContentModified(Idx.graphSnapshot({}));
}

TEST_F(BackgroundIndexTest, SharedDriverProducesOneToolchainCoordinate) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  std::vector<std::string> Files;
  for (const char *Name : {"first.cc", "second.cc"}) {
    const std::string File = testPath(Name);
    Files.push_back(File);
    FS.Files[File] =
        (llvm::Twine("int ") + llvm::sys::path::stem(Name) + "();").str();
    tooling::CompileCommand Command;
    Command.Directory = testRoot();
    Command.Filename = File;
    Command.CommandLine = {
        "clang++", "-fsyntax-only",
        (llvm::Twine("-DGRAPH_") + llvm::sys::path::stem(Name).upper()).str(),
        File};
    CDB.Commands[File] = {std::move(Command)};
  }

  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue(Files);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Snapshot = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  const auto *Universe = Snapshot->getAsObject()->getObject("universe");
  ASSERT_TRUE(Universe);
  ASSERT_TRUE(Universe->getArray("toolchains"));
  ASSERT_TRUE(Universe->getArray("configurations"));
  EXPECT_EQ(1u, Universe->getArray("toolchains")->size());
  EXPECT_EQ(2u, Universe->getArray("configurations")->size());
}

TEST_F(BackgroundIndexTest, GraphSnapshotRecapturesNativeDiskDigest) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;

  int FD = -1;
  llvm::SmallString<128> Source;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("clangd-graph-disk", "cc", FD,
                                                  Source));
  llvm::FileRemover Cleanup(Source);
  const std::string InitialDisk = "int native_version_one();";
  {
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS << InitialDisk;
  }
  const std::string Checker = "int checker_overlay();";
  FS.Files[Source] = Checker;
  tooling::CompileCommand Command;
  Command.Filename = Source.str().str();
  Command.Directory = llvm::sys::path::parent_path(Source).str();
  Command.CommandLine = {"clang++", "-fsyntax-only", Source.str().str()};
  CDB.Commands[Source] = {Command};

  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({Source.str().str()});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Initial = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Initial)) << llvm::toString(Initial.takeError());
  const auto *InitialObject = Initial->getAsObject();
  ASSERT_TRUE(InitialObject);
  const std::string InitialGeneration =
      InitialObject->getString("generation")->str();
  const auto *InitialGraph =
      (*InitialObject->getArray("upserts"))[0].getAsObject()->getObject(
          "graph");
  ASSERT_TRUE(InitialGraph);
  const auto *InitialSource =
      (*InitialGraph->getArray("sources"))[0].getAsObject();
  ASSERT_TRUE(InitialSource);
  EXPECT_EQ(InitialSource->getString("digest"), graphDigest(Checker));
  EXPECT_EQ(InitialSource->getString("diskDigest"), graphDigest(InitialDisk));

  const std::string MovedDisk = "int native_version_two();";
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(Source, EC, llvm::sys::fs::OF_None);
    ASSERT_FALSE(EC);
    OS << MovedDisk;
  }
  GraphSnapshotParams Params;
  Params.KnownGeneration = InitialGeneration;
  auto Moved = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(Moved)) << llvm::toString(Moved.takeError());
  const auto *MovedObject = Moved->getAsObject();
  ASSERT_TRUE(MovedObject);
  ASSERT_NE(MovedObject->getString("generation"), InitialGeneration);
  EXPECT_EQ(MovedObject->getString("baseGeneration"), InitialGeneration);
  const auto *MovedGraph =
      (*MovedObject->getArray("upserts"))[0].getAsObject()->getObject("graph");
  ASSERT_TRUE(MovedGraph);
  const auto *MovedSource = (*MovedGraph->getArray("sources"))[0].getAsObject();
  ASSERT_TRUE(MovedSource);
  EXPECT_EQ(MovedSource->getString("digest"), graphDigest(Checker));
  EXPECT_EQ(MovedSource->getString("diskDigest"), graphDigest(MovedDisk));

  Params.KnownGeneration = MovedObject->getString("generation")->str();
  auto Unchanged = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(Unchanged)) << llvm::toString(Unchanged.takeError());
  EXPECT_TRUE(Unchanged->getAsObject()->getArray("upserts")->empty());
}

TEST_F(BackgroundIndexTest, GeneratedHeaderCreationRetriesFailedGraph) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  const std::string File = testPath("generated.cc");
  const std::string Header = testPath("generated.h");
  FS.Files[File] = "#include \"generated.h\"\nint use() { return made(); }";
  tooling::CompileCommand Command;
  Command.Filename = File;
  Command.Directory = testRoot();
  Command.CommandLine = {"clang++", "-fsyntax-only", File};
  CDB.Commands[File] = {Command};

  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({File});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Failed = Idx.graphSnapshot({});
  ASSERT_FALSE(Failed);
  llvm::consumeError(Failed.takeError());

  FS.Files[Header] = "inline int made() { return 1; }";
  expectContentModified(Idx.graphSnapshot({}));
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Recovered = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Recovered)) << llvm::toString(Recovered.takeError());
  const auto *Upserts = Recovered->getAsObject()->getArray("upserts");
  ASSERT_TRUE(Upserts);
  ASSERT_EQ(1u, Upserts->size());
  const auto *Graph = (*Upserts)[0].getAsObject()->getObject("graph");
  ASSERT_TRUE(Graph);
  const auto *Includes = Graph->getArray("includes");
  ASSERT_TRUE(Includes);
  ASSERT_EQ(1u, Includes->size());
  EXPECT_EQ((*Includes)[0].getAsObject()->getString("target"),
            URI::create(Header).toString());
}

TEST_F(BackgroundIndexTest, GeneratedForcedIncludeRetriesFailedGraph) {
  struct Case {
    const char *Name;
    std::vector<std::string> Prefix;
  } Cases[] = {
      {"gnu-include",
       {"clang++", "-includegenerated-force.h", "-isystemgenerated"}},
      {"gnu-imacros",
       {"clang++", "-imacrosgenerated-force.h", "-iquotegenerated"}},
      {"cl-external",
       {"clang-cl", "--driver-mode=cl", "/FIgenerated-force.h",
        "/external:Igenerated"}},
      {"cl-imsvc",
       {"clang-cl", "--driver-mode=cl", "/FIgenerated-force.h",
        "/imsvcgenerated"}},
  };
  for (const auto &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    MockFS FS;
    llvm::StringMap<std::string> Storage;
    size_t CacheHits = 0;
    MemoryShardStorage MSS(Storage, CacheHits);
    MultiCommandCDB CDB;
    const std::string File = testPath((llvm::Twine(Test.Name) + ".cc").str());
    const std::string Header = testPath("generated/generated-force.h");
    FS.Files[File] = "int use() { return FORCED_VALUE; }";
    tooling::CompileCommand Command;
    Command.Filename = File;
    Command.Directory = testRoot();
    Command.CommandLine = Test.Prefix;
    Command.CommandLine.push_back("-fsyntax-only");
    Command.CommandLine.push_back(File);
    CDB.Commands[File] = {Command};

    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
    Idx.enqueue({File});
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
    auto Failed = Idx.graphSnapshot({});
    ASSERT_FALSE(Failed);
    llvm::consumeError(Failed.takeError());

    FS.Files[Header] = "#define FORCED_VALUE 7";
    expectContentModified(Idx.graphSnapshot({}));
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
    auto Recovered = Idx.graphSnapshot({});
    ASSERT_TRUE(bool(Recovered)) << llvm::toString(Recovered.takeError());
    EXPECT_EQ(1u, Recovered->getAsObject()->getArray("upserts")->size());
  }
}

TEST_F(BackgroundIndexTest, PersistedGraphRequiresCurrentProducerFingerprint) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  const std::string File = testPath("persisted.cc");
  FS.Files[File] = "int persisted();";
  tooling::CompileCommand Command;
  Command.Filename = File;
  Command.Directory = testRoot();
  Command.CommandLine = {"clang++", "-fsyntax-only", File};
  CDB.Commands[File] = {Command};

  {
    BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
    Idx.enqueue({File});
    ASSERT_TRUE(Idx.blockUntilIdleForTest());
    ASSERT_TRUE(bool(Idx.graphSnapshot({})));
  }
  auto Stored = Storage.find(File);
  ASSERT_NE(Stored, Storage.end());
  const std::string Fingerprint = graphProducerFingerprint();
  const size_t FingerprintAt = Stored->getValue().find(Fingerprint);
  ASSERT_NE(std::string::npos, FingerprintAt);
  Stored->getValue().replace(FingerprintAt, Fingerprint.size(),
                             Fingerprint.size(), '0');

  BackgroundIndex Reloaded(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Reloaded.enqueue({File});
  ASSERT_TRUE(Reloaded.blockUntilIdleForTest());
  auto Snapshot = Reloaded.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  const auto *Upserts = Snapshot->getAsObject()->getArray("upserts");
  ASSERT_TRUE(Upserts);
  ASSERT_EQ(1u, Upserts->size());
  const auto *Graph = (*Upserts)[0].getAsObject()->getObject("graph");
  ASSERT_TRUE(Graph);
  EXPECT_EQ(Graph->getString("producerFingerprint"), Fingerprint);
}

TEST_F(BackgroundIndexTest, GraphSnapshotPagesShardsAndMeasuresWork) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  std::vector<std::string> Files;
  for (const char *Name : {"one.cc", "two.cc", "three.cc"}) {
    std::string File = testPath(Name);
    FS.Files[File] =
        (llvm::Twine("int ") + llvm::sys::path::stem(Name) + "();").str();
    tooling::CompileCommand Command;
    Command.Filename = File;
    Command.Directory = testRoot();
    Command.CommandLine = {"clang++", "-fsyntax-only", File};
    CDB.Commands[File] = {Command};
    Files.push_back(std::move(File));
  }
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue(Files);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());

  GraphSnapshotParams Params;
  Params.MaxShards = 1;
  auto First = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
  const auto *FirstObject = First->getAsObject();
  ASSERT_TRUE(FirstObject);
  ASSERT_EQ(3u, FirstObject->getArray("manifest")->size());
  ASSERT_EQ(1u, FirstObject->getArray("upserts")->size());
  const auto Generation = FirstObject->getString("generation");
  const auto Sequence = FirstObject->getInteger("sequence");
  ASSERT_TRUE(Generation);
  ASSERT_TRUE(Sequence);
  const auto *FirstPhases = FirstObject->getObject("phases");
  ASSERT_TRUE(FirstPhases);
  EXPECT_GT(*FirstPhases->getInteger("validationMillis"), 0);
  EXPECT_GT(*FirstPhases->getInteger("semanticMillis"), 0);
  EXPECT_GT(*FirstPhases->getInteger("shardMillis"), 0);
  EXPECT_EQ(FirstPhases->getBoolean("cacheHit"), false);

  const auto *FirstPage = FirstObject->getObject("page");
  ASSERT_TRUE(FirstPage);
  ASSERT_EQ(3, *FirstPage->getInteger("total"));
  auto Cursor = FirstPage->getString("nextCursor");
  ASSERT_TRUE(Cursor);
  const std::string FirstCursor = Cursor->str();
  auto Overlapping = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(Overlapping)) << llvm::toString(Overlapping.takeError());
  const auto OverlappingCursor =
      Overlapping->getAsObject()->getObject("page")->getString("nextCursor");
  ASSERT_TRUE(OverlappingCursor);
  EXPECT_NE(*OverlappingCursor, FirstCursor);
  // Starting another paginated request must not invalidate the first plan.
  Params.Cursor = FirstCursor;
  auto FirstContinuation = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(FirstContinuation))
      << llvm::toString(FirstContinuation.takeError());
  EXPECT_EQ(FirstContinuation->getAsObject()->getInteger("sequence"), Sequence);
  Cursor = FirstContinuation->getAsObject()->getObject("page")->getString(
      "nextCursor");
  size_t Seen = 2;
  while (Cursor) {
    Params.Cursor = Cursor->str();
    auto Next = Idx.graphSnapshot(Params);
    ASSERT_TRUE(bool(Next)) << llvm::toString(Next.takeError());
    const auto *Object = Next->getAsObject();
    ASSERT_TRUE(Object);
    EXPECT_EQ(Object->getString("generation"), Generation);
    EXPECT_EQ(Object->getInteger("sequence"), Sequence);
    EXPECT_TRUE(Object->getArray("manifest")->empty());
    ASSERT_EQ(1u, Object->getArray("upserts")->size());
    const auto *Page = Object->getObject("page");
    ASSERT_TRUE(Page);
    EXPECT_EQ(static_cast<int64_t>(Seen), *Page->getInteger("offset"));
    const auto *Phases = Object->getObject("phases");
    ASSERT_TRUE(Phases);
    EXPECT_GT(*Phases->getInteger("validationMillis"), 0);
    EXPECT_EQ(0, *Phases->getInteger("semanticMillis"));
    EXPECT_EQ(0, *Phases->getInteger("shardMillis"));
    ++Seen;
    Cursor = Page->getString("nextCursor");
  }
  EXPECT_EQ(3u, Seen);

  GraphSnapshotParams NoopParams;
  NoopParams.KnownGeneration = Generation->str();
  NoopParams.MaxShards = 1;
  auto Noop = Idx.graphSnapshot(NoopParams);
  ASSERT_TRUE(bool(Noop)) << llvm::toString(Noop.takeError());
  const auto *NoopObject = Noop->getAsObject();
  ASSERT_TRUE(NoopObject->getArray("manifest")->empty());
  ASSERT_TRUE(NoopObject->getArray("upserts")->empty());
  const auto *NoopPhases = NoopObject->getObject("phases");
  ASSERT_TRUE(NoopPhases);
  EXPECT_EQ(NoopPhases->getBoolean("cacheHit"), true);
  EXPECT_EQ(0, *NoopPhases->getInteger("semanticMillis"));
  EXPECT_EQ(0, *NoopPhases->getInteger("shardMillis"));
  EXPECT_GT(*NoopPhases->getInteger("validationMillis"), 0);
  EXPECT_GT(*NoopPhases->getInteger("encodeMillis"), 0);
  EXPECT_EQ(*NoopPhases->getInteger("totalMillis"),
            *NoopPhases->getInteger("validationMillis") +
                *NoopPhases->getInteger("semanticMillis") +
                *NoopPhases->getInteger("shardMillis") +
                *NoopPhases->getInteger("encodeMillis"));
}

TEST_F(BackgroundIndexTest, MixedClangLanguagesPublishOnlyCAndCXX) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  struct Fixture {
    const char *Name;
    const char *Language;
    const char *Source;
  } Fixtures[] = {{"plain.c", "c", "int plain_c(void);"},
                  {"plain.cpp", "c++", "int plain_cpp();"},
                  {"objective.m", "objective-c", "@interface Obj @end"},
                  {"objective.mm", "objective-c++", "@interface ObjCpp @end"},
                  {"device.cu", "cuda", "int device_value;"}};
  std::vector<std::string> Files;
  for (const auto &Fixture : Fixtures) {
    std::string File = testPath(Fixture.Name);
    FS.Files[File] = Fixture.Source;
    tooling::CompileCommand Command;
    Command.Filename = File;
    Command.Directory = testRoot();
    Command.CommandLine = {"clang", "-x", Fixture.Language, "-fsyntax-only",
                           File};
    if (llvm::StringRef(Fixture.Language) == "cuda") {
      Command.CommandLine.insert(
          Command.CommandLine.end() - 1,
          {"--cuda-host-only", "-nocudainc", "-nocudalib"});
    }
    CDB.Commands[File] = {Command};
    Files.push_back(std::move(File));
  }
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue(Files);
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Snapshot = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  const auto *Upserts = Snapshot->getAsObject()->getArray("upserts");
  ASSERT_TRUE(Upserts);
  ASSERT_EQ(2u, Upserts->size());
  std::set<std::string> Languages;
  for (const auto &Shard : *Upserts)
    Languages.insert(
        Shard.getAsObject()->getObject("graph")->getString("language")->str());
  EXPECT_EQ((std::set<std::string>{"c", "cpp"}), Languages);
}

TEST_F(BackgroundIndexTest, HeaderViewsStayOwnedByTheirTranslationUnits) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  const std::string Header = testPath("shared.h");
  const std::string CFile = testPath("owner.c");
  const std::string CXXFile = testPath("owner.cpp");
  FS.Files[Header] =
      "#ifdef __cplusplus\nclass Shared {};\n#else\nstruct Shared;\n#endif";
  FS.Files[CFile] = "#include \"shared.h\"\nstruct Shared *c_owner;";
  FS.Files[CXXFile] = "#include \"shared.h\"\nShared cpp_owner;";
  for (const auto &Entry :
       {std::pair<std::string, std::string>{CFile, "c"},
        std::pair<std::string, std::string>{CXXFile, "c++"}}) {
    tooling::CompileCommand Command;
    Command.Filename = Entry.first;
    Command.Directory = testRoot();
    Command.CommandLine = {"clang", "-x", Entry.second, "-fsyntax-only",
                           Entry.first};
    CDB.Commands[Entry.first] = {Command};
  }
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({CFile, CXXFile});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Snapshot = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Snapshot)) << llvm::toString(Snapshot.takeError());
  const auto *Upserts = Snapshot->getAsObject()->getArray("upserts");
  ASSERT_TRUE(Upserts);
  ASSERT_EQ(2u, Upserts->size());
  std::set<std::pair<std::string, std::string>> Views;
  std::set<std::string> Configurations;
  std::set<std::string> ShardKeys;
  for (const auto &Shard : *Upserts) {
    const auto *ShardObject = Shard.getAsObject();
    ASSERT_TRUE(ShardObject);
    const auto *Graph = ShardObject->getObject("graph");
    ASSERT_TRUE(Graph);
    const auto Language = Graph->getString("language");
    const auto Configuration = ShardObject->getString("configuration");
    const auto Key = ShardObject->getString("key");
    ASSERT_TRUE(Language);
    ASSERT_TRUE(Configuration);
    ASSERT_TRUE(Key);
    EXPECT_EQ(Graph->getString("commandDigest"), Configuration);
    EXPECT_TRUE(Key->ends_with((llvm::Twine("#") + *Configuration).str()));
    Configurations.insert(Configuration->str());
    ShardKeys.insert(Key->str());

    bool HasSharedHeader = false;
    for (const auto &Source : *Graph->getArray("sources"))
      HasSharedHeader |= Source.getAsObject()->getString("uri") ==
                         URI::create(Header).toString();
    EXPECT_TRUE(HasSharedHeader);

    size_t SharedSymbols = 0;
    bool SharedIsDefined = false;
    std::set<std::string> Names;
    for (const auto &EncodedSymbol : *Graph->getArray("symbols")) {
      const auto *Symbol = EncodedSymbol.getAsObject();
      ASSERT_TRUE(Symbol);
      const auto Name = Symbol->getString("name");
      const auto Kind = Symbol->getInteger("kind");
      ASSERT_TRUE(Name);
      ASSERT_TRUE(Kind);
      Names.insert(Name->str());
      if (*Name == "Shared" && *Kind != 23) {
        ++SharedSymbols;
        EXPECT_EQ(Symbol->getObject("declaration")->getString("file"),
                  URI::create(Header).toString());
        SharedIsDefined =
            !Symbol->getObject("definition")->getString("file")->empty();
      }
    }
    EXPECT_EQ(1u, SharedSymbols);
    if (*Language == "c") {
      EXPECT_FALSE(SharedIsDefined);
      EXPECT_EQ(1u, Names.count("c_owner"));
      EXPECT_EQ(0u, Names.count("cpp_owner"));
    } else {
      EXPECT_EQ("cpp", *Language);
      EXPECT_TRUE(SharedIsDefined);
      EXPECT_EQ(1u, Names.count("cpp_owner"));
      EXPECT_EQ(0u, Names.count("c_owner"));
    }
    Views.emplace(Graph->getString("mainFile")->str(), Language->str());
  }
  EXPECT_EQ((std::set<std::pair<std::string, std::string>>{{CFile, "c"},
                                                           {CXXFile, "cpp"}}),
            Views);
  EXPECT_EQ(2u, Configurations.size());
  EXPECT_EQ(2u, ShardKeys.size());
}

TEST_F(BackgroundIndexTest, GraphSnapshotDeletesAreCanonical) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  MultiCommandCDB CDB;
  const std::string Removed = testPath("removed.cc");
  const std::string Retained = testPath("retained.cc");
  FS.Files[Removed] = "int removed();";
  FS.Files[Retained] = "int retained();";

  tooling::CompileCommand First;
  First.Filename = Removed;
  First.Directory = testRoot();
  First.CommandLine = {"clang++", "-DFIRST", "-fsyntax-only", Removed};
  tooling::CompileCommand Second = First;
  Second.CommandLine = {"clang++", "-DSECOND", "-fsyntax-only", Removed};
  tooling::CompileCommand Keep = First;
  Keep.Filename = Retained;
  Keep.CommandLine = {"clang++", "-fsyntax-only", Retained};
  CDB.Commands[Removed] = {First, Second};
  CDB.Commands[Retained] = {Keep};

  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({Removed, Retained});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  auto Initial = Idx.graphSnapshot({});
  ASSERT_TRUE(bool(Initial)) << llvm::toString(Initial.takeError());
  auto Generation = Initial->getAsObject()->getString("generation");
  ASSERT_TRUE(Generation);

  CDB.Commands.erase(Removed);
  Idx.enqueue({Removed});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  GraphSnapshotParams Params;
  Params.KnownGeneration = Generation->str();
  auto Delta = Idx.graphSnapshot(Params);
  ASSERT_TRUE(bool(Delta)) << llvm::toString(Delta.takeError());
  const auto *Deletes = Delta->getAsObject()->getArray("deletes");
  ASSERT_TRUE(Deletes);
  ASSERT_EQ(2u, Deletes->size());
  ASSERT_TRUE((*Deletes)[0].getAsString());
  ASSERT_TRUE((*Deletes)[1].getAsString());
  EXPECT_LT(*(*Deletes)[0].getAsString(), *(*Deletes)[1].getAsString());
}

TEST_F(BackgroundIndexTest, FailedRefreshPreservesPublishedGraph) {
  MockFS FS;
  llvm::StringMap<std::string> Storage;
  size_t CacheHits = 0;
  MemoryShardStorage MSS(Storage, CacheHits);
  OverlayCDB CDB(/*Base=*/nullptr);
  const std::string File = testPath("A.cc");
  FS.Files[File] = "int stable();";
  tooling::CompileCommand Cmd;
  Cmd.Filename = File;
  Cmd.Directory = testRoot();
  Cmd.CommandLine = {"clang++", "-fsyntax-only", File};
  CDB.setCompileCommand(File, Cmd);
  BackgroundIndex Idx(FS, CDB, [&](llvm::StringRef) { return &MSS; }, {});
  Idx.enqueue({File});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  std::string Published = Storage.lookup(File);
  ASSERT_FALSE(Published.empty());
  auto StableShard = MSS.loadShard(File);
  ASSERT_TRUE(StableShard);
  ASSERT_EQ(1u, StableShard->Graphs.size());
  const std::string StableCommand = StableShard->Graphs.front().CommandDigest;

  FS.Files[File] = "int moved();";
  expectContentModified(Idx.graphSnapshot({}));

  FS.Files[File] = "int broken(";
  Idx.enqueueGraphDependents(std::vector<std::string>{File});
  ASSERT_TRUE(Idx.blockUntilIdleForTest());
  // The ordinary erroneous shard is still updated as clangd historically did,
  // but its graph lane retains the last complete generation.
  EXPECT_NE(Published, Storage.lookup(File));
  EXPECT_EQ(0u, runFuzzyFind(Idx, "stable").size());
  auto BrokenShard = MSS.loadShard(File);
  ASSERT_TRUE(BrokenShard);
  ASSERT_EQ(1u, BrokenShard->Graphs.size());
  EXPECT_EQ(StableCommand, BrokenShard->Graphs.front().CommandDigest);
  EXPECT_FALSE(BrokenShard->Graphs.front().HadErrors);
  EXPECT_TRUE(llvm::any_of(
      BrokenShard->Graphs.front().Symbols,
      [](const GraphSymbol &Symbol) { return Symbol.Name == "stable"; }));
  auto Rejected = Idx.graphSnapshot({});
  EXPECT_FALSE(bool(Rejected));
  if (!Rejected) {
    bool WasLSPError = false;
    llvm::handleAllErrors(
        Rejected.takeError(), [&](const LSPError &) { WasLSPError = true; },
        [](const llvm::ErrorInfoBase &) {});
    EXPECT_FALSE(WasLSPError);
  }
}

class BackgroundIndexRebuilderTest : public testing::Test {
protected:
  BackgroundIndexRebuilderTest()
      : Source(IndexContents::All, /*SupportContainedRefs=*/true),
        Target(std::make_unique<MemIndex>()),
        Rebuilder(&Target, &Source, /*Threads=*/10) {
    // Prepare FileSymbols with TestSymbol in it, for checkRebuild.
    TestSymbol.ID = SymbolID("foo");
  }

  // Perform Action and determine whether it rebuilt the index or not.
  bool checkRebuild(std::function<void()> Action) {
    // Update name so we can tell if the index updates.
    VersionStorage.push_back("Sym" + std::to_string(++VersionCounter));
    TestSymbol.Name = VersionStorage.back();
    SymbolSlab::Builder SB;
    SB.insert(TestSymbol);
    Source.update("", std::make_unique<SymbolSlab>(std::move(SB).build()),
                  nullptr, nullptr, false);
    // Now maybe update the index.
    Action();
    // Now query the index to get the name count.
    std::string ReadName;
    LookupRequest Req;
    Req.IDs.insert(TestSymbol.ID);
    Target.lookup(Req,
                  [&](const Symbol &S) { ReadName = std::string(S.Name); });
    // The index was rebuild if the name is up to date.
    return ReadName == VersionStorage.back();
  }

  Symbol TestSymbol;
  FileSymbols Source;
  SwapIndex Target;
  BackgroundIndexRebuilder Rebuilder;

  unsigned VersionCounter = 0;
  std::deque<std::string> VersionStorage;
};

TEST_F(BackgroundIndexRebuilderTest, IndexingTUs) {
  for (unsigned I = 0; I < Rebuilder.TUsBeforeFirstBuild - 1; ++I)
    EXPECT_FALSE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  for (unsigned I = 0; I < Rebuilder.TUsBeforeRebuild - 1; ++I)
    EXPECT_FALSE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.indexedTU(); }));
}

TEST_F(BackgroundIndexRebuilderTest, LoadingShards) {
  Rebuilder.startLoading();
  Rebuilder.loadedShard(10);
  Rebuilder.loadedShard(20);
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.doneLoading(); }));

  // No rebuild for no shards.
  Rebuilder.startLoading();
  EXPECT_FALSE(checkRebuild([&] { Rebuilder.doneLoading(); }));

  // Loads can overlap.
  Rebuilder.startLoading();
  Rebuilder.loadedShard(1);
  Rebuilder.startLoading();
  Rebuilder.loadedShard(1);
  EXPECT_FALSE(checkRebuild([&] { Rebuilder.doneLoading(); }));
  Rebuilder.loadedShard(1);
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.doneLoading(); }));

  // No rebuilding for indexed files while loading.
  Rebuilder.startLoading();
  for (unsigned I = 0; I < 3 * Rebuilder.TUsBeforeRebuild; ++I)
    EXPECT_FALSE(checkRebuild([&] { Rebuilder.indexedTU(); }));
  // But they get indexed when we're done, even if no shards were loaded.
  EXPECT_TRUE(checkRebuild([&] { Rebuilder.doneLoading(); }));
}

TEST(BackgroundQueueTest, Priority) {
  // Create high and low priority tasks.
  // Once a bunch of high priority tasks have run, the queue is stopped.
  // So the low priority tasks should never run.
  BackgroundQueue Q;
  std::atomic<unsigned> HiRan(0), LoRan(0);
  BackgroundQueue::Task Lo([&] { ++LoRan; });
  BackgroundQueue::Task Hi([&] {
    if (++HiRan >= 10)
      Q.stop();
  });
  Hi.QueuePri = 100;

  // Enqueuing the low-priority ones first shouldn't make them run first.
  Q.append(std::vector<BackgroundQueue::Task>(30, Lo));
  for (unsigned I = 0; I < 30; ++I)
    Q.push(Hi);

  AsyncTaskRunner ThreadPool;
  for (unsigned I = 0; I < 5; ++I)
    ThreadPool.runAsync("worker", [&] { Q.work(); });
  // We should test enqueue with active workers, but it's hard to avoid races.
  // Just make sure we don't crash.
  Q.push(Lo);
  Q.append(std::vector<BackgroundQueue::Task>(2, Hi));

  // After finishing, check the tasks that ran.
  ThreadPool.wait();
  EXPECT_GE(HiRan, 10u);
  EXPECT_EQ(LoRan, 0u);
}

TEST(BackgroundQueueTest, Boost) {
  std::string Sequence;

  BackgroundQueue::Task A([&] { Sequence.push_back('A'); });
  A.Tag = "A";
  A.QueuePri = 1;

  BackgroundQueue::Task B([&] { Sequence.push_back('B'); });
  B.QueuePri = 2;
  B.Tag = "B";

  {
    BackgroundQueue Q;
    Q.append({A, B});
    Q.work([&] { Q.stop(); });
    EXPECT_EQ("BA", Sequence) << "priority order";
  }
  Sequence.clear();
  {
    BackgroundQueue Q;
    Q.boost("A", 3);
    Q.append({A, B});
    Q.work([&] { Q.stop(); });
    EXPECT_EQ("AB", Sequence) << "A was boosted before enqueueing";
  }
  Sequence.clear();
  {
    BackgroundQueue Q;
    Q.append({A, B});
    Q.boost("A", 3);
    Q.work([&] { Q.stop(); });
    EXPECT_EQ("AB", Sequence) << "A was boosted after enqueueing";
  }
}

TEST(BackgroundQueueTest, Duplicates) {
  std::string Sequence;
  BackgroundQueue::Task A([&] { Sequence.push_back('A'); });
  A.QueuePri = 100;
  A.Key = 1;
  BackgroundQueue::Task B([&] { Sequence.push_back('B'); });
  // B has no key, and is not subject to duplicate detection.
  B.QueuePri = 50;

  BackgroundQueue Q;
  Q.append({A, B, A, B}); // One A is dropped, the other is high priority.
  Q.work(/*OnIdle=*/[&] {
    // The first time we go idle, we enqueue the same task again.
    if (!llvm::is_contained(Sequence, ' ')) {
      Sequence.push_back(' ');
      Q.append({A, B, A, B}); // Both As are dropped.
    } else {
      Q.stop();
    }
  });

  // This could reasonably be "ABB BBA", if we had good *re*indexing support.
  EXPECT_EQ("ABB BB", Sequence);
}

TEST(BackgroundQueueTest, RepeatWhileActive) {
  BackgroundQueue Q;
  std::string Sequence;
  BackgroundQueue::Task First([&] {
    Sequence.push_back('A');
    BackgroundQueue::Task Again([&] { Sequence.push_back('A'); });
    Again.Key = 1;
    Again.Repeatable = true;
    Q.push(std::move(Again));
  });
  First.Key = 1;
  First.Repeatable = true;
  Q.push(std::move(First));
  Q.work([&] {
    if (Sequence.size() == 2)
      Q.stop();
  });
  EXPECT_EQ("AA", Sequence);
}

TEST(BackgroundQueueTest, Progress) {
  using testing::AnyOf;
  BackgroundQueue::Stats S;
  BackgroundQueue Q([&](BackgroundQueue::Stats New) {
    // Verify values are sane.
    // Items are enqueued one at a time (at least in this test).
    EXPECT_THAT(New.Enqueued, AnyOf(S.Enqueued, S.Enqueued + 1));
    // Items are completed one at a time.
    EXPECT_THAT(New.Completed, AnyOf(S.Completed, S.Completed + 1));
    // Items are started or completed one at a time.
    EXPECT_THAT(New.Active, AnyOf(S.Active - 1, S.Active, S.Active + 1));
    // Idle point only advances in time.
    EXPECT_GE(New.LastIdle, S.LastIdle);
    // Idle point is a task that has been completed in the past.
    EXPECT_LE(New.LastIdle, New.Completed);
    // LastIdle is now only if we're really idle.
    EXPECT_EQ(New.LastIdle == New.Enqueued,
              New.Completed == New.Enqueued && New.Active == 0u);
    S = New;
  });

  // Two types of tasks: a ping task enqueues a pong task.
  // This avoids all enqueues followed by all completions (boring!)
  std::atomic<int> PingCount(0), PongCount(0);
  BackgroundQueue::Task Pong([&] { ++PongCount; });
  BackgroundQueue::Task Ping([&] {
    ++PingCount;
    Q.push(Pong);
  });

  for (int I = 0; I < 1000; ++I)
    Q.push(Ping);
  // Spin up some workers and stop while idle.
  AsyncTaskRunner ThreadPool;
  for (unsigned I = 0; I < 5; ++I)
    ThreadPool.runAsync("worker", [&] { Q.work([&] { Q.stop(); }); });
  ThreadPool.wait();

  // Everything's done, check final stats.
  // Assertions above ensure we got from 0 to 2000 in a reasonable way.
  EXPECT_EQ(PingCount.load(), 1000);
  EXPECT_EQ(PongCount.load(), 1000);
  EXPECT_EQ(S.Active, 0u);
  EXPECT_EQ(S.Enqueued, 2000u);
  EXPECT_EQ(S.Completed, 2000u);
  EXPECT_EQ(S.LastIdle, 2000u);
}

TEST(BackgroundIndex, Profile) {
  MockFS FS;
  MockCompilationDatabase CDB;
  BackgroundIndex Idx(FS, CDB, [](llvm::StringRef) { return nullptr; },
                      /*Opts=*/{});

  llvm::BumpPtrAllocator Alloc;
  MemoryTree MT(&Alloc);
  Idx.profile(MT);
  ASSERT_THAT(MT.children(),
              UnorderedElementsAre(Pair("slabs", _), Pair("index", _)));
}

} // namespace clangd
} // namespace clang
