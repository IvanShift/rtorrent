#!/usr/bin/env python3
"""Run the real metadata callback body against safe, non-deleting test doubles.

No daemon is executed. Retiring a double expires its lifetime token; later access
throws a normal exception instead of touching freed storage. This checks control
flow and Stop transfer, not libtorrent or allocator behavior.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

SOURCE = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2] / 'src/core/download_list.cc'
temporary_scratch = tempfile.TemporaryDirectory(prefix='rtorrent-meta-callback-') if len(sys.argv) < 3 else None
SCRATCH = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else pathlib.Path(temporary_scratch.name)
SCRATCH.mkdir(parents=True, exist_ok=True)
body = re.search(
    r"void\nDownloadList::process_meta_download\(Download\* download\) \{([\s\S]*?)\n\}\n\n\}",
    SOURCE.read_text(),
)
if body is None:
    raise SystemExit("metadata function boundary changed; review extraction")

PREFIX = r'''
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#define lt_log_print_info(...) ((void)0)
#define lt_log_print(...) ((void)0)
namespace torrent {
struct Object {
  int64_t value{};
  std::map<std::string, Object> keys;
  Object() = default;
  Object(int64_t number) : value(number) {}
  static Object create_map() { return Object(); }
  Object& insert_key(const std::string& key, const Object& object) { return keys[key] = object; }
  Object& get_key(const std::string& key) { return keys.at(key); }
  int64_t get_key_value(const std::string& key) const { return keys.at(key).value; }
  bool has_key(const std::string& key) const { return keys.count(key) != 0; }
};
std::istream& operator>>(std::istream& stream, Object&) { char c; return stream.get(c); }
}
namespace fixture {
enum class Mode {
  Normal, UserStopped, EraseView, EraseClose, StopClose, StopErase,
  RefuseErase, FailCreate, PendingMigration, OriginalClaim, UncommittedCandidate
};
Mode mode;
int state, generation, stale_access, replacement_start, replacements;
}
struct Path { std::string value; std::string str() const { return value; } };
struct File { Path path; Path frozen_path() const { return path; } };
class Download {
public:
  explicit Download(const std::string& path) : file{{path}}, files{&file} {
    root.insert_key("rtorrent_meta_download", torrent::Object::create_map()).insert_key("start", int64_t(1));
  }
  std::weak_ptr<void> lifetime() const { return live; }
  void retire() { live.reset(); }
  void ensure() const {
    if (!live) { ++fixture::stale_access; throw std::runtime_error("test double retired"); }
  }
  std::vector<File*>* file_list() { ensure(); return &files; }
  torrent::Object* bencode() { ensure(); return &root; }
  void set_message(const std::string&) { ensure(); }
private:
  std::shared_ptr<void> live{std::make_shared<char>()};
  torrent::Object root;
  File file;
  std::vector<File*> files;
};
namespace stop_intent {
struct ScopedSuppress { ~ScopedSuppress() {} };
uint64_t counter(const std::string&, const std::string&, bool) { return fixture::generation; }
void erase_counter(const std::string&, const std::string&) { fixture::generation = 0; }
}
bool retrackers_migration_pending(const std::string&) { return fixture::mode == fixture::Mode::PendingMigration; }
bool retrackers_migration_original_claim_active(const std::string&) { return fixture::mode == fixture::Mode::OriginalClaim; }
bool retrackers_migration_uncommitted_candidate(const std::string&) { return fixture::mode == fixture::Mode::UncommittedCandidate; }
namespace rpc {
Download* make_target(Download* value) { return value; }
void parse_command_single(Download* target, const char*) {
  target->ensure();
  fixture::state = 0;
  if (fixture::mode == fixture::Mode::EraseView) target->retire();
}
void call_command(const char*, const torrent::Object&, Download* target) {
  target->ensure();
  if (fixture::mode == fixture::Mode::EraseClose) target->retire();
  if (fixture::mode == fixture::Mode::StopClose) ++fixture::generation;
}
int64_t call_command_value(const char* command, Download* target) {
  target->ensure();
  if (std::string(command) == "d.state") return fixture::state;
  throw std::runtime_error("unmodeled value RPC");
}
std::string call_command_string(const char* command, Download* target = nullptr) {
  if (target) target->ensure();
  if (std::string(command) == "d.hash") return std::string(40, 'a');
  if (std::string(command) == "session.path") return "/test-session";
  throw std::runtime_error("unmodeled string RPC");
}
}
struct Manager {
  void try_create_download_from_meta_download(std::unique_ptr<torrent::Object> root, const std::string&) {
    if (fixture::mode == fixture::Mode::FailCreate) throw std::runtime_error("benign factory failure");
    ++fixture::replacements;
    fixture::replacement_start = root->get_key("rtorrent_meta_download").get_key_value("start");
  }
};
struct Control { Manager manager; Manager* core() { return &manager; } } instance;
Control* control = &instance;
class DownloadList {
public:
  void process_meta_download(Download* download);
  void erase_meta_placeholder(Download* download) { erase_ptr(download); }
  void erase_ptr(Download* download) {
    if (fixture::mode == fixture::Mode::RefuseErase) throw std::runtime_error("benign lease refusal");
    if (fixture::mode == fixture::Mode::StopErase) ++fixture::generation;
    download->retire();
  }
};
'''

SUFFIX = r'''
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::ofstream(argv[1]) << "metadata";
  struct Case { const char* name; fixture::Mode mode; int start; int replacements; int final_generation = 0; };
  const std::vector<Case> cases{
    {"normal-conversion", fixture::Mode::Normal, 1, 1},
    {"explicit-Stop-kept", fixture::Mode::UserStopped, 0, 1},
    {"retired-by-view-callback", fixture::Mode::EraseView, -1, 0},
    {"retired-by-close-callback", fixture::Mode::EraseClose, -1, 0},
    {"Stop-in-close-callback", fixture::Mode::StopClose, 0, 1},
    {"Stop-in-erase-callback", fixture::Mode::StopErase, 0, 1},
    {"lease-refusal-contained", fixture::Mode::RefuseErase, -1, 0},
    {"factory-exception-after-erase-contained", fixture::Mode::FailCreate, -1, 0},
    {"pending-migration-counter-retained", fixture::Mode::PendingMigration, 1, 1, 5},
    {"original-claim-counter-retained", fixture::Mode::OriginalClaim, 1, 1, 5},
    {"uncommitted-candidate-counter-retained", fixture::Mode::UncommittedCandidate, 1, 1, 5},
  };
  unsigned failures = 0;
  for (const auto& item : cases) {
    fixture::mode = item.mode;
    fixture::state = item.mode == fixture::Mode::UserStopped ? 0 : 1;
    fixture::generation = item.final_generation ? item.final_generation : item.mode == fixture::Mode::UserStopped ? 1 : 0;
    fixture::stale_access = fixture::replacements = 0;
    fixture::replacement_start = -1;
    Download download(argv[1]);
    DownloadList list;
    list.process_meta_download(&download);
    const bool passed = fixture::stale_access == 0 && fixture::replacements == item.replacements &&
        fixture::replacement_start == item.start && fixture::generation == item.final_generation;
    std::cout << item.name << ": " << (passed ? "PASS" : "FAIL")
      << " stale_access=" << fixture::stale_access << " replacements=" << fixture::replacements
      << " start=" << fixture::replacement_start << " generation=" << fixture::generation << '\n';
    failures += !passed;
  }
  return failures ? 1 : 0;
}
'''

cpp = SCRATCH / "metadata.cc"
cpp.write_text(PREFIX + "\nvoid\nDownloadList::process_meta_download(Download* download) {" + body[1] + "\n}\n" + SUFFIX)
binary = SCRATCH / "metadata-test"
subprocess.run(
    ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(binary)],
    check=True,
    timeout=30,
)
result = subprocess.run([str(binary), str(SCRATCH / "metadata.fixture")], timeout=10, check=False)
raise SystemExit(result.returncode)
