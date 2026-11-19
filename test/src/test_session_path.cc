#include "config.h"

#include "test/src/test_session_path.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <torrent/exceptions.h>

#include "core/erasedata_intent.h"
#include "core/session_path.h"
#include "stop_intent.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestSessionPath);

namespace {
struct stat identity(int fd) {
  struct stat st{};
  CPPUNIT_ASSERT_EQUAL_MESSAGE(std::strerror(errno), 0, ::fstat(fd, &st));
  return st;
}

struct stat path_identity(const std::string& path) {
  struct stat st{};
  CPPUNIT_ASSERT_EQUAL_MESSAGE(std::strerror(errno), 0, ::lstat(path.c_str(), &st));
  return st;
}

bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

const std::string intents = stop_intent::intent_directory;
const std::string journals = ".retrackers-migration";
const std::string hash(40, 'A');
const std::string other_hash(40, 'B');

void write_file(const std::string& path, const std::string& contents) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CPPUNIT_ASSERT_MESSAGE(path + ": " + std::strerror(errno), fd >= 0);
  CPPUNIT_ASSERT_EQUAL(static_cast<ssize_t>(contents.size()), ::write(fd, contents.data(), contents.size()));
  CPPUNIT_ASSERT_EQUAL(0, ::close(fd));
}

void write_journal(const std::string& path, char generation = '3') {
  write_file(path, std::string(15, '0') + generation + "\n");
}

std::string complete_claim() {
  return std::string(32, 'b') + ":0000000000000001\n" + std::string(32, 'c') + "\n100\n" + std::string(40, 'd') + "\n";
}

std::string refusal(const std::string& session) {
  try {
    core::SessionPathFd fd(core::open_trusted_session_directory(session));
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return std::string();
}

// The identity of a copy handed out now (and pins the session if no pass holds it).
struct stat handout_identity(const std::string& session) {
  core::SessionPathFd fd(core::open_trusted_session_directory(session));
  return identity(fd.get());
}

std::string counter_refusal(const std::string& session) {
  try {
    stop_intent::counter(session, hash, false);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return std::string();
}

std::set<std::string> names(int directory) {
  std::set<std::string> result;
  DIR* entries = ::fdopendir(::dup(directory));
  CPPUNIT_ASSERT(entries != nullptr);
  while (const auto* entry = ::readdir(entries)) result.insert(entry->d_name);
  ::closedir(entries);
  return result;
}

// Open descriptors of the process: a leak shows even above a freed lower number.
int open_descriptors() {
  int count = 0;
  for (int fd = 0; fd < 1024; ++fd)
    count += ::fcntl(fd, F_GETFD) != -1;
  return count;
}

// A sidecar directory without a gateway of its own in these tests.
core::SidecarDirectory read_journals(const std::string& session) {
  return core::read_session_sidecar(session, journals, [](int parent, struct stat* found) {
    const int fd = ::openat(parent, journals.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && found) ::fstat(fd, found);
    return fd;
  });
}
}

void TestSessionPath::setUp() {
  test_fixture::setUp();
  char path[] = "/tmp/rtorrent_session_path_test_XXXXXX";
  CPPUNIT_ASSERT(::mkdtemp(path) != nullptr);
  m_root = path;
  m_outer = m_root + "/outer";
  m_session = m_outer + "/session";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(m_outer.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(m_session.c_str(), 0700));
  core::session_directory_pass().serve(std::this_thread::get_id());
}

void TestSessionPath::tearDown() {
  core::session_directory_pass().serve(std::thread::id());
  ::chmod(m_outer.c_str(), 0700);
  std::filesystem::remove_all(m_root);
  test_fixture::tearDown();
}

// The probes of one pass share one verified walk: a later probe keeps the
// pinned directory after an ancestor was made unsafe, and the next pass walks
// again and refuses it (check() refuses it too: test_check_attests_again).
void TestSessionPath::test_pass_walks_once() {
  core::SessionPathFd first(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0777));
  core::SessionPathFd second(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT(same(identity(first.get()), identity(second.get())));
  core::session_directory_end();
  const std::string message = refusal(m_session);
  CPPUNIT_ASSERT_MESSAGE("Unsafe ancestor must be refused after the pass: " + message,
                         message.find(m_outer + " ") != std::string::npos &&
                         message.find("mode=0777") != std::string::npos);
}

void TestSessionPath::test_failed_walk_is_not_cached() {
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0777));
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string message = refusal(m_session);
    CPPUNIT_ASSERT_MESSAGE("Every caller must get the refusal: " + message,
                           message.find("unsafe session directory") != std::string::npos);
  }
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0700));
  core::SessionPathFd accepted(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT(same(identity(accepted.get()), path_identity(m_session)));
}

