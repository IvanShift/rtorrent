#ifndef RTORRENT_CORE_MIGRATION_TEST_CUT_H
#define RTORRENT_CORE_MIGRATION_TEST_CUT_H

// The cut switch comes from configure. Including config.h here keeps every
// translation unit on the same inline definition, whatever it included first.
#include "config.h"

#ifdef ENABLE_RR_MIGRATION_TEST_CUTS
#include <cstdlib>
#include <cstring>
#endif

namespace core {

// The single reader of RR_MIG_TEST_CUT. Only builds configured with
// --enable-rr-migration-test-cuts honor it; production builds return false
// without reading the environment. The value is read on every call because
// unit tests change it while one process runs.
inline bool migration_test_cut(const char* name) {
#ifdef ENABLE_RR_MIGRATION_TEST_CUTS
  const char* selected = ::getenv("RR_MIG_TEST_CUT");
  return selected != nullptr && std::strcmp(selected, name) == 0;
#else
  (void)name;
  return false;
#endif
}

} // namespace core

#endif
