//===--- Background.h - Build an index in a background thread ----*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_BACKGROUND_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_BACKGROUND_H

#include "Context.h"
#include "FSProvider.h"
#include "index/FileIndex.h"
#include "index/Index.h"
#include "index/Serialization.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/Threading.h"
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace clang {
namespace clangd {

// Base class for Shard Storage operations. See DiskShardStorage for more info.
class ShardStorage {
public:
  using FileDigest = decltype(llvm::SHA1::hash({}));
  virtual bool storeShard(llvm::StringRef ShardIdentifier,
                          IndexFileOut Shard) const = 0;
  virtual llvm::Expected<IndexFileIn>
  retrieveShard(llvm::StringRef ShardIdentifier, FileDigest Hash) const = 0;
  virtual bool initialize(llvm::StringRef Directory) = 0;
};

// Builds an in-memory index by by running the static indexer action over
// all commands in a compilation database. Indexing happens in the background.
// FIXME: it should also persist its state on disk for fast start.
// FIXME: it should watch for changes to files on disk.
class BackgroundIndex : public SwapIndex {
public:
  // FIXME: resource-dir injection should be hoisted somewhere common.
  BackgroundIndex(Context BackgroundContext, llvm::StringRef ResourceDir,
                  const FileSystemProvider &, ArrayRef<std::string> URISchemes,
                  std::unique_ptr<ShardStorage> IndexShardStorage = nullptr,
                  size_t ThreadPoolSize = llvm::hardware_concurrency());
  ~BackgroundIndex(); // Blocks while the current task finishes.

  // Enqueue a translation unit for indexing.
  // The indexing happens in a background thread, so the symbols will be
  // available sometime later.
  void enqueue(llvm::StringRef Directory, tooling::CompileCommand);
  // Index all TUs described in the compilation database.
  void enqueueAll(llvm::StringRef Directory,
                  const tooling::CompilationDatabase &);

  // Cause background threads to stop after ther current task, any remaining
  // tasks will be discarded.
  void stop();

  // Wait until the queue is empty, to allow deterministic testing.
  void blockUntilIdleForTest();

  using FileDigest = decltype(llvm::SHA1::hash({}));

private:
  /// Given index results from a TU, only update files in \p FilesToUpdate.
  void update(llvm::StringRef MainFile, SymbolSlab Symbols, RefSlab Refs,
              const llvm::StringMap<FileDigest> &FilesToUpdate);

  // configuration
  std::string ResourceDir;
  const FileSystemProvider &FSProvider;
  Context BackgroundContext;
  std::vector<std::string> URISchemes;
  std::unique_ptr<ShardStorage> IndexShardStorage;

  // index state
  llvm::Error index(tooling::CompileCommand);

  FileSymbols IndexedSymbols;
  llvm::StringMap<FileDigest> IndexedFileDigests; // Key is absolute file path.
  std::mutex DigestsMu;

  // queue management
  using Task = std::function<void()>;
  void run(); // Main loop executed by Thread. Runs tasks from Queue.
  void enqueueLocked(tooling::CompileCommand Cmd);
  std::mutex QueueMu;
  unsigned NumActiveTasks = 0; // Only idle when queue is empty *and* no tasks.
  std::condition_variable QueueCV;
  bool ShouldStop = false;
  std::deque<Task> Queue;
  std::vector<std::thread> ThreadPool; // FIXME: Abstract this away.
};

// Handles storage and retrieval of index shards into disk. Requires Initialize
// to be called before storing or retrieval. Creates a directory called
// ".clangd-index/" under the path provided during initialize. This class is
// thread-safe.
class DiskShardStorage : public ShardStorage {
  mutable std::mutex DiskShardRootMu;
  llvm::SmallString<128> DiskShardRoot;
  bool Initialized;

public:
  // Retrieves the shard if found and contents are consistent with the provided
  // Hash.
  llvm::Expected<IndexFileIn> retrieveShard(llvm::StringRef ShardIdentifier,
                                            FileDigest Hash) const;

  // Stores given shard with name ShardIdentifier under initialized directory.
  bool storeShard(llvm::StringRef ShardIdentifier, IndexFileOut Shard) const;

  // Initializes DiskShardRoot to (Directory + ".clangd-index/") which is the
  // base directory for all shard files. After the initialization succeeds all
  // subsequent calls or no-op.
  bool initialize(llvm::StringRef Directory);
};

} // namespace clangd
} // namespace clang

#endif