// Each copy is a close-on-exec reopening of the pinned inode with an offset of
// its own: reading the entries of one leaves the other's listing whole.
void TestSessionPath::test_copies_have_own_offset() {
  write_file(m_session + "/entry", "x");
  core::SessionPathFd first(core::open_trusted_session_directory(m_session));
  core::SessionPathFd second(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT((::fcntl(first.get(), F_GETFD) & FD_CLOEXEC) != 0);
  CPPUNIT_ASSERT((::fcntl(second.get(), F_GETFD) & FD_CLOEXEC) != 0);
  CPPUNIT_ASSERT(same(identity(first.get()), identity(second.get())));
  CPPUNIT_ASSERT_EQUAL(size_t(1), names(first.get()).count("entry"));
  CPPUNIT_ASSERT_EQUAL(size_t(1), names(second.get()).count("entry"));
  core::SessionPathFd third(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT_EQUAL(size_t(1), names(third.get()).count("entry"));
}

// The pass belongs to one session path: an unsafe other path is walked and
// refused on every request, and the first path is walked again afterwards.
void TestSessionPath::test_other_path_refused_each_time() {
  const std::string other = m_outer + "/other";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(other.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(other.c_str(), 0770));
  core::SessionPathFd first(core::open_trusted_session_directory(m_session));
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string message = refusal(other);
    CPPUNIT_ASSERT_MESSAGE("Other session path must be refused: " + message,
                           message.find("unsafe session directory ownership or mode: " + other + " ") != std::string::npos);
  }
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0777));
  const std::string message = refusal(m_session);
  CPPUNIT_ASSERT_MESSAGE("The first path must be walked again: " + message,
                         message.find("unsafe session path ancestor ownership or mode: " + m_outer + " ") != std::string::npos);
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0700));
  core::SessionPathFd again(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT(same(identity(again.get()), identity(first.get())));
}

// Every entry point called on another thread walks or does nothing: it never
// reads, drops or ends the serving thread's pass.
void TestSessionPath::test_other_thread_never_uses_pass() {
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir((m_session + "/" + intents).c_str(), 0700));
  write_journal(m_session + "/" + intents + "/" + hash, '3');
  core::SessionPathFd pinned(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(m_session, hash, false));
  // A reader on another thread walks with descriptors of its own and closes them.
  const int before = open_descriptors();
  uint64_t value = 0;
  std::thread([&] { value = stop_intent::counter(m_session, hash, false); }).join();
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), value);
  CPPUNIT_ASSERT_EQUAL(before, open_descriptors());
  CPPUNIT_ASSERT_EQUAL(0, ::rename((m_session + "/" + intents).c_str(), (m_session + "/old").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir((m_session + "/" + intents).c_str(), 0700));
  write_journal(m_session + "/" + intents + "/" + hash, '5');
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0777));
  std::string opened, read;
  std::thread([&] {
    opened = refusal(m_session);
    read = counter_refusal(m_session);
    core::session_sidecar_created(intents);
    core::session_directory_check();
    core::session_directory_end();
  }).join();
  CPPUNIT_ASSERT_MESSAGE("Another thread must walk: " + opened, opened.find(m_outer + " ") != std::string::npos);
  CPPUNIT_ASSERT_MESSAGE("Another thread must walk: " + read, read.find(m_outer + " ") != std::string::npos);
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(m_session, hash, false));
  core::SessionPathFd again(core::open_trusted_session_directory(m_session));
  CPPUNIT_ASSERT(same(identity(again.get()), identity(pinned.get())));
}

