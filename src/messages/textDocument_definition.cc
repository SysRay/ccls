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

#include "message_handler.hh"
#include "query.hh"
#include "log.hh"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>

namespace ccls {
namespace {
std::vector<DeclRef> GetNonDefDeclarationTargets(DB *db, SymbolRef sym) {
  switch (sym.kind) {
  case Kind::Var: {
    std::vector<DeclRef> ret = GetNonDefDeclarations(db, sym);
    // If there is no declaration, jump to its type.
    if (ret.empty()) {
      for (auto &def : db->GetVar(sym).def)
        if (def.type) {
          if (Maybe<DeclRef> use =
                  GetDefinitionSpell(db, SymbolIdx{def.type, Kind::Type})) {
            ret.push_back(*use);
            break;
          }
        }
    }
    return ret;
  }
  default:
    return GetNonDefDeclarations(db, sym);
  }
}
} // namespace

void MessageHandler::textDocument_declaration(TextDocumentPositionParam &param,
                                              ReplyOnce &reply) {
  int file_id;
  auto [file, wf] = FindOrFail(param.textDocument.uri.GetPath(), reply, &file_id);
  if (!wf)
    return;

  std::vector<LocationLink> result;
  Position &ls_pos = param.position;
  for (SymbolRef sym : FindSymbolsAtLocation(wf, file, param.position))
    for (DeclRef dr : GetNonDefDeclarations(db, sym))
      if (!(dr.file_id == file_id &&
            dr.range.Contains(ls_pos.line, ls_pos.character)))
        if (auto loc = GetLocationLink(db, wfiles, dr))
          result.push_back(loc);
  reply.ReplyLocationLink(result);
}

void MessageHandler::textDocument_definition(TextDocumentPositionParam &param,
                                             ReplyOnce &reply) {
  int file_id;
  auto [file, wf] = FindOrFail(param.textDocument.uri.GetPath(), reply, &file_id);
  if (!wf)
    return;

  std::vector<LocationLink> result;
  Maybe<DeclRef> on_def;
  Position &ls_pos = param.position;

  for (SymbolRef sym : FindSymbolsAtLocation(wf, file, ls_pos, true)) {
    // Special cases which are handled:
    //  - symbol has declaration but no definition (ie, pure virtual)
    //  - goto declaration while in definition of recursive type
    std::vector<DeclRef> drs;
    EachEntityDef(db, sym, [&](const auto &def) {
      if (def.spell) {
        DeclRef spell = *def.spell;
        if (spell.file_id == file_id &&
            spell.range.Contains(ls_pos.line, ls_pos.character)) {
          on_def = spell;
          drs.clear();
          return false;
        }
        drs.push_back(spell);
      }
      return true;
    });

    // |uses| is empty if on a declaration/definition, otherwise it includes
    // all declarations/definitions.
    if (drs.empty()) {
      for (DeclRef dr : GetNonDefDeclarationTargets(db, sym))
        if (!(dr.file_id == file_id &&
              dr.range.Contains(ls_pos.line, ls_pos.character)))
          drs.push_back(dr);
      // There is no declaration but the cursor is on a definition.
      if (drs.empty() && on_def)
        drs.push_back(*on_def);
    }
    for (DeclRef dr : drs)
      if (auto loc = GetLocationLink(db, wfiles, dr))
        result.push_back(loc);
  }

  reply.ReplyLocationLink(result);
}

void MessageHandler::textDocument_typeDefinition(
    TextDocumentPositionParam &param, ReplyOnce &reply) {
  auto [file, wf] = FindOrFail(param.textDocument.uri.GetPath(), reply);
  if (!file)
    return;

  std::vector<LocationLink> result;
  auto Add = [&](const QueryType &type) {
    for (const auto &def : type.def)
      if (def.spell)
        if (auto loc = GetLocationLink(db, wfiles, *def.spell))
          result.push_back(loc);
    if (result.empty())
      for (const DeclRef &dr : type.declarations)
        if (auto loc = GetLocationLink(db, wfiles, dr))
          result.push_back(loc);
  };
  for (SymbolRef sym : FindSymbolsAtLocation(wf, file, param.position)) {
    switch (sym.kind) {
    case Kind::Var: {
      const QueryVar::Def *def = db->GetVar(sym).AnyDef();
      if (def && def->type)
        Add(db->Type(def->type));
      break;
    }
    case Kind::Type: {
      for (auto &def : db->GetType(sym).def)
        if (def.alias_of) {
          Add(db->Type(def.alias_of));
          break;
        }
      break;
    }
    default:
      break;
    }
  }

  reply.ReplyLocationLink(result);
}
} // namespace ccls
