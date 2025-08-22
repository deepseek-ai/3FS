#include "common/app/OnePhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "monitor_collector/service/MonitorCollectorServer.h"

int main(int argc, char *argv[]) {
  return hf3fs::OnePhaseApplication<hf3fs::monitor::MonitorCollectorServer>::instance().run(argc, argv);
}
