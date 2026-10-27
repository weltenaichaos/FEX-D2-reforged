// SPDX-License-Identifier: MIT
/*
$info$
tags: Bin|ARM64EC
desc: Reads and writes the list of learned game reads in FEX's cache folder (GameFiles.cpp)
$end_info$
*/

#include "GameFiles.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/File.h>
#include <FEXHeaderUtils/Filesystem.h>

namespace GameFiles::Store {
fextl::string LearnedPath() {
  return FEXCore::Config::GetCacheDirectory() + "GameReadsLearned.txt";
}

bool ReadText(const fextl::string& Path, fextl::string& Out, size_t MaxBytes) {
  FEXCore::File::File F(Path.c_str(), FEXCore::File::FileModes::READ);
  if (!F.IsValid()) {
    return false;
  }
  Out.clear();
  char Buffer[16384];
  while (Out.size() < MaxBytes) {
    const auto Got = F.Read(Buffer, sizeof(Buffer));
    if (Got <= 0) {
      break;
    }
    Out.append(Buffer, static_cast<size_t>(Got));
  }
  if (Out.size() > MaxBytes) {
    Out.resize(MaxBytes);
  }
  return true;
}

bool WriteText(const fextl::string& Path, const fextl::string& Text) {
  // Created without the trailing separator (see NativeUnpackCache.cpp); FEX's cache folder normally exists already.
  fextl::string Dir = FEXCore::Config::GetCacheDirectory();
  while (!Dir.empty() && (Dir.back() == '\\' || Dir.back() == '/')) {
    Dir.pop_back();
  }
  if (!Dir.empty() && !FHU::Filesystem::Exists(Dir)) {
    FHU::Filesystem::CreateDirectories(Dir);
  }
  using FEXCore::File::FileModes;
  FEXCore::File::File F(Path.c_str(), FileModes::WRITE | FileModes::CREATE | FileModes::TRUNCATE);
  if (!F.IsValid()) {
    return false;
  }
  size_t Done {};
  while (Done < Text.size()) {
    const auto Wrote = F.Write(Text.data() + Done, Text.size() - Done);
    if (Wrote <= 0) {
      return false;
    }
    Done += static_cast<size_t>(Wrote);
  }
  return true;
}
} // namespace GameFiles::Store
