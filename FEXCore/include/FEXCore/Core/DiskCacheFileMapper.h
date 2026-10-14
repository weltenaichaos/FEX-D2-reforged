// SPDX-License-Identifier: MIT
#pragma once

#include "FEXCore/Utils/CompilerDefs.h"
#include "FEXCore/Utils/File.h"

namespace FEXCore::DiskCache {
// Maps up to MapSize bytes of the file read-only. A mapper that can only map part of it (for example only the bytes the
// file holds right now) reports how much it mapped in MappedSize; reads past that fall back to file reads.
using FileMapperFunc = void* (*)(FEXCore::File::File::FileHandleType Handle, uint64_t MapSize, uint64_t* MappedSize);

FEX_DEFAULT_VISIBILITY void SetFileMapper(FileMapperFunc Func);
} // namespace FEXCore::DiskCache
