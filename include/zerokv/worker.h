#pragma once

#include "zerokv/transport/worker.h"

namespace zerokv {

using AcceptCallback [[deprecated("use zerokv::transport::AcceptCallback")]]
    = ::zerokv::transport::AcceptCallback;
using Listener [[deprecated("use zerokv::transport::Listener")]] = ::zerokv::transport::Listener;
using Worker [[deprecated("use zerokv::transport::Worker")]] = ::zerokv::transport::Worker;

}  // namespace zerokv
