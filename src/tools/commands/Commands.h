#pragma once

#include "client/meta/MetaClient.h"

namespace hf3fs::tools {
void setDirLayout(meta::client::MetaClient &metaClient, const flat::UserInfo &ui);
void createWithLayout(meta::client::MetaClient &metaClient, const flat::UserInfo &ui);
}  // namespace hf3fs::tools
