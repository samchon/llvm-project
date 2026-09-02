//===------ IndexActionTests.cpp  -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Headers.h"
#include "SourceCode.h"
#include "TestFS.h"
#include "URI.h"
#include "index/IndexAction.h"
#include "index/Serialization.h"
#include "support/Context.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/raw_ostream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>

namespace clang {
namespace clangd {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::EndsWith;
using ::testing::Not;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedPointwise;

std::string toUri(llvm::StringRef Path) { return URI::create(Path).toString(); }

MATCHER(isTU, "") { return arg.Flags & IncludeGraphNode::SourceFlag::IsTU; }

MATCHER_P(hasDigest, Digest, "") { return arg.Digest == Digest; }

MATCHER_P(hasName, Name, "") { return arg.Name == Name; }

MATCHER(hasSameURI, "") {
  llvm::StringRef URI = ::testing::get<0>(arg);
  const std::string &Path = ::testing::get<1>(arg);
  return toUri(Path) == URI;
}

MATCHER_P(includeHeader, P, "") {
  return (arg.IncludeHeaders.size() == 1) &&
         (arg.IncludeHeaders.begin()->IncludeHeader == P);
}

::testing::Matcher<const IncludeGraphNode &>
includesAre(const std::vector<std::string> &Includes) {
  return ::testing::Field(&IncludeGraphNode::DirectIncludes,
                          UnorderedPointwise(hasSameURI(), Includes));
}

void checkNodesAreInitialized(const IndexFileIn &IndexFile,
                              const std::vector<std::string> &Paths) {
  ASSERT_TRUE(IndexFile.Sources);
  EXPECT_THAT(Paths.size(), IndexFile.Sources->size());
  for (llvm::StringRef Path : Paths) {
    auto URI = toUri(Path);
    const auto &Node = IndexFile.Sources->lookup(URI);
    // Uninitialized nodes will have an empty URI.
    EXPECT_EQ(Node.URI.data(), IndexFile.Sources->find(URI)->getKeyData());
  }
}

std::map<std::string, const IncludeGraphNode &> toMap(const IncludeGraph &IG) {
  std::map<std::string, const IncludeGraphNode &> Nodes;
  for (auto &I : IG)
    Nodes.emplace(std::string(I.getKey()), I.getValue());
  return Nodes;
}

class IndexActionTest : public ::testing::Test {
public:
  IndexActionTest() : InMemoryFileSystem(new llvm::vfs::InMemoryFileSystem) {}

  IndexFileIn
  runIndexingAction(llvm::StringRef MainFilePath,
                    const std::vector<std::string> &ExtraArgs = {},
                    bool OverlayRealFileSystem = false) {
    IndexFileIn IndexFile;
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS = InMemoryFileSystem;
    if (OverlayRealFileSystem) {
      auto Overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
          llvm::vfs::getRealFileSystem());
      Overlay->pushOverlay(InMemoryFileSystem);
      FS = std::move(Overlay);
    }
    llvm::IntrusiveRefCntPtr<FileManager> Files(
        new FileManager(FileSystemOptions(), FS));

    auto Action = createStaticIndexingAction(
        Opts, [&](SymbolSlab S) { IndexFile.Symbols = std::move(S); },
        [&](RefSlab R) { IndexFile.Refs = std::move(R); },
        [&](RelationSlab R) { IndexFile.Relations = std::move(R); },
        [&](IncludeGraph IG) { IndexFile.Sources = std::move(IG); },
        [&](GraphTU Graph) { IndexFile.Graphs.push_back(std::move(Graph)); });

    std::vector<std::string> Args = {"index_action", "-fsyntax-only",
                                     "-xc++",        "-std=c++11",
                                     "-iquote",      testRoot()};
    Args.insert(Args.end(), ExtraArgs.begin(), ExtraArgs.end());
    Args.push_back(std::string(MainFilePath));

    tooling::ToolInvocation Invocation(
        Args, std::move(Action), Files.get(),
        std::make_shared<PCHContainerOperations>());

    Invocation.run();

    checkNodesAreInitialized(IndexFile, FilePaths);
    return IndexFile;
  }

