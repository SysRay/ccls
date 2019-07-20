/// @file \ccls\src\cmakeserver\cmakeserverTerminal.cc.
/// @author SysRay
/// Cmakeserver terminal class.

#include "ICMakeServerTerminal.hh"
#include "config.hh"
#include "log.hh"
#include "utils.hh"

#include <filesystem>
#include <atlstr.h> // windows
#include <string>
#include <windows.h> // windows


class consoleProcess : public ICMakeServerTerminal {
private:
  HANDLE wPipeInput, wPipeOutput;
  HANDLE rPipeInput, rPipeOutput;

  PROCESS_INFORMATION m_processInfo;
  std::string m_path;

public:
  std::string read_blocking() final;
  bool write_blocking(std::string const &) final;
  bool write_blocking(std::string &&) final;
  bool restart() final;
  void deinit() final {
    m_isValid = false;
    CHAR chBuf[1]{0};
    DWORD written;
    WriteFile(wPipeOutput, &chBuf[0], 1, &written, NULL);
  }

  /// Initializes this object.
  /// @param  path        Full pathname of the cmake exe.
  /// @param  preCommand  This command is send bevor calling cmake. (for setting
  /// env)
  /// @return True if it succeeds, false if it fails.
  bool init(std::string const &pathBuild, std::string const &pathCmake,
            std::string const &preCommand);

  ~consoleProcess() {
    if (m_isValid) {
      CloseHandle(m_processInfo.hProcess);
      CloseHandle(m_processInfo.hThread);
      CloseHandle(wPipeOutput);
      CloseHandle(rPipeInput);
    }
  }
};

std::string consoleProcess::read_blocking() {
  std::string retData;

  DWORD dwRead;
  CHAR chBuf[1024];
  BOOL bSuccess = FALSE;
  HANDLE hParentStdOut = GetStdHandle(STD_OUTPUT_HANDLE);

  while (m_isValid) {
    bSuccess = ReadFile(wPipeInput, chBuf, 1024, &dwRead, NULL);
    if (!bSuccess || dwRead == 0)
      break;

    retData += std::string(chBuf, dwRead);
    break;
  }
  return retData;
}

bool consoleProcess::write_blocking(std::string const &data) {
  DWORD written;
  return WriteFile(rPipeInput, &data[0], data.size(), &written, NULL);
}
bool consoleProcess::write_blocking(std::string &&data) {
  DWORD written;
  return WriteFile(rPipeInput, &data[0], data.size(), &written, NULL);
}

bool consoleProcess::init(std::string const &pathBuild,
                          std::string const &pathCmake,
                          std::string const &preCommand) {
  if (m_path.empty()) {
    m_path = pathCmake;
    
    auto file = ccls::ReadContent(pathBuild + "/CMakeCache.txt");
    if (file) {
      m_pathCode = getArg(*file, "CMAKE_HOME_DIRECTORY");
      LOG_S(INFO) << "[CMakeCache] Path to code is: " << m_pathCode;

      if (m_path.empty()) {
        m_path = getArg(*file, "CMAKE_COMMAND");
      }

      LOG_S(INFO) << "[CMakeCache] Path to exe is: " << m_path;
    } else {
      LOG_S(ERROR) << "[CMakeCache] Couldn't find file: "
                   << pathBuild << "/CMakeCache.txt";
      return false;
    }

    // Create pipes to write and read data
    SECURITY_ATTRIBUTES secattr;
    ZeroMemory(&secattr, sizeof(secattr));
    secattr.nLength = sizeof(secattr);
    secattr.bInheritHandle = TRUE;

    CreatePipe(&wPipeInput, &wPipeOutput, &secattr, 0);
    CreatePipe(&rPipeOutput, &rPipeInput, &secattr, 0);
    //-

    // Create Process with pipe
    STARTUPINFO si;
    DWORD flags = 0; // CREATE_NO_WINDOW;

    ZeroMemory(&m_processInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = rPipeOutput;
    si.hStdError = wPipeOutput;
    si.hStdOutput = wPipeOutput;

    std::string temp = "cmd.exe /v /c ";
    if (preCommand.size()) {
      temp += preCommand + " && ";
    }
    temp += "\"" + m_path + "\"" + CMAKE_PARAM_SERVERCALL;

    LOG_S(INFO) << temp;
    USES_CONVERSION;
    TCHAR *cmd = A2T(&temp[0]);

    if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si,
                       &m_processInfo)) {
      CloseHandle(m_processInfo.hProcess);
      CloseHandle(m_processInfo.hThread);
      CloseHandle(wPipeOutput);
      CloseHandle(rPipeInput);
      return false;
    }

    m_isValid = true;
    return true;
  }

  LOG_S(ERROR) << "[consoleProcess] Already initialized!";
  return false;
}

bool consoleProcess::restart() {
  CloseHandle(m_processInfo.hProcess);
  CloseHandle(m_processInfo.hThread);
  CloseHandle(wPipeOutput);
  CloseHandle(rPipeInput);

  // Create pipes to write and read data
  SECURITY_ATTRIBUTES secattr;
  ZeroMemory(&secattr, sizeof(secattr));
  secattr.nLength = sizeof(secattr);
  secattr.bInheritHandle = TRUE;

  CreatePipe(&wPipeInput, &wPipeOutput, &secattr, 0);
  CreatePipe(&rPipeOutput, &rPipeInput, &secattr, 0);
  //-

  // Create Process with pipe
  STARTUPINFO si;
  DWORD flags = 0; // CREATE_NO_WINDOW;

  ZeroMemory(&m_processInfo, sizeof(PROCESS_INFORMATION));
  ZeroMemory(&si, sizeof(STARTUPINFO));
  si.cb = sizeof(STARTUPINFO);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdInput = rPipeOutput;
  si.hStdError = wPipeOutput;
  si.hStdOutput = wPipeOutput;

  std::string temp = m_path + CMAKE_PARAM_SERVERCALL;
  USES_CONVERSION;
  TCHAR *cmd = A2T(&temp[0]);

  if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si,
                     &m_processInfo)) {
    CloseHandle(m_processInfo.hProcess);
    CloseHandle(m_processInfo.hThread);
    CloseHandle(wPipeOutput);
    CloseHandle(rPipeInput);
    return false;
  }

  m_isValid = true;
  return true;
}

// Factory
std::unique_ptr<ICMakeServerTerminal>
createLocalCMakeServerTerminal(std::string const &pathBuild,
                               std::string const &pathCmake,
                               std::string const &preCommand) {
  auto inst = std::make_unique<consoleProcess>();

  if (inst->init(pathBuild, pathCmake, preCommand) == true) {
    return inst;
  }

  return {};
}
