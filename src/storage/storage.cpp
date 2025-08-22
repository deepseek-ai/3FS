#include "common/app/TwoPhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "storage/service/StorageServer.h"

int main(int argc, char *argv[]) {
  using namespace hf3fs;
  return TwoPhaseApplication<storage::StorageServer>().run(argc, argv);
}
