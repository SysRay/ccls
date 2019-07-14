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

#include "log.hh"
#include "message_handler.hh"
#include "query.hh"
#include "sema_manager.hh"

#define FTS_FUZZY_MATCH_IMPLEMENTATION
#include "fts_fuzzy_match.hh"

#include <filesystem>
#include <unordered_set>

namespace ccls {
namespace {
struct Param {
  TextDocumentIdentifier textDocument;
  Position position;
  std::string direction;
};
REFLECT_STRUCT(Param, textDocument, position, direction);

Maybe<Range> FindParent(QueryFile *file, Pos pos) {
  Maybe<Range> parent;
  for (auto [sym, refcnt] : file->symbol2refcnt)
    if (refcnt > 0 && sym.extent.Valid() && sym.extent.start <= pos &&
        pos < sym.extent.end &&
        (!parent || (parent->start == sym.extent.start
                         ? parent->end < sym.extent.end
                         : parent->start < sym.extent.start)))
      parent = sym.extent;
  return parent;
}
} // namespace

void MessageHandler::ccls_navigate(JsonReader &reader, ReplyOnce &reply) {
  Param param;
  Reflect(reader, param);
  auto [file, wf] = FindOrFail(param.textDocument.uri.GetPath(), reply);
  if (!wf) {
    return;
  }
  Position ls_pos = param.position;
  if (wf->index_lines.size())
    if (auto line =
            wf->GetIndexPosFromBufferPos(ls_pos.line, &ls_pos.character, false))
      ls_pos.line = *line;
  Pos pos{(uint16_t)ls_pos.line, (int16_t)ls_pos.character};

  Maybe<Range> res;
  switch (param.direction[0]) {
  case 'D': {
    Maybe<Range> parent = FindParent(file, pos);
    for (auto [sym, refcnt] : file->symbol2refcnt)
      if (refcnt > 0 && pos < sym.extent.start &&
          (!parent || sym.extent.end <= parent->end) &&
          (!res || sym.extent.start < res->start))
        res = sym.extent;
    break;
  }
  case 'L':
    for (auto [sym, refcnt] : file->symbol2refcnt)
      if (refcnt > 0 && sym.extent.Valid() && sym.extent.end <= pos &&
          (!res || (res->end == sym.extent.end ? sym.extent.start < res->start
                                               : res->end < sym.extent.end)))
        res = sym.extent;
    break;
  case 'R': {
    Maybe<Range> parent = FindParent(file, pos);
    if (parent && parent->start.line == pos.line && pos < parent->end) {
      pos = parent->end;
      if (pos.column)
        pos.column--;
    }
    for (auto [sym, refcnt] : file->symbol2refcnt)
      if (refcnt > 0 && sym.extent.Valid() && pos < sym.extent.start &&
          (!res ||
           (sym.extent.start == res->start ? res->end < sym.extent.end
                                           : sym.extent.start < res->start)))
        res = sym.extent;
    break;
  }
  case 'U':
  default:
    for (auto [sym, refcnt] : file->symbol2refcnt)
      if (refcnt > 0 && sym.extent.Valid() && sym.extent.start < pos &&
          pos < sym.extent.end && (!res || res->start < sym.extent.start))
        res = sym.extent;
    break;
  }
  std::vector<Location> result;
  if (res)
    if (auto ls_range = GetLsRange(wf, *res)) {
      Location &ls_loc = result.emplace_back();
      ls_loc.uri = param.textDocument.uri;
      ls_loc.range = *ls_range;
    }
  reply(result);
}

void MessageHandler::ccls_toggleSourceHeader(JsonReader &reader,
                                             ReplyOnce &reply) {

  Param param;
  Reflect(reader, param);
  auto [file, wf] = FindOrFail(param.textDocument.uri.GetPath(), reply);
  if (!wf) {
    return;
  }

  bool isHeader = true;
  {
    std::string fEnding =
        std::filesystem::path(file->def->path).extension().string();
    if (fEnding == ".cxx" || fEnding == ".c" || fEnding == ".cpp" ||
        fEnding == ".cc") {
      isHeader = false;
    }
  }

  std::unordered_set<ccls::Usr> seen;
  std::string filePath = db->files[file->id].def->path;
  std::string fileName = std::filesystem::path(filePath).filename().string();
  fileName = fileName.substr(0, fileName.find_last_of('.'));

  std::vector<Location> result;
  std::unordered_map<std::string, int> fileCount;
  for (auto [sym, refcnt] : file->symbol2refcnt) {
    if (seen.insert(sym.usr).second && sym.kind == Kind::Func) {
      QueryFunc &type = db->GetFunc(sym);

      if (isHeader) {
        for (auto &dec : type.declarations) {
          auto decFile = db->files[dec.file_id];
          if (decFile.def->path != filePath)
            continue;

          for (auto &def : type.def) {
            auto defFile = db->files[def.file_id];
            if (defFile.def->path == filePath)
              continue;
            
            std::string fEnding = std::filesystem::path(defFile.def->path).extension().string();
            if (fEnding == ".cxx" || fEnding == ".c" || fEnding == ".cpp" ||
                fEnding == ".cc") {
              std::string defFileName = std::filesystem::path(defFile.def->path).filename().string();
              defFileName = defFileName.substr(0, defFileName.find_last_of('.'));

              int score = ComputeGuessScore(fileName, defFileName);
              //fts::fuzzy_match(fileName.data(), defFileName.data(), score);
              
              fileCount[defFile.def->path] = score;
            }
          }
        }
      } else {
        for (auto &def : type.def) {
          auto defFile = db->files[def.file_id];
          if (!def.spell || defFile.def->path != filePath)
            continue;

          for (auto &dec : type.declarations) {
            std::string const decFileName = db->files[dec.file_id].def->path;
            if (decFileName != filePath) {
              if (fileCount.find(decFileName) == fileCount.end()) {
                fileCount[decFileName] = 0;
              } else {
                fileCount[decFileName] = ++fileCount[decFileName];
              }
            }
          }
        }
      }
    }
  }

  auto maxValue =
      std::max_element(std::begin(fileCount), std::end(fileCount),
                       [](const decltype(fileCount)::value_type &p1,
                          const decltype(fileCount)::value_type &p2) {
                         return p1.second < p2.second;
                       });

  if (maxValue != std::end(fileCount)) {
    LOG_S(INFO) << "toggle: " << maxValue->first << " score: " << maxValue->second;

    Location temp;
    temp.uri = DocumentUri::FromPath(maxValue->first);
    result.push_back(std::move(temp));
  }

  reply(result);
}
} // namespace ccls
