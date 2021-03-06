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

#include "sema_manager.hh"
#include "filesystem.hh"
#include "include_complete.hh"
#include "log.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "platform.hh"
#include "project.hh"
#include "working_files.hh"

#include <llvm/ADT/Twine.h>
#include <llvm/Support/Threading.h>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include <stdlib.h>
#include <stdexcept>
#include <thread>

namespace ccls {
using namespace llvm;

extern std::vector<std::string> g_init_options;

namespace {
enum class TextDocumentSyncKind { None = 0, Full = 1, Incremental = 2 };
REFLECT_UNDERLYING(TextDocumentSyncKind)

bool didChangeWatchedFiles;

struct ServerCap {
  struct SaveOptions {
    bool includeText = false;
  };
  struct TextDocumentSyncOptions {
    bool openClose = true;
    TextDocumentSyncKind change = TextDocumentSyncKind::Incremental;
    bool willSave = false;
    bool willSaveWaitUntil = false;
    SaveOptions save;
  } textDocumentSync;

  // The server provides hover support.
  bool hoverProvider = true;
  struct CompletionOptions {
    bool resolveProvider = false;

    // The characters that trigger completion automatically.
    // vscode doesn't support trigger character sequences, so we use ':'
    // for
    // '::' and '>' for '->'. See
    // https://github.com/Microsoft/language-server-protocol/issues/138.
    std::vector<const char *> triggerCharacters = {".", ":",  ">", "#",
                                                   "<", "\"", "/"};
  } completionProvider;
  struct SignatureHelpOptions {
    std::vector<const char *> triggerCharacters = {"(", ","};
  } signatureHelpProvider;
  bool declarationProvider = true;
  bool definitionProvider = true;
  bool typeDefinitionProvider = true;
  bool implementationProvider = true;
  bool referencesProvider = true;
  bool documentHighlightProvider = true;
  bool documentSymbolProvider = true;
  bool workspaceSymbolProvider = true;
  struct CodeActionOptions {
    std::vector<const char *> codeActionKinds = {"quickfix"};
  } codeActionProvider;
  struct CodeLensOptions {
    bool resolveProvider = false;
  } codeLensProvider;
  bool documentFormattingProvider = true;
  bool documentRangeFormattingProvider = true;
  Config::ServerCap::DocumentOnTypeFormattingOptions
      documentOnTypeFormattingProvider;
  bool renameProvider = true;
  struct DocumentLinkOptions {
    bool resolveProvider = true;
  } documentLinkProvider;
  bool foldingRangeProvider = true;
  // The server provides execute command support.
  struct ExecuteCommandOptions {
    std::vector<const char *> commands = {ccls_xref};
  } executeCommandProvider;
  Config::ServerCap::Workspace workspace;
};
REFLECT_STRUCT(ServerCap::CodeActionOptions, codeActionKinds);
REFLECT_STRUCT(ServerCap::CodeLensOptions, resolveProvider);
REFLECT_STRUCT(ServerCap::CompletionOptions, resolveProvider,
               triggerCharacters);
REFLECT_STRUCT(ServerCap::DocumentLinkOptions, resolveProvider);
REFLECT_STRUCT(ServerCap::ExecuteCommandOptions, commands);
REFLECT_STRUCT(ServerCap::SaveOptions, includeText);
REFLECT_STRUCT(ServerCap::SignatureHelpOptions, triggerCharacters);
REFLECT_STRUCT(ServerCap::TextDocumentSyncOptions, openClose, change, willSave,
               willSaveWaitUntil, save);
REFLECT_STRUCT(ServerCap, textDocumentSync, hoverProvider, completionProvider,
               signatureHelpProvider, declarationProvider, definitionProvider,
               implementationProvider, typeDefinitionProvider,
               referencesProvider, documentHighlightProvider,
               documentSymbolProvider, workspaceSymbolProvider,
               codeActionProvider, codeLensProvider, documentFormattingProvider,
               documentRangeFormattingProvider,
               documentOnTypeFormattingProvider, renameProvider,
               documentLinkProvider, foldingRangeProvider,
               executeCommandProvider, workspace);

struct DynamicReg {
  bool dynamicRegistration = false;
};
REFLECT_STRUCT(DynamicReg, dynamicRegistration);

// Workspace specific client capabilities.
struct WorkspaceClientCap {
  // The client supports applying batch edits to the workspace.
  std::optional<bool> applyEdit;

  struct WorkspaceEdit {
    // The client supports versioned document changes in `WorkspaceEdit`s
    std::optional<bool> documentChanges;
  };

