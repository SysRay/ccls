#pragma once
#include <string>
#include <vector>

class ICodeVisualizer {
public:
  virtual bool init() = 0;
  virtual ~ICodeVisualizer() = default;
  virtual bool write_blocking(std::string const &data) = 0;
  virtual bool write_blocking(std::string &&data) = 0;
};

ICodeVisualizer &getICodeVisualizer();

struct TextDocumentUML {
  std::string uri;
  std::string classDetailed;
  std::vector<std::string> memberFunctions;
  std::vector<std::string> types;
};

namespace CodeVisualizer {
  void EmitClass(TextDocumentUML * data);
}