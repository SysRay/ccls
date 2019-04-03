#pragma once
#include <string>
constexpr char CMAKE_PARAM_SERVERCALL[] = " -E server --debug --experimental";

#define _CMAKE_SERVER_HEADER_START "[== \"CMake Server\" ==["
#define _CMAKE_SERVER_HEADER_END   "]== \"CMake Server\" ==]"

constexpr char CMAKE_SERVER_HEADER_START[] = _CMAKE_SERVER_HEADER_START;
constexpr char CMAKE_SERVER_HEADER_END[] = _CMAKE_SERVER_HEADER_END;

constexpr char CMAKE_SERVER_COMMAND_CONFIGURE[] = 
  _CMAKE_SERVER_HEADER_START"\n"                                              
  "{\"type\":\"configure\"}\n"
  _CMAKE_SERVER_HEADER_END"\r\n";

constexpr char CMAKE_SERVER_COMMAND_COMPUTING[] = 
  _CMAKE_SERVER_HEADER_START"\n"
  "{\"type\":\"compute\"}\n"
  _CMAKE_SERVER_HEADER_END"\r\n";

constexpr char CMAKE_SERVER_COMMAND_CODEMODEL[] = 
  _CMAKE_SERVER_HEADER_START"\n"
  "{\"type\":\"codemodel\"}\n"
   _CMAKE_SERVER_HEADER_END"\r\n";

inline std::string const CMAKE_SERVER_COMMAND_HANDSHAKE(int const versionMajor, int const versionMinor,std::string const& buildDirectory, std::string const& sourceDirectory) {
   std::string const temp =
     _CMAKE_SERVER_HEADER_START"\n"
              "{\"cookie\":\"zimtstern\",\"type\":\"handshake\",\"protocolVersion\":{\"major\":" + std::to_string(versionMajor) + ", \"minor\":" + std::to_string(versionMinor) + "},"
              "\"buildDirectory\":\"" + buildDirectory + "\", \"sourceDirectory\":\"" + sourceDirectory + "\"}\n"
     _CMAKE_SERVER_HEADER_END"\r\n";
   return temp;
}

// Helper function to get length of strings without \0
template<typename T, int sz>
int size(T(&)[sz]){
  return sz - 1;
}