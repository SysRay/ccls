#include "ICMakeserver.hh"

#include "ICMakeserver.hh"
#include "config.hh"
#include "log.hh"
#include "platform.hh"
#include "rapidjson/document.h"
#include "utils.hh"
#include <clang/Tooling/CompilationDatabase.h>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <llvm/ADT/StringRef.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
std::unordered_map<std::string, clang::tooling::CompileCommand>
extract(rapidjson::Document const &document);

std::unordered_map<std::string, clang::tooling::CompileCommand>
extractCacheCMakeServer(std::string const &path);
} // namespace
/// CMakeServer implementation
class CMakeServer : public clang::tooling::CompilationDatabase {
private:
  std::unique_ptr<ICMakeServerTerminal>
      m_terminal; ///< Terminal used for the server;
  std::string const
      m_buildDirectory; ///< Build directory with the cmakecache file
  std::string const m_sourceDirectory; ///< Normaly the cmake_home_dir from the
                                       ///< cmakecache file

  std::unique_ptr<std::thread>
      m_worker; ///< Thread that for the cmakeserver handling
  /// Thread function
  void workerFunction();
  bool m_isRunning = false; ///< is the thread running

  int m_versionMajor = 1; ///< comming from the cmakeserver: major version
  int m_versionMinor = 0; ///< comming from the cmakeserver: minor version
  bool m_rebuilding = true;
  std::string const
      m_pathCache; ///< The path were the cached file should be saved

  std::unordered_map<std::string, clang::tooling::CompileCommand>
      m_files; ///< The extracted targets from the cmakeServer
  mutable std::mutex m_mtxInterface;

  bool m_isExtracted = false;
  mutable std::condition_variable m_cond;

public:
  CMakeServer(std::string const &pathCache, std::string const &buildDirectory,
              std::string const &sourceDirectory,
              std::unique_ptr<ICMakeServerTerminal> terminal)
      : m_terminal(std::move(terminal)), m_buildDirectory(buildDirectory),
        m_sourceDirectory(sourceDirectory), m_pathCache(pathCache) {

    std::lock_guard<std::mutex> lock(m_mtxInterface);

    m_isRunning = true;
    m_worker = std::make_unique<std::thread>([this] { workerFunction(); });
  }

  virtual ~CMakeServer() {
    m_isRunning = false;
    m_terminal->deinit();
    m_worker->join();
    m_terminal.reset();
  }

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef FilePath) const override {
    std::unique_lock<std::mutex> lock(m_mtxInterface);
    while (!m_isExtracted) {
      m_cond.wait(lock);
    }
    return std::vector<clang::tooling::CompileCommand>{
        m_files.at(FilePath.str())};
  };

  virtual std::vector<std::string> getAllFiles() const override {
    std::unique_lock<std::mutex> lock(m_mtxInterface);
    while (!m_isExtracted) {
      m_cond.wait(lock);
    }

    std::vector<std::string> ret;
    std::transform(
        m_files.begin(), m_files.end(), back_inserter(ret),
        [](std::pair<std::string, clang::tooling::CompileCommand> const &pair) {
          return pair.second.Filename;
        });

    return ret;
  };

  virtual std::vector<clang::tooling::CompileCommand>
  getAllCompileCommands() const override {
    std::unique_lock<std::mutex> lock(m_mtxInterface);
    while (!m_isExtracted) {
      m_cond.wait(lock);
    }

    std::vector<clang::tooling::CompileCommand> ret;
    std::transform(
        m_files.begin(), m_files.end(), back_inserter(ret),
        [](std::pair<std::string, clang::tooling::CompileCommand> const &pair) {
          return pair.second;
        });

    return ret;
  };
};

