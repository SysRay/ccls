/// @file ccls\src\cmakeserver\ICMakeserver.hh.
/// @author SysRay
/// Interface for the cmakeserver.
#pragma once

#include "ICMakeserverTerminal.hh"
#include <clang/Tooling/CompilationDatabase.h>


// ############## Factory #################

/// Creates a cmakeServer object
/// @param Path + filename to use for the cache file
/// @param Path for buildDirectory
/// @param Path for sourceDirectory
/// @param Pass the terminal to cmakeserver
std::unique_ptr<clang::tooling::CompilationDatabase> createCMakeServer(
    std::string const pathCache, std::string const &buildDirectory,
    std::string const &sourceDirectory, std::unique_ptr<ICMakeServerTerminal> terminal);
