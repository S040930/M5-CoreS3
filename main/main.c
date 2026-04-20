#include "app_core.h"

// ESP-IDF looks up this symbol by name.
// NOLINTNEXTLINE(misc-use-internal-linkage)
void app_main(void) {
  app_core_run();
}
