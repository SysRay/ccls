/// @file F:\Source\ccls\src\cmakeserver\ICMakeserverTerminal.hh.
/// @author SysRay
/// Interface for the cmakeserver terminal.
/// 
#pragma once
#include <string>
#include <memory>
#include <vector>


/// Terminal for CmakeServer to communicate with.
class ICMakeServerTerminal {
public:

  /// Read blocking from the CMakeServer.
  /// @return empty on communication error!
  virtual std::string read_blocking() = 0;

  /// Blocking Write.
  /// @param Data to write.
  /// @return True if it succeeds, false if it fails.
  virtual bool write_blocking(std::string const&) = 0;

  /// Blocking Write.
  /// @param Data to write.
  /// @return True if it succeeds, false if it fails.
  virtual bool write_blocking(std::string &&) = 0;

  /// Restarts the terminal.
  /// @return True if it succeeds, false if it fails.
  virtual bool restart() = 0;

  /// Deinits the terminal.
  virtual void deinit() = 0;
  virtual ~ICMakeServerTerminal() = default;
protected:

  bool m_isValid = false;
};

/// Creates a process with input and output pipe for cmake running localy.
/// @return nullptr on error.
std::unique_ptr<ICMakeServerTerminal>
createLocalCMakeServerTerminal(std::string const &path,
                               std::string const &preCommand);

/// Creates a process with input and output pipe for cmake running localy.
/// @return nullptr on error.
std::unique_ptr<ICMakeServerTerminal> createRemoteCMakeServerTerminal(
    std::string const &sshdir, std::string const &path,
    std::string const &hostname, std::string const &username,
    std::string const &password, int const port, std::string const &preCommand);