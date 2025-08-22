#include "common/app/TwoPhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "meta/service/MetaServer.h"

int main(int argc, char *argv[]) {
  using namespace hf3fs;
  return TwoPhaseApplication<meta::server::MetaServer>().run(argc, argv);
}
