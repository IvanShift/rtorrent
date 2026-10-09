#include "config.h"
#include "test/src/test_erasedata_intent.h"

#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <sys/wait.h>
#include <vector>

#include "core/erasedata_intent.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestErasedataIntent);

namespace {
namespace intent = core::erasedata_intent;

intent::Record record() {
  return {std::string(40, 'A'), std::string(32, 'b'), "0000000000000001",
      "user_1", std::string(32, 'c'), std::string(64, 'd'), 'P'};
}

void write_file(const std::string& name, const std::string& bytes = {}) {
  const int fd = ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CPPUNIT_ASSERT_MESSAGE("Test document creation failed", fd >= 0);
  core::SessionPathFd file(fd);
  CPPUNIT_ASSERT_EQUAL(static_cast<ssize_t>(bytes.size()), ::write(file.get(), bytes.data(), bytes.size()));
  CPPUNIT_ASSERT_EQUAL(0, ::fchmod(file.get(), 0600));
}

template <typename Operation>
void assert_refusal(Operation operation, std::initializer_list<std::string> details) {
  try { operation(); }
  catch (const torrent::input_error& error) {
    const std::string message = error.what();
    for (const auto& detail : details)
      CPPUNIT_ASSERT_MESSAGE("Refusal must identify " + detail + ": " + message,
          message.find(detail) != std::string::npos);
    return;
  }
  CPPUNIT_FAIL("Unsafe erasedata state must be refused");
}

// Closing the parent's pipe always releases the child, including on assertion
// failure; no child can escape this test with an inherited stable lock.
struct Child {
  pid_t pid;
  int release;
  bool waited = false;
  int finish() {
    ::close(release);
    release = -1;
    int status = 0;
    pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    waited = true;
    return result == pid ? status : -1;
  }
  ~Child() { if (!waited) (void)finish(); }
};
}

void TestErasedataIntent::setUp() {
  char path[] = "/tmp/rtorrent_erasedata_test_XXXXXX";
  CPPUNIT_ASSERT(::mkdtemp(path) != nullptr);
  m_session = path;
}

void TestErasedataIntent::tearDown() {
  std::filesystem::remove_all(m_session);
}

void TestErasedataIntent::test_canonical_record_round_trip() {
  auto source = record();
  for (char phase : {'P', 'E', 'C'}) {
    for (const auto& user : {std::string(), std::string(128, 'u'), std::string("user-1_a")}) {
      source.phase = phase;
      source.user = user;
      intent::Record restored;
      const auto bytes = intent::format(source);
      CPPUNIT_ASSERT(intent::parse(bytes, &restored));
      CPPUNIT_ASSERT(intent::same(source, restored));
      CPPUNIT_ASSERT_EQUAL(phase, restored.phase);
      CPPUNIT_ASSERT_EQUAL(bytes, intent::format(restored));
      if (user.size() == 128) CPPUNIT_ASSERT_EQUAL(intent::max_record_size, bytes.size());
    }
  }
  CPPUNIT_ASSERT_EQUAL(std::string("prepared"), std::string(intent::phase_name('P')));
  CPPUNIT_ASSERT_EQUAL(std::string("erase-started"), std::string(intent::phase_name('E')));
  CPPUNIT_ASSERT_EQUAL(std::string("committed"), std::string(intent::phase_name('C')));
}