  void addFile(llvm::StringRef Path, llvm::StringRef Content) {
    InMemoryFileSystem->addFile(Path, 0,
                                llvm::MemoryBuffer::getMemBufferCopy(Content));
    FilePaths.push_back(std::string(Path));
  }

protected:
  SymbolCollector::Options Opts;
  std::vector<std::string> FilePaths;
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemoryFileSystem;
};

TEST_F(IndexActionTest, CollectIncludeGraph) {
  std::string MainFilePath = testPath("main.cpp");
  std::string MainCode = "#include \"level1.h\"";
  std::string Level1HeaderPath = testPath("level1.h");
  std::string Level1HeaderCode = "#include \"level2.h\"";
  std::string Level2HeaderPath = testPath("level2.h");
  std::string Level2HeaderCode = "";

  addFile(MainFilePath, MainCode);
  addFile(Level1HeaderPath, Level1HeaderCode);
  addFile(Level2HeaderPath, Level2HeaderCode);

  IndexFileIn IndexFile = runIndexingAction(MainFilePath);
  auto Nodes = toMap(*IndexFile.Sources);

  EXPECT_THAT(Nodes,
              UnorderedElementsAre(
                  Pair(toUri(MainFilePath),
                       AllOf(isTU(), includesAre({Level1HeaderPath}),
                             hasDigest(digest(MainCode)))),
                  Pair(toUri(Level1HeaderPath),
                       AllOf(Not(isTU()), includesAre({Level2HeaderPath}),
                             hasDigest(digest(Level1HeaderCode)))),
                  Pair(toUri(Level2HeaderPath),
                       AllOf(Not(isTU()), includesAre({}),
                             hasDigest(digest(Level2HeaderCode))))));
}

TEST_F(IndexActionTest, CollectCompleteGraphFacts) {
  Opts.CollectGraph = true;
  const std::string Header = testPath("graph.h");
  const std::string Main = testPath("graph.cpp");
  addFile(Header, R"cpp(
#define GRAPH_WRAP(x) x
struct Base { virtual void run(); };
struct Derived : Base {
  int field;
  explicit Derived(int value) : field(value) {}
  void run() override;
};
)cpp");
  addFile(Main, R"cpp(
#include "graph.h"
static int hidden;
void Derived::run() {
  int local = GRAPH_WRAP(hidden);
  field = local;
}
[[nodiscard]] Derived make() { return Derived{1}; }
void dispatch(Base &value) { value.run(); }
)cpp");

  IndexFileIn Indexed = runIndexingAction(Main, {"-std=c++17"});
  ASSERT_EQ(1u, Indexed.Graphs.size());
  const GraphTU &Graph = Indexed.Graphs.front();
  EXPECT_EQ("cpp", Graph.Language);
  EXPECT_EQ(toUri(Main), Graph.MainFileURI);
  EXPECT_EQ(2u, Graph.Sources.size());
  EXPECT_THAT(
      Graph.Includes,
      Contains(AllOf(testing::Field(&GraphInclude::SourceURI, toUri(Main)),
                     testing::Field(&GraphInclude::TargetURI, toUri(Header)))));
  size_t MacroOccurrences = 0;
  std::string MacroID;
  for (const auto &Macro : Graph.Macros) {
    if (Macro.Name != "GRAPH_WRAP")
      continue;
    ++MacroOccurrences;
    if (MacroID.empty())
      MacroID = Macro.ID;
    else
      EXPECT_EQ(MacroID, Macro.ID);
  }
  EXPECT_GE(MacroOccurrences, 2u);
  EXPECT_TRUE(llvm::all_of(Graph.Macros, [](const GraphMacro &Macro) {
    return Macro.Definition.valid();
  }));
  EXPECT_TRUE(llvm::any_of(Graph.Symbols, [](const GraphSymbol &Symbol) {
    return Symbol.Name == "hidden" && Symbol.Internal;
  }));
  EXPECT_TRUE(llvm::any_of(Graph.Symbols, [](const GraphSymbol &Symbol) {
    return Symbol.Name == "local" && Symbol.Local;
  }));
  EXPECT_TRUE(llvm::any_of(Graph.Relations, [](const GraphRelation &Relation) {
    return Relation.Roles &
           static_cast<uint32_t>(index::SymbolRole::RelationBaseOf);
  }));
  EXPECT_TRUE(llvm::any_of(Graph.Relations, [](const GraphRelation &Relation) {
    return Relation.Roles &
           static_cast<uint32_t>(index::SymbolRole::RelationOverrideOf);
  }));
  EXPECT_TRUE(
      llvm::any_of(Graph.Occurrences, [](const GraphOccurrence &Occurrence) {
        return Occurrence.Roles &
               static_cast<uint32_t>(index::SymbolRole::Read);
      }));
  EXPECT_TRUE(llvm::any_of(Graph.Symbols, [](const GraphSymbol &Symbol) {
    return Symbol.Name == "field" &&
           Symbol.Kind == static_cast<uint32_t>(index::SymbolKind::Field);
  }));
  EXPECT_TRUE(llvm::any_of(Graph.Symbols, [](const GraphSymbol &Symbol) {
    return Symbol.Name == "make" &&
           llvm::any_of(Symbol.Attributes, [](const GraphAttribute &Attribute) {
             return Attribute.Name == "nodiscard";
           });
  }));
  EXPECT_TRUE(
      llvm::any_of(Graph.Occurrences, [](const GraphOccurrence &Occurrence) {
        return Occurrence.Roles &
               static_cast<uint32_t>(index::SymbolRole::Write);
      }));
  EXPECT_TRUE(
      llvm::any_of(Graph.Occurrences, [](const GraphOccurrence &Occurrence) {
        return (Occurrence.Roles &
                static_cast<uint32_t>(index::SymbolRole::Call)) &&
               Occurrence.TargetKind ==
                   static_cast<uint32_t>(index::SymbolKind::Constructor);
      }));
  EXPECT_TRUE(
      llvm::any_of(Graph.Occurrences, [](const GraphOccurrence &Occurrence) {
        return Occurrence.Roles &
               static_cast<uint32_t>(index::SymbolRole::Dynamic);
      }));
  EXPECT_TRUE(llvm::all_of(Graph.Symbols, [](const GraphSymbol &Symbol) {
    return !Symbol.Declaration.valid() ||
           Symbol.Declaration.EndColumn >= Symbol.Declaration.StartColumn;
  }));
}

TEST_F(IndexActionTest, CollectGraphModuleFacts) {
  Opts.CollectGraph = true;
  const std::string Header = testPath("module.h");
  const std::string ModuleMap = testPath("module.modulemap");
  const std::string Main = testPath("module-user.cpp");
  addFile(Header, "inline int modular() { return 7; }");
  addFile(Main, "#include \"module.h\"\nint use() { return modular(); }");
  ASSERT_TRUE(InMemoryFileSystem->addFile(
      ModuleMap, 0,
      llvm::MemoryBuffer::getMemBufferCopy(
          "module GraphFixture { header \"module.h\" export * }")));

  llvm::SmallString<128> ModuleCache;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("clangd-graph-modules", ModuleCache));
  auto RemoveModuleCache = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(ModuleCache); });
  IndexFileIn Indexed = runIndexingAction(
      Main,
      {"-fmodules", "-fimplicit-modules",
       "-fmodule-map-file=" + ModuleMap,
       "-fmodules-cache-path=" + ModuleCache.str().str()},
      /*OverlayRealFileSystem=*/true);

  ASSERT_EQ(1u, Indexed.Graphs.size());
  const GraphTU &Graph = Indexed.Graphs.front();
  EXPECT_TRUE(llvm::any_of(Graph.Modules, [](const GraphModule &Module) {
    return Module.Name == "GraphFixture" &&
           (Module.Roles &
            static_cast<uint32_t>(index::SymbolRole::Reference)) &&
           Module.Evidence.valid();
  }));
  EXPECT_TRUE(llvm::any_of(Graph.Includes, [&](const GraphInclude &Include) {
    return Include.TargetURI == toUri(Header) && Include.ModuleImported &&
           Include.Evidence.valid();
  }));
}