// Readers borrow the pass's descriptors without closing them; the pass closes
// them when it ends and when another session path replaces it.
void TestSessionPath::test_descriptors_closed() {
  const std::string other = m_outer + "/other";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(other.c_str(), 0700));
  for (const auto& session : {m_session, other})
    for (const auto& name : {intents, journals})
      CPPUNIT_ASSERT_EQUAL(0, ::mkdir((session + "/" + name).c_str(), 0700));
  const int before = open_descriptors();
  const auto use = [](const std::string& session) {
    const int first = stop_intent::read_directory(session).get();
    CPPUNIT_ASSERT(first >= 0 && ::fcntl(first, F_GETFD) >= 0);
    CPPUNIT_ASSERT_EQUAL(first, stop_intent::read_directory(session).get());
    const int second = read_journals(session).get();
    CPPUNIT_ASSERT(second >= 0 && ::fcntl(second, F_GETFD) >= 0);
  };
  use(m_session);
  // The pinned session directory and both sidecar directories; a check() that
  // keeps them closes what its walk opened.
  const int held = open_descriptors();
  CPPUNIT_ASSERT_EQUAL(before + 3, held);
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(held, open_descriptors());
  use(other);
  CPPUNIT_ASSERT_EQUAL(held, open_descriptors());
  core::session_directory_end();
  CPPUNIT_ASSERT_EQUAL(before, open_descriptors());
  // Writable gateways hand out descriptors of their own, which the writers close.
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
  const int lock = core::erasedata_intent::lock(m_session, hash, true);
  CPPUNIT_ASSERT(lock >= 0);
  ::close(lock);
  core::session_directory_end();
  CPPUNIT_ASSERT_EQUAL(before, open_descriptors());
}

// Under one active pass every reader answers from a live probe of its name:
// a name created by a writer outside the owner after the pass started is seen.
void TestSessionPath::test_readers_see_new_names() {
  const std::string directory = m_session + "/" + intents;
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(directory.c_str(), 0700));
  write_journal(directory + "/" + other_hash, '3');
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(m_session, other_hash, false));
  // The pass holds the directories across a check() that finds them unchanged.
  // A walk would refuse this ancestor now, so the readers answer from the
  // held descriptors.
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0777));
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT(stop_intent::read_lease(m_session, hash).token.empty());
  CPPUNIT_ASSERT(!core::erasedata_intent::read(m_session, hash, nullptr));
  CPPUNIT_ASSERT_EQUAL(-1, core::erasedata_intent::lock(m_session, hash, false));
  write_journal(directory + "/" + hash, '7');
  write_file(directory + "/" + hash + ".claim", complete_claim());
  const core::erasedata_intent::Record record{hash, std::string(32, 'a'), std::string(16, '1'),
                                             "test", std::string(32, 'b'), std::string(64, 'c'), 'P'};
  write_file(directory + "/" + hash + ".erasedata", core::erasedata_intent::format(record));
  write_file(directory + "/" + hash + ".erasedata.lock", "");
  CPPUNIT_ASSERT_EQUAL(uint64_t(7), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(complete_claim().substr(0, 49), stop_intent::read_lease(m_session, hash).token);
  core::erasedata_intent::Record read;
  CPPUNIT_ASSERT(core::erasedata_intent::read(m_session, hash, &read));
  CPPUNIT_ASSERT(core::erasedata_intent::same(record, read));
  const int lock = core::erasedata_intent::lock(m_session, hash, false);
  CPPUNIT_ASSERT(lock >= 0);
  ::close(lock);
}

// A missing directory is cached as missing: the daemon's writable gateway
// that creates it drops that within the pass; a directory created outside the
// daemon is seen from the next unit of work.
void TestSessionPath::test_created_directory_seen() {
  const std::string directory = m_session + "/" + intents;
  const int before = open_descriptors();
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(-1, ::access(directory.c_str(), F_OK));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, true));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, false));
  // A writer drops the held descriptor too, and closes it.
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, other_hash, true));
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, other_hash, false));
  core::session_directory_end();
  CPPUNIT_ASSERT_EQUAL(before, open_descriptors());
  CPPUNIT_ASSERT_EQUAL(0, ::rename(directory.c_str(), (m_session + "/away").c_str()));
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  // An unchanged absence keeps the pass: a walk would refuse this ancestor now.
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0777));
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::rename((m_session + "/away").c_str(), directory.c_str()));
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(uint64_t(1), stop_intent::counter(m_session, hash, false));
}

