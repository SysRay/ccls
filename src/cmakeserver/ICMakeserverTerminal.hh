#pragma once
#include <string>
#include <memory>
#include <vector>
class ICMakeServerTerminal {
public:
  /**
  * @brief Read blocking from the CMakeServer
  * @return empty on communication error! 
  */
  virtual std::string read_blocking() = 0;
  virtual bool write_blocking(std::string const&) = 0;
  virtual bool write_blocking(std::string &&) = 0;
  virtual bool restart() = 0;
  virtual void deinit() = 0;
  virtual ~ICMakeServerTerminal() = default;
protected:

  bool m_isValid = false;
};

/**
  * Creates a process with input and output pipe for cmake running localy
  * @param Path to the cmake executable
  * @return nullptr on error
*/
std::unique_ptr<ICMakeServerTerminal> createLocalCMakeServerTerminal(std::string const& path);

/**
  * Creates a process with input and output pipe for cmake running localy
  * @param Path to the cmake executable
  * @return nullptr on error
*/
std::unique_ptr<ICMakeServerTerminal> createRemoteCMakeServerTerminal(
    std::string const &sshdir, std::string const &path,
    std::string const &hostname, std::string const &username,
    std::string const &password, int const port);