TEST_F(IndexActionTest, GraphDistinguishesCheckerAndDiskDigests) {
  Opts.CollectGraph = true;
  int FD = -1;
  llvm::SmallString<128> Main;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("clangd-graph-source", "cpp",
                                                  FD, Main));
  llvm::FileRemover Cleanup(Main);
  constexpr llvm::StringLiteral DiskContents = "int fromDisk;\n";
  {
    llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS << DiskContents;
  }
  constexpr llvm::StringLiteral CheckerContents = "int fromOverlay;\n";
  addFile(Main, CheckerContents);

  IndexFileIn Indexed = runIndexingAction(Main);
  ASSERT_EQ(1u, Indexed.Graphs.size());
  ASSERT_EQ(1u, Indexed.Graphs.front().Sources.size());
  const GraphSource &Source = Indexed.Graphs.front().Sources.front();
  EXPECT_EQ(graphDigest(CheckerContents), Source.Digest);
  EXPECT_EQ(graphDigest(DiskContents), Source.DiskDigest);
  EXPECT_NE(Source.Digest, Source.DiskDigest);
}

TEST_F(IndexActionTest, GraphOffsetsAreFixedUTF16AndDoNotSaturate) {
  Opts.CollectGraph = true;
  const std::string Main = testPath("wide.cpp");
  const std::string Prefix = std::string("/*😀*/") + std::string(5000, ' ');
  addFile(Main, Prefix + "int veryLongSymbol;\n");

  IndexFileIn UTF8;
  {
    WithContextValue Negotiated(kCurrentOffsetEncoding, OffsetEncoding::UTF8);
    UTF8 = runIndexingAction(Main);
  }
  IndexFileIn UTF32;
  {
    WithContextValue Negotiated(kCurrentOffsetEncoding, OffsetEncoding::UTF32);
    UTF32 = runIndexingAction(Main);
  }
  auto Find = [](const IndexFileIn &Indexed) -> const GraphSymbol * {
    if (Indexed.Graphs.size() != 1)
      return nullptr;
    auto It = llvm::find_if(Indexed.Graphs.front().Symbols,
                            [](const GraphSymbol &Symbol) {
                              return Symbol.Name == "veryLongSymbol";
                            });
    return It == Indexed.Graphs.front().Symbols.end() ? nullptr : &*It;
  };
  const GraphSymbol *UTF8Symbol = Find(UTF8);
  const GraphSymbol *UTF32Symbol = Find(UTF32);
  ASSERT_TRUE(UTF8Symbol);
  ASSERT_TRUE(UTF32Symbol);
  EXPECT_EQ(5006u, UTF8Symbol->Declaration.StartColumn);
  EXPECT_EQ(5024u, UTF8Symbol->Declaration.EndColumn);
  EXPECT_EQ(UTF8Symbol->Declaration.StartColumn,
            UTF32Symbol->Declaration.StartColumn);
  EXPECT_EQ(UTF8Symbol->Declaration.EndColumn,
            UTF32Symbol->Declaration.EndColumn);
}