// A unit of work keeps the pinned inode; the next one sees a replacement, or
// refuses it as a new walk does. A sidecar directory the pass holds, still
// present under the detached original, does not keep the pass there.
void TestSessionPath::test_check_follows_replaced_session() {
  // The daemon's session.path ends with a slash.
  const std::string session = m_session + "/";
  const auto original = path_identity(m_session);
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir((m_session + "/" + intents).c_str(), 0700));
  write_journal(m_session + "/" + intents + "/" + hash, '3');
  handout_identity(session);
  core::session_directory_check();
  CPPUNIT_ASSERT(same(handout_identity(session), original));
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::rename(m_session.c_str(), (m_outer + "/first").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(m_session.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir((m_session + "/" + intents).c_str(), 0700));
  write_journal(m_session + "/" + intents + "/" + hash, '5');
  const auto replacement = path_identity(m_session);
  CPPUNIT_ASSERT(same(handout_identity(session), original));
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(session, hash, false));
  core::session_directory_check();
  CPPUNIT_ASSERT(same(handout_identity(session), replacement));
  CPPUNIT_ASSERT_EQUAL(uint64_t(5), stop_intent::counter(session, hash, false));
  // A symlink to the pinned inode is a replacement too: the walk refuses it.
  CPPUNIT_ASSERT_EQUAL(0, ::rename(m_session.c_str(), (m_outer + "/second").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::symlink("second", m_session.c_str()));
  core::session_directory_check();
  std::string message = refusal(session);
  CPPUNIT_ASSERT_MESSAGE("A symlinked session must be refused: " + message,
                         message.find("cannot open session path component: " + m_session) != std::string::npos);
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(m_session.c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(m_session.c_str(), 0700));
  handout_identity(session);
  CPPUNIT_ASSERT_EQUAL(0, ::rename(m_session.c_str(), (m_outer + "/third").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(m_session.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0777));
  core::session_directory_check();
  message = refusal(session);
  CPPUNIT_ASSERT_MESSAGE("An unsafe replacement must be refused: " + message,
                         message.find("unsafe session directory ownership or mode: " + m_session) != std::string::npos);
}

void TestSessionPath::test_check_follows_replaced_sidecar() {
  const std::string directory = m_session + "/" + intents;
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(directory.c_str(), 0700));
  write_journal(directory + "/" + hash, '3');
  write_file(directory + "/" + hash + ".claim", complete_claim());
  const core::erasedata_intent::Record record{hash, std::string(32, 'a'), std::string(16, '1'),
                                             "test", std::string(32, 'b'), std::string(64, 'c'), 'P'};
  write_file(directory + "/" + hash + ".erasedata", core::erasedata_intent::format(record));
  write_file(directory + "/" + hash + ".erasedata.lock", "");
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::rename(directory.c_str(), (m_session + "/first").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(directory.c_str(), 0700));
  write_journal(directory + "/" + hash, '5');
  // Until the next unit of work every reader keeps the attested directory.
  CPPUNIT_ASSERT_EQUAL(uint64_t(3), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(complete_claim().substr(0, 49), stop_intent::read_lease(m_session, hash).token);
  CPPUNIT_ASSERT(core::erasedata_intent::read(m_session, hash, nullptr));
  const int lock = core::erasedata_intent::lock(m_session, hash, false);
  CPPUNIT_ASSERT(lock >= 0);
  ::close(lock);
  core::session_directory_check();
  CPPUNIT_ASSERT_EQUAL(uint64_t(5), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT(stop_intent::read_lease(m_session, hash).token.empty());
  CPPUNIT_ASSERT(!core::erasedata_intent::read(m_session, hash, nullptr));
  CPPUNIT_ASSERT_EQUAL(-1, core::erasedata_intent::lock(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::rename(directory.c_str(), (m_session + "/second").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(directory.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(directory.c_str(), 0777));
  core::session_directory_check();
  const std::string message = counter_refusal(m_session);
  CPPUNIT_ASSERT_MESSAGE("An unsafe replacement must be refused: " + message,
                         message.find("unsafe stop intent directory ownership or mode: " + directory) != std::string::npos);
}

// A refused sidecar directory is not cached: every reader refuses, and the
// first reader after a repair succeeds within the same pass.
void TestSessionPath::test_unsafe_sidecar_refused_each_time() {
  const std::string directory = m_session + "/" + intents;
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(directory.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(directory.c_str(), 0777));
  for (int attempt = 0; attempt < 2; ++attempt) {
    CPPUNIT_ASSERT(counter_refusal(m_session).find("unsafe stop intent directory") != std::string::npos);
    CPPUNIT_ASSERT_THROW(stop_intent::read_lease(m_session, hash), std::runtime_error);
  }
  try {
    core::erasedata_intent::read(m_session, hash, nullptr);
    CPPUNIT_FAIL("erasedata must refuse an unsafe intent directory");
  } catch (const torrent::input_error& error) {
    CPPUNIT_ASSERT(std::string(error.what()).find("erasedata: unsafe stop intent directory") == 0);
  }
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(directory.c_str(), 0700));
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  core::session_directory_end();
  // A symlink or a file in place of the directory is refused too.
  CPPUNIT_ASSERT_EQUAL(0, ::rename(directory.c_str(), (m_session + "/real").c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::symlink("real", directory.c_str()));
  CPPUNIT_ASSERT(counter_refusal(m_session).find("cannot open stop intent directory") != std::string::npos);
  core::session_directory_end();
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(directory.c_str()));
  write_file(directory, "");
  CPPUNIT_ASSERT_THROW(stop_intent::read_lease(m_session, hash), std::runtime_error);
  core::session_directory_end();
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0777));
  CPPUNIT_ASSERT(counter_refusal(m_session).find("unsafe session directory") != std::string::npos);
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0700));
}

