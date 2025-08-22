#include "common/app/TwoPhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "mgmtd/MgmtdServer.h"

int main(int argc, char *argv[]) {
  using namespace hf3fs;
  return TwoPhaseApplication<mgmtd::MgmtdServer>().run(argc, argv);
}
