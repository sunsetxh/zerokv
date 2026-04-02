#pragma once

#include "zerokv/transport/future.h"

namespace zerokv {

using CompletionCallback [[deprecated("use zerokv::transport::CompletionCallback")]]
    = ::zerokv::transport::CompletionCallback;
using Request [[deprecated("use zerokv::transport::Request")]] = ::zerokv::transport::Request;

template <typename T>
using Future [[deprecated("use zerokv::transport::Future")]] = ::zerokv::transport::Future<T>;

template <typename T>
using Promise [[deprecated("use zerokv::transport::Promise")]] = ::zerokv::transport::Promise<T>;

}  // namespace zerokv
