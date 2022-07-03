// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.hh"
#include "query.hh"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>

namespace ccls {
void MessageHandler::textDocument_declaration(TextDocumentPositionParam &param,
                                              ReplyOnce &reply) {
  int file_id;
  auto [file, wf] =
      findOrFail(param.textDocument.uri.getPath(), reply, &file_id);
  if (!wf)
    return;

  std::vector<LocationLink> result;
  Position &ls_pos = param.position;
  for (SymbolRef sym : findSymbolsAtLocation(wf, file, param.position))
    for (DeclRef dr : getNonDefDeclarations(db, sym))
      if (!(dr.file_id == file_id &&
            dr.range.contains(ls_pos.line, ls_pos.character)))
        if (auto loc = getLocationLink(db, wfiles, dr))
          result.push_back(loc);
  reply.replyLocationLink(result);
}

void MessageHandler::textDocument_definition(TextDocumentPositionParam &param,
                                             ReplyOnce &reply) {
  int file_id;
  auto [file, wf] =
      findOrFail(param.textDocument.uri.getPath(), reply, &file_id);
  if (!wf)
    return;

  std::vector<LocationLink> result;
  Maybe<DeclRef> on_def;
  Position &ls_pos = param.position;

  for (SymbolRef sym : findSymbolsAtLocation(wf, file, ls_pos, true)) {
    // Special cases which are handled:
    //  - symbol has declaration but no definition (ie, pure virtual)
    //  - goto declaration while in definition of recursive type
    std::vector<DeclRef> drs;
    eachEntityDef(db, sym, [&](const auto &def) {
      if (def.spell) {
        DeclRef spell = *def.spell;
        if (spell.file_id == file_id &&
            spell.range.contains(ls_pos.line, ls_pos.character)) {
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
      for (DeclRef dr : getNonDefDeclarations(db, sym))
        if (!(dr.file_id == file_id &&
              dr.range.contains(ls_pos.line, ls_pos.character)))
          drs.push_back(dr);
      // There is no declaration but the cursor is on a definition.
      if (drs.empty() && on_def)
        drs.push_back(*on_def);
    }
    for (DeclRef dr : drs)
      if (auto loc = getLocationLink(db, wfiles, dr))
        result.push_back(loc);
  }

  reply.replyLocationLink(result);
}

void MessageHandler::textDocument_typeDefinition(
    TextDocumentPositionParam &param, ReplyOnce &reply) {
  auto [file, wf] = findOrFail(param.textDocument.uri.getPath(), reply);
  if (!file)
    return;

  std::vector<LocationLink> result;
  auto add = [&](const QueryType &type) {
    for (const auto &def : type.def)
      if (def.spell)
        if (auto loc = getLocationLink(db, wfiles, *def.spell))
          result.push_back(loc);
    if (result.empty())
      for (const DeclRef &dr : type.declarations)
        if (auto loc = getLocationLink(db, wfiles, dr))
          result.push_back(loc);
  };
  for (SymbolRef sym : findSymbolsAtLocation(wf, file, param.position)) {
    switch (sym.kind) {
    case Kind::Var: {
      const QueryVar::Def *def = db->getVar(sym).anyDef();
      if (def && def->type)
        add(db->getType(def->type));
      break;
    }
    case Kind::Type: {
      for (auto &def : db->getType(sym).def)
        if (def.alias_of) {
          add(db->getType(def.alias_of));
          break;
        }
      break;
    }
    default:
      break;
    }
  }

  reply.replyLocationLink(result);
}
} // namespace ccls