TEST(GraphTest, CommandDigestNormalizesEquivalentInputSpelling) {
  tooling::CompileCommand Native;
  Native.Directory = "C:/project";
  Native.Filename = "C:/project/main.cpp";
  Native.CommandLine = {"clang++", "-DVALUE=a\\b", "--",
                        "C:\\project\\main.cpp"};
  tooling::CompileCommand Slashes = Native;
  Slashes.CommandLine.back() = "C:/project/main.cpp";
#ifdef _WIN32
  EXPECT_EQ(graphCommandDigest(Native), graphCommandDigest(Slashes));
  Slashes.Directory = "c:\\PROJECT";
  Slashes.Filename = "c:\\PROJECT\\main.cpp";
  Slashes.CommandLine.back() = "c:\\PROJECT\\main.cpp";
  EXPECT_EQ(graphCommandDigest(Native), graphCommandDigest(Slashes));
#else
  EXPECT_NE(graphCommandDigest(Native), graphCommandDigest(Slashes));
#endif
  Slashes.CommandLine[1] = "-DVALUE=a/b";
  EXPECT_NE(graphCommandDigest(Native), graphCommandDigest(Slashes));
}

TEST(GraphTest, OnlyPlainCAndCXXCommandsAreAuthoritative) {
  tooling::CompileCommand Command;
  Command.Directory = testRoot();
  Command.Filename = testPath("main.cpp");
  Command.CommandLine = {"clang++", "-fsyntax-only", Command.Filename};
  EXPECT_TRUE(graphCommandIsCOrCXX(Command));

  Command.Filename = testPath("main.c");
  Command.CommandLine = {"clang", "-fsyntax-only", Command.Filename};
  EXPECT_TRUE(graphCommandIsCOrCXX(Command));
  for (const auto &Language :
       {"objective-c", "objective-c++", "cuda", "hip", "cl", "clcpp"}) {
    Command.Filename = testPath("mixed.cpp");
    Command.CommandLine = {"clang", "-x", Language, "-fsyntax-only",
                           Command.Filename};
    EXPECT_FALSE(graphCommandIsCOrCXX(Command)) << Language;
  }
}

