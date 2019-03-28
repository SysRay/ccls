#pragma once

#include "ICMakeserverTerminal.hh"
#include <clang/Tooling/CompilationDatabase.h>


// ############## Factory #################

/**
 * Creates a cmakeServer object
 * @param Path for buildDirectory
 * @param Path for sourceDirectory
 * @param Pass Object to CmakeServer (is handled by cmakeserver)
 */
std::unique_ptr<clang::tooling::CompilationDatabase> createCMakeServer(
    std::string const pathCache, std::string const &buildDirectory,
    std::string const &sourceDirectory, std::unique_ptr<ICMakeServerTerminal> terminal);
