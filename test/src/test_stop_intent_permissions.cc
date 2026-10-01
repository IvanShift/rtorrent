#include "config.h"

#include "test/src/test_stop_intent_permissions.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "stop_intent.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestStopIntentPermissions);

namespace {
const std::string hash(40, 'a');

void write_file(const std::string& path, const std::string& contents, mode_t mode) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  CPPUNIT_ASSERT_MESSAGE(std::strerror(errno), fd >= 0);
  CPPUNIT_ASSERT_EQUAL(static_cast<ssize_t>(contents.size()), ::write(fd, contents.data(), contents.size()));
  CPPUNIT_ASSERT_EQUAL(0, ::close(fd));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(path.c_str(), mode));
}
}

void TestStopIntentPermissions::setUp() {
  test_fixture::setUp();
  char path[] = "/tmp/rtorrent_stop_intent_test_XXXXXX";
  CPPUNIT_ASSERT(::mkdtemp(path) != nullptr);
  m_session = path;
}

void TestStopIntentPermissions::tearDown() {
  ::unlink((m_session + "/linked").c_str());
  ::unlink((m_session + "/journal-link").c_str());
  const std::string nested = m_session + "/outer/session/.rutorrent-stop-intents";
  ::unlink((nested + "/" + hash).c_str());
  ::rmdir(nested.c_str());
  ::rmdir((m_session + "/outer/session").c_str());
  ::rmdir((m_session + "/outer").c_str());
  ::rmdir((m_session + "/target").c_str());
  const std::string dir = m_session + "/.rutorrent-stop-intents";
  ::unlink((dir + "/" + hash).c_str());
  ::unlink((dir + "/" + hash + ".claim").c_str());
  ::rmdir(dir.c_str());
  ::rmdir(m_session.c_str());
  test_fixture::tearDown();
}

void TestStopIntentPermissions::test_private_journal_accepts_owned_session_mode_0755() {
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0755));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(std::string(), stop_intent::read_lease(m_session, hash).token);
  struct stat st{};
  CPPUNIT_ASSERT_EQUAL(0, ::stat((m_session + "/.rutorrent-stop-intents").c_str(), &st));
  CPPUNIT_ASSERT_EQUAL(mode_t(0700), st.st_mode & 07777);
  CPPUNIT_ASSERT_EQUAL(0, ::stat((m_session + "/.rutorrent-stop-intents/" + hash).c_str(), &st));
  CPPUNIT_ASSERT_EQUAL(mode_t(0600), st.st_mode & 07777);
}

void TestStopIntentPermissions::test_writable_session_refuses_first_claim() {
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0777));
  CPPUNIT_ASSERT_THROW(stop_intent::counter(m_session, hash, true), std::runtime_error);
  CPPUNIT_ASSERT_THROW(stop_intent::read_lease(m_session, hash), std::runtime_error);
  CPPUNIT_ASSERT_EQUAL(-1, ::access((m_session + "/.rutorrent-stop-intents").c_str(), F_OK));
}

void TestStopIntentPermissions::test_writable_intent_directory_refuses_absence_and_claim() {
  const std::string dir = m_session + "/.rutorrent-stop-intents";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(dir.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(dir.c_str(), 0777));
  CPPUNIT_ASSERT_THROW(stop_intent::counter(m_session, hash, false), std::runtime_error);
  CPPUNIT_ASSERT_THROW(stop_intent::read_lease(m_session, hash), std::runtime_error);
}

void TestStopIntentPermissions::test_writable_journal_cannot_supply_generation() {
  const std::string dir = m_session + "/.rutorrent-stop-intents";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(dir.c_str(), 0700));
  write_file(dir + "/" + hash, "0000000000000001\n", 0666);
  CPPUNIT_ASSERT_THROW(stop_intent::counter(m_session, hash, false), std::runtime_error);
  CPPUNIT_ASSERT_THROW(stop_intent::counter(m_session, hash, true), std::runtime_error);
}

void TestStopIntentPermissions::test_readable_claim_cannot_reserve_download() {
  const std::string dir = m_session + "/.rutorrent-stop-intents";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(dir.c_str(), 0700));
  const std::string token = std::string(32, 'b') + ":0000000000000001";
  write_file(dir + "/" + hash + ".claim", token + "\n", 0644);
  CPPUNIT_ASSERT_THROW(stop_intent::read_lease(m_session, hash), std::runtime_error);
}