void CMakeServer::workerFunction() {
  LOG_S(INFO) << "[CMakeServer] Started";

  std::string readTemp;
  bool foundStart = false;
  size_t startIndex, endIndex;
  std::string lastCMakeError;

  while (m_isRunning) {
    // Parse information
    std::string temp = m_terminal->read_blocking();
    if (temp.empty()) {
      continue;
    }
    readTemp += temp;

    for (;;) {
      if (readTemp.empty()) break;

      // Extract the message from the Server
      
      // Wait for header start
      if (!foundStart) {
        if (readTemp.size() <= (1 + size(CMAKE_SERVER_HEADER_START) + size(CMAKE_SERVER_HEADER_END))) {
          break; // Not enough data for header-start
        } else {
          startIndex = readTemp.find(CMAKE_SERVER_HEADER_START, 1);
          if (startIndex == std::string::npos) {
            readTemp.clear();
            break; // Not header-start
          } else {
            foundStart = true;
          }
        }
      }

      // Wait for header end
      endIndex = readTemp.find(CMAKE_SERVER_HEADER_END,
                               startIndex + size(CMAKE_SERVER_HEADER_START));
      if (endIndex == std::string::npos) {
        break;
      }

      // Extract the message
      std::string const message =
          readTemp.substr(startIndex + size(CMAKE_SERVER_HEADER_START) + 1,
                          endIndex - 3 - size(CMAKE_SERVER_HEADER_END));

      {
        size_t endTemp = startIndex + endIndex + size(CMAKE_SERVER_HEADER_END);
        if (endTemp > readTemp.size())
          endTemp = readTemp.size();
        readTemp.erase(readTemp.begin(), readTemp.begin() + startIndex +
                                             endIndex +
                                             size(CMAKE_SERVER_HEADER_END));
      }
      //- Extract the message from the Server

      // LOG_S(INFO) << "[CMakeServer] Recv: " << message; // For Debugging only

      // Parse json
      rapidjson::Document document;
      document.Parse(message.data());

      assert(document.IsObject());
      assert(document.HasMember("type"));

      //####### Handle the messages from the server #####

      // Error Message
      //[== "CMake Server" ==[
      //{"cookie":"", "errorMessage" : "Protocol version not supported.",
      //"inReplyTo" : "handshake", "type" : "error"}
      //]== "CMake Server" ==]
      if (std::string(document["type"].GetString()).compare("error") == 0) {
        assert(document.HasMember("errorMessage"));
        LOG_S(ERROR) << "[CMakeSever] Error occured: "
                     << document["errorMessage"].GetString();
 
        m_files = extractCacheCMakeServer(m_pathCache);
        m_isExtracted = true;
        m_cond.notify_all();
        m_isRunning = false; // Leave Thread!
      }

      // Hello Message
      // [== "CMake Server" ==[
      // {"supportedProtocolVersions":[{"major":1, "minor" : 0}], "type" :
      // "hello"}
      // ]== "CMake Server" ==]
      else if (std::string(document["type"].GetString()).compare("hello") == 0) {
        LOG_S(INFO) << "[CMakeServer] Received hello-Message";

        const rapidjson::Value &versions = document["supportedProtocolVersions"];
        assert(versions.IsArray());

        bool found = false;
        for (auto &line : versions.GetArray()) {
          assert(line.IsObject());

          assert(line.HasMember("major"));
          int const major = line["major"].GetInt() <= m_versionMajor;
          int const minor = line.HasMember("minor") == true ? line["minor"].GetInt() : 0;

          if (major >= m_versionMajor && minor >= m_versionMinor) {
            m_versionMajor = major;
            m_versionMinor = minor;
            
            // Send Handshake
            m_terminal->write_blocking(CMAKE_SERVER_COMMAND_HANDSHAKE(
                m_versionMajor, m_versionMinor, m_buildDirectory,
                m_sourceDirectory));

            found = true;
            break;
          }
        }

        if (!found) {
          LOG_S(ERROR) << "[CMakeServer] Version is not supported!";

          m_files = extractCacheCMakeServer(m_pathCache);
          m_isExtracted = true;
          m_cond.notify_all();
          m_isRunning = false; // Leave Thread!
        }
      }

      // Reply Message
      // [=="CMake Server" ==[
      // {"inReplyTo":"XXXXX", "type" : "reply"}
      // ]== "CMake Server" ==]
      else if (std::string(document["type"].GetString()).compare("reply") == 0) {
        assert(document.HasMember("inReplyTo"));
        if (std::string(document["inReplyTo"].GetString()).compare("handshake") == 0) {
          LOG_S(INFO) << "[CMakeServer] --> configuring ...";
          m_terminal->write_blocking(CMAKE_SERVER_COMMAND_CONFIGURE);
        }else if (std::string(document["inReplyTo"].GetString()).compare("configure") == 0) {
          LOG_S(INFO) << "[CMakeServer]  <- Configuring done!";

          LOG_S(INFO) << "[CMakeServer] --> computing ...";
          m_terminal->write_blocking(CMAKE_SERVER_COMMAND_COMPUTING);
        } else if (std::string(document["inReplyTo"].GetString()).compare("compute") == 0) {
          LOG_S(INFO) << "[CMakeServer]  <- computing done!";

          LOG_S(INFO) << "[CMakeServer] --> generating codemodel ...";
          m_terminal->write_blocking(CMAKE_SERVER_COMMAND_CODEMODEL);
        } else if (std::string(document["inReplyTo"].GetString()).compare("codemodel") == 0) {
          LOG_S(INFO) << "[CMakeServer]  <- codemodel complete!";
          m_rebuilding = false;

          // We got the project structure!
          // go through all source files -> extract
          LOG_S(INFO) << "[CMakeServer] --> Extracting ...";

          assert(document.HasMember("configurations"));
          assert(document["configurations"].IsArray());

          // Save the configurations to pathCache
          {
            std::fstream tempFile(m_pathCache, std::ofstream::out | std::ofstream::trunc);
            if (tempFile) {
              tempFile << message.data();
              LOG_S(INFO) << "[CMakeServer] Created Cache: " << m_pathCache;
            } else 
              LOG_S(ERROR) << "[CMakeServer] Couldn't create Cache! " << m_pathCache;
          }

          // Extract it and notify user
          {
            auto temp = extract(document);
            std::lock_guard<std::mutex> lock(m_mtxInterface);
            m_files.swap(temp);
          }

          LOG_S(INFO) << "[CMakeServer]  <- Extraction completed!";
          m_isExtracted = true;
          m_cond.notify_all();

        } else {
          LOG_S(ERROR) << "[CMakeServer] Unsupported Reply, " << document["inReplyTo"].GetString();
        }
      }

      // Message
      // [== "CMake Server" ==[
      // {"cookie":"", "message" : "Something happened.", "title" : "Title
      // Text", "inReplyTo" : "handshake", "type" : "message"}
      //]== "CMake Server" ==]
      else if (std::string(document["type"].GetString()).compare("message") == 0) {
        if (document.HasMember("title") && std::string(document["title"].GetString()).compare("Error") == 0) {
          assert(document.HasMember("message"));

          LOG_S(ERROR) << "[CMakeServer] " << document["message"].GetString();
          lastCMakeError = document["message"].GetString();
        }
      }

      // Progress
      //[== "CMake Server" == [
      //{"cookie":"", "inReplyTo" : "...", "progressCurrent" : 7,
      //"progressMaximum" : 1000, "progressMessage" : "...", "progressMinimum" :
      // 0, "type" : "progress"} ] == "CMake Server" == ]
      else if (std::string(document["type"].GetString()).compare("progress") == 0) {
        if (document.HasMember("progressCurrent") &&
            document.HasMember("progressMaximum") &&
            document.HasMember("progressMessage")) {
          // onProgress(document["progressMessage"].GetString(),
          // document["progressCurrent"].GetInt(),
          // document["progressMaximum"].GetInt());
        }
      }

      // Signal Message
      else if (std::string(document["type"].GetString()).compare("signal") == 0) {
        if (document.HasMember("name") && std::string(document["name"].GetString()).compare("dirty") == 0) {
          if (!m_rebuilding) {
            m_rebuilding = true;
            lastCMakeError.clear();

            LOG_S(INFO) << "[CMakeServer] Reconfiguring ...";
            // onReconfig();

            readTemp.clear();
            m_terminal->restart();
            break;
          }
        }
      }
    }
  }

  LOG_S(INFO) << "[CMakeServer] Exited!";
}