  // Capabilities specific to `WorkspaceEdit`s
  std::optional<WorkspaceEdit> workspaceEdit;
  DynamicReg didChangeConfiguration;
  DynamicReg didChangeWatchedFiles;
  DynamicReg symbol;
  DynamicReg executeCommand;
};

REFLECT_STRUCT(WorkspaceClientCap::WorkspaceEdit, documentChanges);
REFLECT_STRUCT(WorkspaceClientCap, applyEdit, workspaceEdit,
               didChangeConfiguration, didChangeWatchedFiles, symbol,
               executeCommand);

// Text document specific client capabilities.
struct TextDocumentClientCap {
  struct Completion {
    struct CompletionItem {
      // Client supports snippets as insert text.
      //
      // A snippet can define tab stops and placeholders with `$1`, `$2`
      // and `${3:foo}`. `$0` defines the final tab stop, it defaults to
      // the end of the snippet. Placeholders with equal identifiers are linked,
      // that is typing in one will update others too.
      bool snippetSupport = false;
    } completionItem;
  } completion;

  // Ignore declaration, implementation, typeDefinition
  struct LinkSupport {
    bool linkSupport = false;
  } definition;

  struct DocumentSymbol {
    bool hierarchicalDocumentSymbolSupport = false;
  } documentSymbol;

  struct PublishDiagnostics {
    bool relatedInformation = false;
  } publishDiagnostics;
};

REFLECT_STRUCT(TextDocumentClientCap::Completion::CompletionItem,
               snippetSupport);
REFLECT_STRUCT(TextDocumentClientCap::Completion, completionItem);
REFLECT_STRUCT(TextDocumentClientCap::DocumentSymbol,
               hierarchicalDocumentSymbolSupport);
REFLECT_STRUCT(TextDocumentClientCap::LinkSupport, linkSupport);
REFLECT_STRUCT(TextDocumentClientCap::PublishDiagnostics, relatedInformation);
REFLECT_STRUCT(TextDocumentClientCap, completion, definition, documentSymbol, publishDiagnostics);

struct ClientCap {
  WorkspaceClientCap workspace;
  TextDocumentClientCap textDocument;
};
REFLECT_STRUCT(ClientCap, workspace, textDocument);

struct InitializeParam {
  // The rootUri of the workspace. Is null if no
  // folder is open. If both `rootPath` and `rootUri` are set
  // `rootUri` wins.
  std::optional<DocumentUri> rootUri;

  Config initializationOptions;
  ClientCap capabilities;

  enum class Trace {
    // NOTE: serialized as a string, one of 'off' | 'messages' | 'verbose';
    Off,      // off
    Messages, // messages
    Verbose   // verbose
  };
  Trace trace = Trace::Off;

