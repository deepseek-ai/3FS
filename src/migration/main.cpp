#include "common/app/OnePhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "migration/service/Server.h"

int main(int argc, char *argv[]) {
  return hf3fs::OnePhaseApplication<hf3fs::migration::server::MigrationServer>::instance().run(argc, argv);
}
