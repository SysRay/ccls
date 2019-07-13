#include "codeVisualizer.hh"
#include "message_handler.hh"
#include "log.hh"
#include <atlstr.h>  // windows
#include <windows.h> // windows


BOOL CALLBACK SendWMCloseMsg(HWND hwnd, LPARAM lParam) {
  DWORD dwProcessId = 0;
  GetWindowThreadProcessId(hwnd, &dwProcessId);
  if (dwProcessId == lParam)
    SendMessageTimeout(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 30000, NULL);
  return TRUE;
}

class codeVisualizer : public ICodeVisualizer {
private: 
   HANDLE wPipeInput, wPipeOutput;
   HANDLE rPipeInput, rPipeOutput;
   PROCESS_INFORMATION m_processInfo;
   HANDLE hJob;

 public:

    virtual bool init() override;
    virtual ~codeVisualizer() override {
      if (m_processInfo.hProcess != 0) {
        
        CloseHandle(hJob);

        CloseHandle(m_processInfo.hProcess);
        CloseHandle(m_processInfo.hThread);
        CloseHandle(wPipeOutput);
        CloseHandle(rPipeInput);
      }
      
    }

    virtual bool write_blocking(std::string const &data) override {
      DWORD written;
      return WriteFile(rPipeInput, &data[0], data.size(), &written, NULL);
    }
    virtual bool write_blocking(std::string &&data) override {
      DWORD written;
      return WriteFile(rPipeInput, &data[0], data.size(), &written, NULL);
    }
};
bool codeVisualizer::init() {
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
  DWORD flags = CREATE_SUSPENDED; // CREATE_NO_WINDOW;

  ZeroMemory(&m_processInfo, sizeof(PROCESS_INFORMATION));
  ZeroMemory(&si, sizeof(STARTUPINFO));
  si.cb = sizeof(STARTUPINFO);
  si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdInput = rPipeOutput;
  si.hStdError = wPipeOutput;
  si.hStdOutput = wPipeOutput;
  si.wShowWindow = SW_MINIMIZE;

  USES_CONVERSION;
  TCHAR *cmd = A2T(
      "F:/Source/codeVisualizer/codeVisualizer/bin/Debug/codeVisualizer.exe");

  if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, flags, NULL, NULL, &si, &m_processInfo)) {
    CloseHandle(m_processInfo.hProcess);
    CloseHandle(m_processInfo.hThread);
    CloseHandle(wPipeOutput);
    CloseHandle(rPipeInput);

    return false;
  }

  hJob = CreateJobObject(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &info,
                          sizeof(info));

  if (!(AssignProcessToJobObject(hJob, m_processInfo.hProcess) &&
          ResumeThread(m_processInfo.hThread) != (DWORD)-1) )
  {
    TerminateProcess(m_processInfo.hProcess, 1);
    CloseHandle(m_processInfo.hProcess);
    CloseHandle(m_processInfo.hThread);
    CloseHandle(wPipeOutput);
    CloseHandle(rPipeInput);
    m_processInfo.hProcess = m_processInfo.hThread = nullptr;
  }

  return true;
}


ICodeVisualizer &getICodeVisualizer() { 
  static codeVisualizer inst;
  return inst;
}


void CodeVisualizer::EmitClass(TextDocumentUML *data) {
  std::string payload = "{"
                        "\"command\":\"class\""
                        ",\"uri\":\"" + data->uri + "\""
                        ",\"class\":\"" + data->classDetailed + "\"";
  payload += ",\"function\":[";
                      
  for (auto &element: data->memberFunctions)
                       payload += "\"" + element + "\",";
  if (!data->memberFunctions.empty())
    payload.pop_back();

  payload += "],"
             "\"type\":[";

  for (auto &element : data->types)
                       payload += ",\"type\":\"" + element + "\",";
  if (!data->types.empty())
    payload.pop_back();

  payload += "]}";
  getICodeVisualizer().write_blocking(std::string("size=") + std::to_string(payload.size()) + "|" + payload);
}

