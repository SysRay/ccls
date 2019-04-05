/* Copyright 2017-2018 ccls Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#pragma once

#include "log.hh"
#include "position.hh"
#include "utils.hh"
#include <clang/Basic/FileManager.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <mutex>
#include <unordered_map>

#if LLVM_VERSION_MAJOR < 8
// D52783 Lift VFS from clang to llvm
namespace llvm {
namespace vfs = clang::vfs;
}
#endif

namespace ccls {
std::string PathFromFileEntry(const clang::FileEntry &file);

Range FromCharSourceRange(const clang::SourceManager &SM,
                          const clang::LangOptions &LangOpts,
                          clang::CharSourceRange R,
                          llvm::sys::fs::UniqueID *UniqueID = nullptr);

Range FromCharRange(const clang::SourceManager &SM,
                    const clang::LangOptions &LangOpts, clang::SourceRange R,
                    llvm::sys::fs::UniqueID *UniqueID = nullptr);

Range FromTokenRange(const clang::SourceManager &SM,
                     const clang::LangOptions &LangOpts, clang::SourceRange R,
                     llvm::sys::fs::UniqueID *UniqueID = nullptr);

Range FromTokenRangeDefaulted(const clang::SourceManager &SM,
                              const clang::LangOptions &Lang,
                              clang::SourceRange R, const clang::FileEntry *FE,
                              Range range);

std::unique_ptr<clang::CompilerInvocation>
BuildCompilerInvocation(const std::string &main, std::vector<const char *> args,
                        llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS);

class SystemFileCacher {
  std::unordered_map<std::string, std::string> files;
  std::mutex m_mtx;

public:
  void addFile(std::string const &path) {
    std::unique_lock lock(m_mtx);

    if (files.find(path) == files.end()) {
      auto file = ccls::ReadContent(path);
      if (file) {
        files.emplace(std::make_pair(path, *file));
      }
    }
  }

  void remapFiles(clang::CompilerInvocation* CI ) {
    std::unique_lock lock(m_mtx);
    for (auto &item : files) {
      CI->getPreprocessorOpts().addRemappedFile(
          item.first, llvm::MemoryBuffer::getMemBuffer(item.second).release());
    }
  }
  static SystemFileCacher &getInstance() {
    static SystemFileCacher inst;
    return inst;
  }

private:
};
const char *ClangBuiltinTypeName(int);
} // namespace ccls