void TestStopIntentPermissions::test_trailing_slash_cannot_follow_session_symlink() {
  const std::string linked = m_session + "/linked";
  CPPUNIT_ASSERT_EQUAL(0, ::symlink(m_session.c_str(), linked.c_str()));
  CPPUNIT_ASSERT_THROW(stop_intent::counter(linked + "/", hash, false), std::runtime_error);
  CPPUNIT_ASSERT_THROW(stop_intent::read_lease(linked + "/", hash), std::runtime_error);
}

void TestStopIntentPermissions::test_writable_ancestor_cannot_replace_private_session() {
  const std::string outer = m_session + "/outer";
  const std::string nested = outer + "/session";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(outer.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(nested.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(outer.c_str(), 0777));
  CPPUNIT_ASSERT_THROW(stop_intent::counter(nested + "/", hash, true), std::runtime_error);
  CPPUNIT_ASSERT_THROW(stop_intent::read_lease(nested + "/", hash), std::runtime_error);
}

void TestStopIntentPermissions::test_relative_session_under_sticky_tmp_still_works() {
  const std::string prior = std::filesystem::current_path().string();
  CPPUNIT_ASSERT_EQUAL(0, ::chdir("/tmp"));
  try {
    const std::string relative = m_session.substr(m_session.find_last_of('/') + 1) + "/";
    CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(relative, hash, true));
  } catch (...) {
    ::chdir(prior.c_str());
    throw;
  }
  CPPUNIT_ASSERT_EQUAL(0, ::chdir(prior.c_str()));
}

void TestStopIntentPermissions::test_dotdot_cannot_hide_writable_ancestor() {
  const std::string outer = m_session + "/outer";
  const std::string target = m_session + "/target";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(outer.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(target.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(outer.c_str(), 0777));
  CPPUNIT_ASSERT_THROW(stop_intent::counter(outer + "/../target/", hash, false), std::runtime_error);
}

void TestStopIntentPermissions::test_setgid_session_accepts_private_journal() {
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 02755));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, false));
  struct stat st{};
  CPPUNIT_ASSERT_EQUAL(0, ::stat((m_session + "/.rutorrent-stop-intents").c_str(), &st));
  CPPUNIT_ASSERT_EQUAL(mode_t(0700), st.st_mode & 0777);
}

void TestStopIntentPermissions::test_legacy_counter_compacts_after_increment() {
  const std::string dir = m_session + "/.rutorrent-stop-intents";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(dir.c_str(), 0700));
  write_file(dir + "/" + hash, "0000000000000001\n0000000000000002\n0000000000000003\n", 0600);
  CPPUNIT_ASSERT_EQUAL(uint64_t(4), stop_intent::counter(m_session, hash, true));
  struct stat st{};
  CPPUNIT_ASSERT_EQUAL(0, ::stat((dir + "/" + hash).c_str(), &st));
  CPPUNIT_ASSERT_EQUAL(off_t(17), st.st_size);
}

void TestStopIntentPermissions::test_counter_stays_bounded() {
  for (unsigned i = 1; i <= 100; ++i)
    CPPUNIT_ASSERT_EQUAL(uint64_t(i), stop_intent::counter(m_session, hash, true));
  const std::string dir = m_session + "/.rutorrent-stop-intents";
  struct stat st{};
  CPPUNIT_ASSERT_EQUAL(0, ::stat((dir + "/" + hash).c_str(), &st));
  CPPUNIT_ASSERT_EQUAL(off_t(17), st.st_size);
  CPPUNIT_ASSERT_EQUAL(size_t(1), static_cast<size_t>(std::distance(
      std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator())));
}

void TestStopIntentPermissions::test_retired_counter_is_not_inherited() {
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
  stop_intent::erase_counter(m_session, hash);
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  stop_intent::erase_counter(m_session, hash);
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
}

void TestStopIntentPermissions::test_multiple_link_journal_is_refused() {
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
  CPPUNIT_ASSERT_EQUAL(0, ::link((m_session + "/.rutorrent-stop-intents/" + hash).c_str(),
                                (m_session + "/journal-link").c_str()));
  CPPUNIT_ASSERT_THROW(stop_intent::counter(m_session, hash, true), std::runtime_error);
  CPPUNIT_ASSERT_THROW(stop_intent::erase_counter(m_session, hash), std::runtime_error);
}
