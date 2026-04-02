#pragma once

#include "zerokv/transport/endpoint.h"

namespace zerokv {

using Endpoint [[deprecated("use zerokv::transport::Endpoint")]] = ::zerokv::transport::Endpoint;

}  // namespace zerokv
