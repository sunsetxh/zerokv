#pragma once

#include "zerokv/transport/memory.h"

namespace zerokv {

using RemoteKey [[deprecated("use zerokv::transport::RemoteKey")]] = ::zerokv::transport::RemoteKey;
using MemoryRegion [[deprecated("use zerokv::transport::MemoryRegion")]] = ::zerokv::transport::MemoryRegion;
using MemoryPool [[deprecated("use zerokv::transport::MemoryPool")]] = ::zerokv::transport::MemoryPool;
using RegistrationCache [[deprecated("use zerokv::transport::RegistrationCache")]]
    = ::zerokv::transport::RegistrationCache;

}  // namespace zerokv
