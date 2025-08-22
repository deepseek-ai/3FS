#include "meta/event/Event.h"

#include <cassert>
#include <folly/json.h>
#include <folly/logging/Logger.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/xlog.h>

#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"

namespace hf3fs::meta::server {
namespace {
folly::Logger create("eventlog.Create");
folly::Logger mkdir("eventlog.Mkdir");
folly::Logger hardLink("eventlog.HardLink");
folly::Logger remove("eventlog.Remove");
folly::Logger truncate("eventlog.Truncate");
folly::Logger openWrite("eventlog.OpenWrite");
folly::Logger closeWrite("eventlog.CloseWrite");
folly::Logger rename("eventlog.Rename");
folly::Logger symlink("eventlog.Symlink");
folly::Logger gc("eventlog.GC");
folly::Logger unknown("eventlog.Unknown");
}  // namespace

static folly::Logger &getLogger(Event::Type type) {
  switch (type) {
    case Event::Type::Create:
      return create;
    case Event::Type::Mkdir:
      return mkdir;
    case Event::Type::HardLink:
      return hardLink;
    case Event::Type::Remove:
      return remove;
    case Event::Type::Truncate:
      return truncate;
    case Event::Type::OpenWrite:
      return openWrite;
    case Event::Type::CloseWrite:
      return closeWrite;
    case Event::Type::Rename:
      return rename;
    case Event::Type::Symlink:
      return symlink;
    case Event::Type::GC:
      return gc;
  }
  XLOGF(DFATAL, "Unknown type {}", (int)type);
  return unknown;
}

void Event::log() const {
  folly::json::serialization_opts opts;
  opts.pretty_formatting = false;
  opts.sort_keys = false;
  return log(opts);
}

void Event::log(const folly::json::serialization_opts &opts) const {
  try {
    auto msg = folly::json::serialize(data, opts);
    auto logger = getLogger(type);
    FB_LOG(logger, INFO, msg);
  } catch (folly::json::print_error &exception) {
    XLOGF(ERR, "Event failed to serialize to json, type {}, error {}", magic_enum::enum_name(type), exception.what());
  }
}

}  // namespace hf3fs::meta::server