  std::vector<WorkspaceFolder> workspaceFolders;
};

void Reflect(JsonReader &reader, InitializeParam::Trace &value) {
  if (!reader.m->IsString()) {
    value = InitializeParam::Trace::Off;
    return;
  }
  std::string v = reader.m->GetString();
  if (v == "off")
    value = InitializeParam::Trace::Off;
  else if (v == "messages")
    value = InitializeParam::Trace::Messages;
  else if (v == "verbose")
    value = InitializeParam::Trace::Verbose;
}

// initializationOptions is deserialized separately.
REFLECT_STRUCT(InitializeParam, rootUri, capabilities, trace, workspaceFolders);

struct InitializeResult {
  ServerCap capabilities;
};
REFLECT_STRUCT(InitializeResult, capabilities);

struct FileSystemWatcher {
  std::string globPattern = "**/*";
};
struct DidChangeWatchedFilesRegistration {
  std::string id = "didChangeWatchedFiles";
  std::string method = "workspace/didChangeWatchedFiles";
  struct Option {
    std::vector<FileSystemWatcher> watchers = {{}};
  } registerOptions;
};
struct RegistrationParam {
  std::vector<DidChangeWatchedFilesRegistration> registrations = {{}};
};
REFLECT_STRUCT(FileSystemWatcher, globPattern);
REFLECT_STRUCT(DidChangeWatchedFilesRegistration::Option, watchers);
REFLECT_STRUCT(DidChangeWatchedFilesRegistration, id, method, registerOptions);
REFLECT_STRUCT(RegistrationParam, registrations);

void *Indexer(void *arg_) {
  MessageHandler *h;
  int idx;
  auto *arg = static_cast<std::pair<MessageHandler *, int> *>(arg_);
  std::tie(h, idx) = *arg;
  delete arg;
  std::string name = "indexer" + std::to_string(idx);
  set_thread_name(name.c_str());
  pipeline::Indexer_Main(h->manager, h->vfs, h->project, h->wfiles);
  pipeline::ThreadLeave();
  return nullptr;
}
void getCMakeServerConfig( Config::CmakeServerConfig &config, std::string_view configData) {
  rapidjson::Document document;
  document.Parse(configData.data());

  if (document.FindMember("cmakePath") != document.MemberEnd()) {
    config.cmakePath = document["cmakePath"].GetString();
  }

  if (document.FindMember("runCmakeLocal") != document.MemberEnd()) {
    config.runCmakeLocal = document["runCmakeLocal"].GetBool();

    if (!config.runCmakeLocal) {
      if (document.FindMember("username") != document.MemberEnd()) {
        config.sshUser = document["username"].GetString();
      } else {
        LOG_S(ERROR) << "username not set in .cmakeServerConfig!";
        return ;
      }
      if (document.FindMember("remoteName") != document.MemberEnd()) {
        config.sshServer = document["remoteName"].GetString();
      } else {
        LOG_S(ERROR) << "remoteName not set in .cmakeServerConfig!";
        return;
      }

      if (document.FindMember("sshDir") != document.MemberEnd()) {
        config.sshDir = document["sshDir"].GetString();
      } else {
        LOG_S(ERROR) << "sshDir not set in .cmakeServerConfig!";
        return;
      }
    }
  }

  if (document.FindMember("cmakeBuildDir") != document.MemberEnd()) {
    config.cmakeBuildDir = document["cmakeBuildDir"].GetString();
  } else {
    LOG_S(ERROR) << "cmakeBuildDir not set in .cmakeServerConfig!";
    return;
  }

  if (document.FindMember("preCommand") != document.MemberEnd()) {
    config.preCommand = document["preCommand"].GetString();
  }

  if (document.FindMember("cmakeArguments") != document.MemberEnd()) {
    config.cmakeArguments = document["cmakeArguments"].GetString();
  }

  config._isValid = true;
}
} // namespace

void checkCMakeServerConfig(Config::CmakeServerConfig &config) {
  config._isValid = false;
  LOG_S(INFO) << "build: " << config.cmakeBuildDir;
  if (config.cmakePath.empty() || config.cmakeBuildDir.empty())
    return;

  if (!config.runCmakeLocal) {
    if (config.sshUser.empty() || config.sshDir.empty() || config.sshServer.empty())
      return;
  }

  config._isValid = true;
}


void Initialize(MessageHandler *m, InitializeParam &param, ReplyOnce &reply) {
  std::string project_path = NormalizePath(param.rootUri->GetPath());
  LOG_S(INFO) << "initialize in directory " << project_path << " with uri "
              << param.rootUri->raw_uri;

  {
    g_config = new Config(param.initializationOptions);
    rapidjson::Document reader;
    for (const std::string &str : g_init_options) {
      reader.Parse(str.c_str());
      if (!reader.HasParseError()) {
        JsonReader json_reader{&reader};
        try {
          Reflect(json_reader, *g_config);
        } catch (std::invalid_argument &) {
          // This will not trigger because parse error is handled in
          // MessageRegistry::Parse in lsp.cc
        }
      }
    }

    rapidjson::StringBuffer output;
    rapidjson::Writer<rapidjson::StringBuffer> writer(output);
    JsonWriter json_writer(&writer);
    Reflect(json_writer, *g_config);
    LOG_S(INFO) << "initializationOptions: " << output.GetString();

    if (g_config->cache.directory.size()) {
      SmallString<256> Path(g_config->cache.directory);
      sys::fs::make_absolute(project_path, Path);
      // Use upper case for the Driver letter on Windows.
      g_config->cache.directory =
          NormalizePath(Path.str()) + "/" +
          std::to_string(HashUsr(g_config->cmakeServerConfig.cmakeBuildDir));
      EnsureEndsInSlash(g_config->cache.directory);
    }

    checkCMakeServerConfig(g_config->cmakeServerConfig);
  }

  // Client capabilities
  const auto &capabilities = param.capabilities;
  g_config->client.hierarchicalDocumentSymbolSupport &=
      capabilities.textDocument.documentSymbol
          .hierarchicalDocumentSymbolSupport;
  g_config->client.linkSupport &=
      capabilities.textDocument.definition.linkSupport;
  g_config->client.snippetSupport &=
      capabilities.textDocument.completion.completionItem.snippetSupport;
  g_config->client.diagnosticsRelatedInformation &=
      capabilities.textDocument.publishDiagnostics.relatedInformation;
  didChangeWatchedFiles =
      capabilities.workspace.didChangeWatchedFiles.dynamicRegistration;

  // Ensure there is a resource directory.
  if (g_config->clang.resourceDir.empty())
    g_config->clang.resourceDir = GetDefaultResourceDirectory();
  DoPathMapping(g_config->clang.resourceDir);
  LOG_S(INFO) << "use -resource-dir=" << g_config->clang.resourceDir;

  // Send initialization before starting indexers, so we don't send a
  // status update too early.
  {
    InitializeResult result;
    auto &c = result.capabilities;
    c.documentOnTypeFormattingProvider =
        g_config->capabilities.documentOnTypeFormattingProvider;
    c.foldingRangeProvider = g_config->capabilities.foldingRangeProvider;
    c.workspace = g_config->capabilities.workspace;
    reply(result);
  }

  // Set project root.
  EnsureEndsInSlash(project_path);
  g_config->fallbackFolder = project_path;
  auto &workspaceFolders = g_config->workspaceFolders;
  for (const WorkspaceFolder &wf : param.workspaceFolders) {
    std::string path = wf.uri.GetPath();
    EnsureEndsInSlash(path);
    std::string real = RealPath(path);
    EnsureEndsInSlash(real);
    workspaceFolders.emplace_back(path, path == real ? "" : real);
  }
  if (workspaceFolders.empty()) {
    std::string real = RealPath(project_path);
    EnsureEndsInSlash(real);
    workspaceFolders.emplace_back(project_path,
                                  project_path == real ? "" : real);
  }
  std::sort(workspaceFolders.begin(), workspaceFolders.end(),
            [](auto &l, auto &r) { return l.first.size() > r.first.size(); });
  for (auto &[folder, real] : workspaceFolders)
    if (real.empty())
      LOG_S(INFO) << "workspace folder: " << folder;
    else
      LOG_S(INFO) << "workspace folder: " << folder << " -> " << real;

  if (g_config->cache.directory.empty())
    g_config->cache.retainInMemory = 1;
  else if (!g_config->cache.hierarchicalPath)
    for (auto &[folder, _] : workspaceFolders) {
      // Create two cache directories for files inside and outside of the
      // project.
      std::string escaped = EscapeFileName(folder.substr(0, folder.size() - 1));
      sys::fs::create_directories(g_config->cache.directory + escaped);
      sys::fs::create_directories(g_config->cache.directory + '@' + escaped);
    }

  idx::Init();
  for (auto &[folder, _] : workspaceFolders)
    m->project->Load(folder);

  // Start indexer threads. Start this after loading the project, as that
  // may take a long time. Indexer threads will emit status/progress
  // reports.
  if (g_config->index.threads == 0)
    g_config->index.threads = std::thread::hardware_concurrency();

  LOG_S(INFO) << "start " << g_config->index.threads << " indexers";
  for (int i = 0; i < g_config->index.threads; i++)
    SpawnThread(Indexer, new std::pair<MessageHandler *, int>{m, i});

  // Start scanning include directories before dispatching project
  // files, because that takes a long time.
  m->include_complete->Rescan();

  LOG_S(INFO) << "dispatch initial index requests";
  m->project->Index(m->wfiles, reply.id);

  m->manager->sessions.SetCapacity(g_config->session.maxNum);
}

void MessageHandler::initialize(JsonReader &reader, ReplyOnce &reply) {
  InitializeParam param;
  Reflect(reader, param);
  auto it = reader.m->FindMember("initializationOptions");
  if (it != reader.m->MemberEnd() && it->value.IsObject()) {
    JsonReader m1(&it->value);
    try {
      Reflect(m1, param.initializationOptions);
    } catch (std::invalid_argument &) {
      reader.path_.push_back("initializationOptions");
      reader.path_.insert(reader.path_.end(), m1.path_.begin(), m1.path_.end());
      throw;
    }
  }
  if (!param.rootUri) {
    reply.Error(ErrorCode::InvalidRequest, "expected rootUri");
    return;
  }
  Initialize(this, param, reply);
}

void StandaloneInitialize(MessageHandler &handler, const std::string &root) {
  InitializeParam param;
  param.rootUri = DocumentUri::FromPath(root);
  ReplyOnce reply{handler};
  Initialize(&handler, param, reply);
}

void MessageHandler::initialized(EmptyParam &) {
  if (didChangeWatchedFiles) {
    RegistrationParam param;
    pipeline::Request("client/registerCapability", param);
  }
}

void MessageHandler::shutdown(EmptyParam &, ReplyOnce &reply) {
  reply(JsonNull{});
}

void MessageHandler::exit(EmptyParam &) {
  pipeline::quit.store(true, std::memory_order_relaxed);
}
} // namespace ccls