TEST_F(IndexActionTest, IncludeGraphSelfInclude) {
  std::string MainFilePath = testPath("main.cpp");
  std::string MainCode = "#include \"header.h\"";
  std::string HeaderPath = testPath("header.h");
  std::string HeaderCode = R"cpp(
      #ifndef _GUARD_
      #define _GUARD_
      #include "header.h"
      #endif)cpp";

  addFile(MainFilePath, MainCode);
  addFile(HeaderPath, HeaderCode);

  IndexFileIn IndexFile = runIndexingAction(MainFilePath);
  auto Nodes = toMap(*IndexFile.Sources);

  EXPECT_THAT(
      Nodes,
      UnorderedElementsAre(
          Pair(toUri(MainFilePath), AllOf(isTU(), includesAre({HeaderPath}),
                                          hasDigest(digest(MainCode)))),
          Pair(toUri(HeaderPath), AllOf(Not(isTU()), includesAre({HeaderPath}),
                                        hasDigest(digest(HeaderCode))))));
}

TEST_F(IndexActionTest, IncludeGraphSkippedFile) {
  std::string MainFilePath = testPath("main.cpp");
  std::string MainCode = R"cpp(
      #include "common.h"
      #include "header.h"
      )cpp";

  std::string CommonHeaderPath = testPath("common.h");
  std::string CommonHeaderCode = R"cpp(
      #ifndef _GUARD_
      #define _GUARD_
      void f();
      #endif)cpp";

  std::string HeaderPath = testPath("header.h");
  std::string HeaderCode = R"cpp(
      #include "common.h"
      void g();)cpp";

  addFile(MainFilePath, MainCode);
  addFile(HeaderPath, HeaderCode);
  addFile(CommonHeaderPath, CommonHeaderCode);

  IndexFileIn IndexFile = runIndexingAction(MainFilePath);
  auto Nodes = toMap(*IndexFile.Sources);

  EXPECT_THAT(
      Nodes, UnorderedElementsAre(
                 Pair(toUri(MainFilePath),
                      AllOf(isTU(), includesAre({HeaderPath, CommonHeaderPath}),
                            hasDigest(digest(MainCode)))),
                 Pair(toUri(HeaderPath),
                      AllOf(Not(isTU()), includesAre({CommonHeaderPath}),
                            hasDigest(digest(HeaderCode)))),
                 Pair(toUri(CommonHeaderPath),
                      AllOf(Not(isTU()), includesAre({}),
                            hasDigest(digest(CommonHeaderCode))))));
}

TEST_F(IndexActionTest, IncludeGraphDynamicInclude) {
  std::string MainFilePath = testPath("main.cpp");
  std::string MainCode = R"cpp(
      #ifndef FOO
      #define FOO "main.cpp"
      #else
      #define FOO "header.h"
      #endif

      #include FOO)cpp";
  std::string HeaderPath = testPath("header.h");
  std::string HeaderCode = "";

  addFile(MainFilePath, MainCode);
  addFile(HeaderPath, HeaderCode);

  IndexFileIn IndexFile = runIndexingAction(MainFilePath);
  auto Nodes = toMap(*IndexFile.Sources);

  EXPECT_THAT(
      Nodes,
      UnorderedElementsAre(
          Pair(toUri(MainFilePath),
               AllOf(isTU(), includesAre({MainFilePath, HeaderPath}),
                     hasDigest(digest(MainCode)))),
          Pair(toUri(HeaderPath), AllOf(Not(isTU()), includesAre({}),
                                        hasDigest(digest(HeaderCode))))));
}

TEST_F(IndexActionTest, NoWarnings) {
  std::string MainFilePath = testPath("main.cpp");
  std::string MainCode = R"cpp(
      void foo(int x) {
        if (x = 1) // -Wparentheses
          return;
        if (x = 1) // -Wparentheses
          return;
      }
      void bar() {}
  )cpp";
  addFile(MainFilePath, MainCode);
  // We set -ferror-limit so the warning-promoted-to-error would be fatal.
  // This would cause indexing to stop (if warnings weren't disabled).
  IndexFileIn IndexFile = runIndexingAction(
      MainFilePath, {"-ferror-limit=1", "-Wparentheses", "-Werror"});
  ASSERT_TRUE(IndexFile.Sources);
  ASSERT_NE(0u, IndexFile.Sources->size());
  EXPECT_THAT(*IndexFile.Symbols, ElementsAre(hasName("foo"), hasName("bar")));
}