// Factory for CMakeServer
std::unique_ptr<clang::tooling::CompilationDatabase> createCMakeServer(
                  std::string const pathCache,
                  std::string const &buildDirectory,
                  std::string const &sourceDirectory,
                  std::unique_ptr<ICMakeServerTerminal> terminal) 
{
  return std::make_unique<CMakeServer>(pathCache, buildDirectory, sourceDirectory, std::move(terminal));
}

namespace {

std::unordered_map<std::string, clang::tooling::CompileCommand> extractCacheCMakeServer(std::string const &path) {
  auto file = ccls::ReadContent(path);
  if (!file)
    return {};

  rapidjson::Document document;
  document.Parse(file->data());
  return extract(document);
}

bool EndsWith(std::string const &value, std::string const &ending) {
  if (ending.size() > value.size())
    return false;
  return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

bool EndsWithAny(const std::string &value,
                 const std::vector<std::string> &endings) {
  return std::any_of(std::begin(endings), std::end(endings),
      [&value](const std::string &ending) { return EndsWith(value, ending); });
}

std::unordered_map<std::string, clang::tooling::CompileCommand> extract(rapidjson::Document const &document) {
  std::unordered_map<std::string, clang::tooling::CompileCommand> files;
  std::vector<std::string> const endings{".c", ".cc", ".cpp", ".cxx"};

  for (auto &config : document["configurations"].GetArray()) {
    assert(config.IsObject());
    if (config.HasMember("projects") && config["projects"].IsArray()) {
      for (auto &projects : config["projects"].GetArray()) {
        if (projects.HasMember("targets") && projects["targets"].IsArray()) {
          for (auto &target : projects["targets"].GetArray()) {
            assert(target.HasMember("type"));

            // Add only specific targets
            if ((std::string(target["type"].GetString()).compare("UTILITY") != 0) &&
                target.HasMember("sourceDirectory") &&
                target.HasMember("fullName")) 
            {
              std::string const sourceDirectory = std::string(target["sourceDirectory"].GetString());

              if (target.HasMember("fileGroups") && target["fileGroups"].IsArray()) {
                for (auto &fileGroup : target["fileGroups"].GetArray()) {
                  if (fileGroup.HasMember("sources") && fileGroup["sources"].IsArray()) {
                    // Parameter for sourcefile
                    
                    //bool isGenerated = false;
                    //if (fileGroup.HasMember("isGenerated"))
                    //  isGenerated = fileGroup["isGenerated"].GetBool();

                    //std::string language{"CXX"};
                    //if (fileGroup.HasMember("language"))
                    //  language = fileGroup["language"].GetString();

                    std::vector<std::string> args;
                    if (fileGroup.HasMember("compileFlags")) {
                      // Split the comma seperated compileFlags and add to args
                      std::stringstream argsStream(fileGroup["compileFlags"].GetString());

                      std::istream_iterator<std::string> begin(argsStream);
                      std::istream_iterator<std::string> end;
                      std::vector<std::string> vstrings(begin, end);

                      std::copy(vstrings.begin(), vstrings.end(),
                                std::back_inserter(args));
                    }

                    if (fileGroup.HasMember("defines") && fileGroup["defines"].IsArray()) {
                      for (auto &item : fileGroup["defines"].GetArray()) {
                        args.push_back("-D" + std::string(item.GetString()));
                      }
                    }

                    if (fileGroup.HasMember("includePath") &&
                        fileGroup["includePath"].IsArray()) {
                      for (auto &item : fileGroup["includePath"].GetArray()) {
                        auto const tempPath = std::string(item["path"].GetString());

                        if (item.HasMember("isSystem") &&
                            item["isSystem"].GetBool() == true) {
                          args.push_back("-isystem" + tempPath);
                        } else {
                          args.push_back("-I" + tempPath);
                        }
                      }
                    };

                    for (auto &source : fileGroup["sources"].GetArray()) {
                      // Only add source files!
                      if (!EndsWithAny(source.GetString(), endings))
                        continue;

                      clang::tooling::CompileCommand tempTarget;
                      tempTarget.Directory = sourceDirectory;
                      tempTarget.CommandLine = args;
                      tempTarget.Filename = source.GetString();

                      files[tempTarget.Filename] = std::move(tempTarget);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  return files;
}
} // namespace