void TestErasedataIntent::test_malformed_records_are_refused() {
  const auto source = record();
  const auto canonical = intent::format(source);
  for (const auto& bytes : {std::string(), canonical.substr(0, canonical.size() - 1),
      canonical + "\n", canonical + "x", " " + canonical,
      canonical.substr(0, canonical.size() - 1) + "\r\n",
      std::string("e2") + canonical.substr(2), canonical.substr(0, 3) + "PP" + canonical.substr(4),
      canonical.substr(0, canonical.size() - 1) + ":\n", std::string(324, 'x')}) {
    auto unchanged = source;
    CPPUNIT_ASSERT(!intent::parse(bytes, &unchanged));
    CPPUNIT_ASSERT(intent::same(source, unchanged));
    CPPUNIT_ASSERT_EQUAL(source.phase, unchanged.phase);
  }
  std::vector<intent::Record> invalid;
  auto changed = source; changed.hash[0] = 'a'; invalid.push_back(changed);
  changed = source; changed.hash.pop_back(); invalid.push_back(changed);
  changed = source; changed.incarnation[0] = 'B'; invalid.push_back(changed);
  changed = source; changed.generation = std::string(16, '0'); invalid.push_back(changed);
  changed = source; changed.generation[0] = 'F'; invalid.push_back(changed);
  changed = source; changed.generation = "1"; invalid.push_back(changed);
  changed = source; changed.user = std::string(129, 'u'); invalid.push_back(changed);
  for (const auto& user : {std::string("User"), std::string("a:b"), std::string("a/b"), std::string("a\0b", 3)}) {
    changed = source; changed.user = user; invalid.push_back(changed);
  }
  changed = source; changed.nonce[0] = 'C'; invalid.push_back(changed);
  changed = source; changed.digest[0] = 'D'; invalid.push_back(changed);
  changed = source; changed.phase = 'p'; invalid.push_back(changed);
  for (const auto& value : invalid) {
    CPPUNIT_ASSERT(!intent::valid(value));
    const auto bytes = "e1:" + std::string(1, value.phase) + ":" + value.hash + ":" + value.incarnation +
        ":" + value.generation + ":" + value.user + ":" + value.nonce + ":" + value.digest + "\n";
    CPPUNIT_ASSERT(!intent::parse(bytes, nullptr));
  }
}

void TestErasedataIntent::test_identity_does_not_depend_on_phase() {
  const auto source = record();
  auto advanced = source;
  advanced.phase = 'C';
  CPPUNIT_ASSERT(intent::same(source, advanced));
  advanced.nonce[0] = 'e';
  CPPUNIT_ASSERT(!intent::same(source, advanced));
}

void TestErasedataIntent::test_exact_record_publication_and_retirement() {
  auto source = record();
  core::SessionPathFd lease(intent::lock(m_session, source.hash, true));
  intent::write(m_session, source.hash, source, true);
  intent::Record restored;
  CPPUNIT_ASSERT(intent::read(m_session, source.hash, &restored));
  CPPUNIT_ASSERT(intent::same(source, restored));
  const auto document = intent::path(m_session, source.hash, ".erasedata");
  assert_refusal([&] { intent::write(m_session, source.hash, source, true); }, {document});
  auto stale = source;
  stale.nonce[0] = 'e';
  assert_refusal([&] { intent::write(m_session, source.hash, stale); }, {document, "identity-changed"});
  assert_refusal([&] { intent::remove(m_session, source.hash, stale); }, {document, "identity-changed"});
  source.phase = 'C';
  intent::write(m_session, source.hash, source);
  intent::remove(m_session, source.hash, restored);
  CPPUNIT_ASSERT(!intent::read(m_session, source.hash, nullptr));
  CPPUNIT_ASSERT_EQUAL(0, ::access(intent::path(m_session, source.hash, ".erasedata.lock").c_str(), F_OK));
}