TEST_F(IndexActionTest, SkipFiles) {
  std::string MainFilePath = testPath("main.cpp");
  addFile(MainFilePath, R"cpp(
    // clang-format off
    #include "good.h"
    #include "bad.h"
    // clang-format on
  )cpp");
  addFile(testPath("good.h"), R"cpp(
    struct S { int s; };
    void f1() { S f; }
    auto unskippable1() { return S(); }
  )cpp");
  addFile(testPath("bad.h"), R"cpp(
    struct T { S t; };
    void f2() { S f; }
    auto unskippable2() { return S(); }
  )cpp");
  Opts.FileFilter = [](const SourceManager &SM, FileID F) {
    return !SM.getFileEntryRefForID(F)->getName().ends_with("bad.h");
  };
  IndexFileIn IndexFile = runIndexingAction(MainFilePath, {"-std=c++14"});
  EXPECT_THAT(*IndexFile.Symbols,
              UnorderedElementsAre(hasName("S"), hasName("s"), hasName("f1"),
                                   hasName("unskippable1")));
  for (const auto &Pair : *IndexFile.Refs)
    for (const auto &Ref : Pair.second)
      EXPECT_THAT(Ref.Location.FileURI, EndsWith("good.h"));
}

TEST_F(IndexActionTest, SkipNestedSymbols) {
  std::string MainFilePath = testPath("main.cpp");
  addFile(MainFilePath, R"cpp(
  namespace ns1 {
  namespace ns2 {
  namespace ns3 {
  namespace ns4 {
  namespace ns5 {
  namespace ns6 {
  namespace ns7 {
  namespace ns8 {
  namespace ns9 {
  class Bar {};
  void foo() {
    class Baz {};
  }
  }
  }
  }
  }
  }
  }
  }
  }
  })cpp");
  IndexFileIn IndexFile = runIndexingAction(MainFilePath, {"-std=c++14"});
  EXPECT_THAT(*IndexFile.Symbols, testing::Contains(hasName("foo")));
  EXPECT_THAT(*IndexFile.Symbols, testing::Contains(hasName("Bar")));
  EXPECT_THAT(*IndexFile.Symbols, Not(testing::Contains(hasName("Baz"))));
}

TEST_F(IndexActionTest, SymbolFromCC) {
  std::string MainFilePath = testPath("main.cpp");
  addFile(MainFilePath, R"cpp(
 #include "main.h"
 void foo() {}
 )cpp");
  addFile(testPath("main.h"), R"cpp(
 #pragma once
 void foo();
 )cpp");
  Opts.FileFilter = [](const SourceManager &SM, FileID F) {
    return !SM.getFileEntryRefForID(F)->getName().ends_with("main.h");
  };
  IndexFileIn IndexFile = runIndexingAction(MainFilePath, {"-std=c++14"});
  EXPECT_THAT(*IndexFile.Symbols,
              UnorderedElementsAre(AllOf(
                  hasName("foo"),
                  includeHeader(URI::create(testPath("main.h")).toString()))));
}

TEST_F(IndexActionTest, IncludeHeaderForwardDecls) {
  std::string MainFilePath = testPath("main.cpp");
  addFile(MainFilePath, R"cpp(
#include "fwd.h"
#include "full.h"
 )cpp");
  addFile(testPath("fwd.h"), R"cpp(
#ifndef _FWD_H_
#define _FWD_H_
struct Foo;
#endif
 )cpp");
  addFile(testPath("full.h"), R"cpp(
#ifndef _FULL_H_
#define _FULL_H_
struct Foo {};

// This decl is important, as otherwise we detect control macro for the file,
// before handling definition of Foo.
void other();
#endif
 )cpp");
  IndexFileIn IndexFile = runIndexingAction(MainFilePath);
  EXPECT_THAT(*IndexFile.Symbols,
              testing::Contains(AllOf(
                  hasName("Foo"),
                  includeHeader(URI::create(testPath("full.h")).toString()))))
      << *IndexFile.Symbols->begin();
}
} // namespace
} // namespace clangd
} // namespace clang