// Each unit of work attests the pass again as a walk does: a widened ancestor,
// session directory or held sidecar directory, and an ancestor replaced by a
// symlink to its moved original (every inode unchanged), end the pass, and the
// next reader refuses as a walk does.
void TestSessionPath::test_check_attests_again() {
  const std::string directory = m_session + "/" + intents;
  const std::string moved = m_root + "/moved";
  CPPUNIT_ASSERT_EQUAL(0, ::mkdir(directory.c_str(), 0700));
  const auto check_refuses = [&](const std::string& expected) {
    core::session_directory_check();
    const std::string message = counter_refusal(m_session);
    CPPUNIT_ASSERT_MESSAGE("Expected " + expected + ", got: " + message, message.find(expected) != std::string::npos);
  };
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0777));
  check_refuses("unsafe session path ancestor ownership or mode: " + m_outer + " ");
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_outer.c_str(), 0700));

  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0707));
  check_refuses("unsafe session directory ownership or mode: " + m_session + " ");
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0700));

  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(directory.c_str(), 0750));
  check_refuses("unsafe stop intent directory ownership or mode: " + directory + " ");
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(directory.c_str(), 0700));

  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::rename(m_outer.c_str(), moved.c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::symlink("moved", m_outer.c_str()));
  check_refuses("cannot open session path component: " + m_outer + ": ");
  CPPUNIT_ASSERT_EQUAL(0, ::unlink(m_outer.c_str()));
  CPPUNIT_ASSERT_EQUAL(0, ::rename(moved.c_str(), m_outer.c_str()));
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
}

// A held missing directory is trusted only while its name is still ENOENT: a
// session directory that lost its search permission ends the pass, and the
// reader refuses as a walk does instead of answering that no journal exists.
void TestSessionPath::test_missing_needs_enoent() {
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), stop_intent::counter(m_session, hash, false));
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0600));
  core::session_directory_check();
  const std::string message = counter_refusal(m_session);
  CPPUNIT_ASSERT_EQUAL(0, ::chmod(m_session.c_str(), 0700));
  CPPUNIT_ASSERT_MESSAGE("The reader must refuse: " + message,
                         message.find("cannot open stop intent directory: " + m_session + "/" + intents + ": " +
                                      std::strerror(EACCES)) != std::string::npos);
}