void TestErasedataIntent::test_stable_lock_survives_record_retirement_and_fork() {
  const auto source = record();
  CPPUNIT_ASSERT_EQUAL(-1, intent::lock(m_session, source.hash, false));
  core::SessionPathFd lease(intent::lock(m_session, source.hash, true));
  struct stat before{};
  CPPUNIT_ASSERT_EQUAL(0, ::fstat(lease.get(), &before));
  intent::write(m_session, source.hash, source, true);
  intent::remove(m_session, source.hash, source);
  int release[2];
  CPPUNIT_ASSERT_EQUAL(0, ::pipe(release));
  const pid_t pid = ::fork();
  if (pid < 0) { ::close(release[0]); ::close(release[1]); CPPUNIT_FAIL("Lock child fork failed"); }
  if (pid == 0) {
    ::close(release[1]);
    char byte;
    ssize_t result;
    do { result = ::read(release[0], &byte, 1); } while (result < 0 && errno == EINTR);
    ::close(release[0]);
    ::close(lease.release());
    ::_exit(result == 0 ? 0 : 1);
  }
  ::close(release[0]);
  Child child{pid, release[1]};
  lease.reset(-1);
  assert_refusal([&] {
    const int acquired = intent::lock(m_session, source.hash, false);
    if (acquired >= 0) ::close(acquired);
  }, {"erasedata: collector-busy"});
  const int status = child.finish();
  CPPUNIT_ASSERT(WIFEXITED(status));
  CPPUNIT_ASSERT_EQUAL(0, WEXITSTATUS(status));
  core::SessionPathFd next(intent::lock(m_session, source.hash, false));
  struct stat after{};
  CPPUNIT_ASSERT_EQUAL(0, ::fstat(next.get(), &after));
  CPPUNIT_ASSERT_EQUAL(before.st_dev, after.st_dev);
  CPPUNIT_ASSERT_EQUAL(before.st_ino, after.st_ino);
  CPPUNIT_ASSERT_EQUAL(off_t(0), after.st_size);
  CPPUNIT_ASSERT_EQUAL(mode_t(0600), after.st_mode & 07777);
  CPPUNIT_ASSERT_EQUAL(nlink_t(1), after.st_nlink);
}

void TestErasedataIntent::test_missing_lock_and_unsafe_lock_are_refused() {
  const auto source = record();
  core::SessionPathFd dir(stop_intent::open_directory(m_session, true));
  const auto journal = intent::path(m_session, source.hash, ".erasedata");
  const auto stable = intent::path(m_session, source.hash, ".erasedata.lock");
  write_file(journal, intent::format(source));
  assert_refusal([&] { intent::lock(m_session, source.hash, false); }, {journal, stable});
  assert_refusal([&] { intent::lock(m_session, source.hash, true); }, {journal, stable});
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(journal.c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkfifo(stable.c_str(), 0600));
  assert_refusal([&] { intent::lock(m_session, source.hash, false); }, {stable, "unsafe-document"});
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(stable.c_str()));
  write_file(stable, "x");
  assert_refusal([&] { intent::lock(m_session, source.hash, false); }, {stable, "unsafe-document"});
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(stable.c_str()));
  write_file(stable);
  CPPUNIT_ASSERT_EQUAL(0, ::link(stable.c_str(), (m_session + "/other-link").c_str()));
  assert_refusal([&] { intent::lock(m_session, source.hash, false); }, {stable, "unsafe-document"});
  CPPUNIT_ASSERT_EQUAL(0, ::unlink((m_session + "/other-link").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(stable.c_str(), 0644));
  assert_refusal([&] { intent::lock(m_session, source.hash, false); }, {stable, "unsafe-document"});
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(stable.c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::symlink("missing", stable.c_str()));
  assert_refusal([&] { intent::lock(m_session, source.hash, false); }, {stable});
}

void TestErasedataIntent::test_session_absence_requires_every_document_and_staging_name() {
  const auto source = record();
  CPPUNIT_ASSERT(intent::session_absent(m_session, source.hash));
  for (const char* suffix : {"", ".rtorrent", ".libtorrent_resume", ".meta"}) {
    for (const char* staging : {"", ".new"}) {
      const auto document = m_session + "/" + source.hash + ".torrent" + suffix + staging;
      write_file(document);
      assert_refusal([&] { intent::session_absent(m_session, source.hash); }, {document, "session-document-present"});
      CPPUNIT_ASSERT_EQUAL(0, ::access(document.c_str(), F_OK));
      CPPUNIT_ASSERT_EQUAL(0, ::unlink(document.c_str()));
    }
  }
  const auto dangling = m_session + "/" + source.hash + ".torrent";
  CPPUNIT_ASSERT_EQUAL(0, ::symlink("missing", dangling.c_str()));
  assert_refusal([&] { intent::session_absent(m_session, source.hash); }, {dangling, "session-document-present"});
}
