#include "config.h"

#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <dirent.h>
#include <torrent/object_stream.h>

#include <cstdio>
#include <functional>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <netdb.h>
#include <unistd.h>
#include <fnmatch.h>
#include <torrent/rate.h>
#include <torrent/throttle.h>
#include <torrent/tracker/tracker.h>
#include <torrent/data/download_data.h>
#include <torrent/data/file.h>
#include <torrent/data/file_list.h>
#include <torrent/download/resource_manager.h>
#include <torrent/net/resolver.h>
#include <torrent/net/types.h>
#include <torrent/peer/connection_list.h>
#include <torrent/peer/peer_list.h>
#include <torrent/system/callbacks.h>
#include <torrent/utils/file_stat.h>
#include <torrent/utils/log.h>
#include <torrent/utils/option_strings.h>
#include <torrent/utils/string_manip.h>

#include "core/download.h"
#include "core/incarnation.h"
#include "core/download_factory.h"
#include "core/retrackers_migration_limits.h"
#include "core/retrackers_migration_creation.h"
#include "core/retrackers_migration_pending.h"
#include "core/manager.h"
#include "rpc/parse.h"
#include "session/session_manager.h"
#include "session/download_storer.h"

#include "globals.h"
#include "control.h"
#include "command_helpers.h"
#include "stop_intent.h"

torrent::string_utf8
retrieve_d_base_path(core::Download* download) {
  if (download->file_list()->is_multi_file())
    return download->file_list()->frozen_root_dir();

  if (download->file_list()->empty())
    return {};

  return download->file_list()->at(0)->frozen_path();
}

torrent::string_utf8
retrieve_d_base_filename(core::Download* download) {
  torrent::string_utf8 base_path;

  if (download->file_list()->is_multi_file())
    base_path = download->file_list()->frozen_root_dir();
  else if (!download->file_list()->empty())
    base_path = download->file_list()->at(0)->frozen_path();
  else
    return {};

  auto split = base_path.str().rfind('/');

  if (split == std::string::npos)
    return base_path;

  return torrent::string_utf8::from_string(base_path.str().substr(split + 1));
}

torrent::Object
apply_d_change_link(core::Download* download, const torrent::Object::list_type& args, int changeType) {
  if (args.size() != 3)
    throw torrent::input_error("Wrong argument count.");

  torrent::Object::list_const_iterator itr = args.begin();

  const std::string& type    = (itr++)->as_string();
  const std::string& prefix  = (itr++)->as_string();
  const std::string& postfix = (itr++)->as_string();

  if (type.empty())
    throw torrent::input_error("Invalid arguments.");

  std::string target;
  std::string link;

  if (type == "base_path") {
    target = rpc::call_command_string("d.base_path", rpc::make_target(download));
    link   = expand_path(prefix + rpc::call_command_string("d.base_path", rpc::make_target(download)) + postfix);

  } else if (type == "base_filename") {
    target = rpc::call_command_string("d.base_path", rpc::make_target(download));
    link   = expand_path(prefix + rpc::call_command_string("d.base_filename", rpc::make_target(download)) + postfix);

//   } else if (type == "directory_path") {
//     target = rpc::call_command_string("d.directory", rpc::make_target(download));
//     link = path_expand(prefix + rpc::call_command_string("d.base_path", rpc::make_target(download)) + postfix);

  } else if (type == "tied") {
    link = expand_path(rpc::call_command_string("d.tied_to_file", rpc::make_target(download)));

    if (link.empty())
      return torrent::Object();

    link = expand_path(prefix + link + postfix);
    target = rpc::call_command_string("d.base_path", rpc::make_target(download));

  } else {
    throw torrent::input_error("Unknown type argument.");
  }

  switch (changeType) {
  case 0:
    if (symlink(target.c_str(), link.c_str()) == -1)
      lt_log_print(torrent::LOG_TORRENT_WARN, "create_link failed: %s", std::strerror(errno));

    break;

  case 1:
  {
    torrent::utils::FileStat fileStat;
    errno = 0;

    if (!fileStat.update_link(link) || !fileStat.is_link() || unlink(link.c_str()) == -1)
      lt_log_print(torrent::LOG_TORRENT_WARN, "delete_link failed: %s", std::strerror(errno));

    break;
  }
  default:
    break;
  }

  return torrent::Object();
}

torrent::Object
apply_d_delete_tied(core::Download* download) {
  const std::string& tie = rpc::call_command_string("d.tied_to_file", rpc::make_target(download));

  if (tie.empty())
    return torrent::Object();

  if (::unlink(expand_path(tie).c_str()) == -1)
    control->core()->push_log_std("Could not unlink tied file: " + std::string(std::strerror(errno)));

  rpc::call_command("d.tied_to_file.set", std::string(), rpc::make_target(download));
  return torrent::Object();
}

torrent::Object
apply_d_connection_type(core::Download* download, const std::string& name) {
  torrent::Download::ConnectionType t =
    (torrent::Download::ConnectionType)torrent::option_find_string(torrent::OPTION_CONNECTION_TYPE, name.c_str());

  download->download()->set_connection_type(t);
  return torrent::Object();
}

torrent::Object
apply_d_choke_heuristics(core::Download* download, const std::string& name, bool is_down) {
  torrent::heuristics_enum t =
    static_cast<torrent::heuristics_enum>(torrent::option_find_string(torrent::OPTION_CHOKE_HEURISTICS, name.c_str()));

  if (is_down)
    download->download()->set_download_choke_heuristic(t);
  else
    download->download()->set_upload_choke_heuristic(t);

  return torrent::Object();
}

const char*
retrieve_d_priority_str(core::Download* download) {
  switch (download->priority()) {
  case 0:
    return "off";
  case 1:
    return "low";
  case 2:
    return "normal";
  case 3:
    return "high";
  default:
    throw torrent::input_error("Priority out of range.");
  }
}

torrent::Object
retrieve_d_ratio(core::Download* download) {
  if (download->is_hash_checking())
    return int64_t();

  int64_t bytesDone = download->download()->bytes_done();
  int64_t upTotal   = download->info()->up_rate()->total();

  return bytesDone > 0 ? (1000 * upTotal) / bytesDone : 0;
}

torrent::Object
apply_d_custom(core::Download* download, const torrent::Object::list_type& args) {
  torrent::Object::list_const_iterator itr = args.begin();

  if (itr == args.end())
    throw torrent::bencode_error("Missing key argument.");

  const std::string& key = itr->as_string();
  if (stop_intent::checker_marker_key(key) && !stop_intent::held(download).empty())
    throw torrent::input_error("stop claim: checker marker write refused during file operation");

  if (++itr == args.end())
    throw torrent::bencode_error("Missing value argument.");

  if (stop_intent::checker_marker_key(key)) {
    const std::string hash = rpc::call_command_string("d.hash", rpc::make_target(download));
    const bool native_candidate = core::RetrackersMigrationCreation::in_scope(download) &&
        retrackers_migration_target_source(hash) == hash &&
        retrackers_migration_pending(hash);
    if (retrackers_migration_original_claim_active(hash) && !native_candidate)
      throw torrent::input_error("RR original claim protects checker marker; use native finish.");
  }

  download->bencode()->get_key("rtorrent").
                       insert_preserve_copy("custom", torrent::Object::create_map()).first->second.
                       insert_key(key, itr->as_string());
  return torrent::Object();
}

torrent::Object
retrieve_d_custom(core::Download* download, const std::string& key) {
  try {
    return download->bencode()->get_key("rtorrent").get_key("custom").get_key_string(key);

  } catch (torrent::bencode_error& e) {
    return std::string();
  }
}

torrent::Object
retrieve_d_custom_throw(core::Download* download, const std::string& key) {
  try {
    return download->bencode()->get_key("rtorrent").get_key("custom").get_key_string(key);

  } catch (torrent::bencode_error& e) {
    throw torrent::input_error("No such custom value.");
  }
}

torrent::Object
retrieve_d_custom_if_z(core::Download* download, const torrent::Object::list_type& args) {
  torrent::Object::list_const_iterator itr = args.begin();
  if (itr == args.end())
    throw torrent::bencode_error("d.custom.if_z: Missing key argument.");
  const std::string& key = (itr++)->as_string();
  if (key.empty())
    throw torrent::bencode_error("d.custom.if_z: Empty key argument.");
  if (itr == args.end())
    throw torrent::bencode_error("d.custom.if_z: Missing default argument.");

  try {
    const std::string& val = download->bencode()->get_key("rtorrent").get_key("custom").get_key_string(key);
    return val.empty() ? itr->as_string() : val;
  } catch (torrent::bencode_error& e) {
    return itr->as_string();
  }
}

torrent::Object
retrieve_d_custom_map(core::Download* download, bool keys_only, const torrent::Object::list_type& args) {
  if (args.begin() != args.end())
    throw torrent::bencode_error("d.custom.keys/items takes no arguments.");

  torrent::Object result = keys_only ? torrent::Object::create_list() : torrent::Object::create_map();

  for (const auto& entry : download->bencode()->get_key("rtorrent").get_key("custom").as_map())
    if (keys_only)
      result.as_list().push_back(entry.first);
    else
      result.as_map()[entry.first] = entry.second;

  return result;
}

void
apply_d_add_peer(core::Download* download, const std::string& arg) {
  int port, ret;
  char dummy;
  char host[1024];

  if (download->download()->info()->is_private())
    throw torrent::input_error("Download is private.");

  ret = std::sscanf(arg.c_str(), "[%64[^]]]:%i%c", host, &port, &dummy);

  if (ret < 1)
    ret = std::sscanf(arg.c_str(), "%1023[^:]:%i%c", host, &port, &dummy);

  if (ret == 1)
    port = 6881;
  else if (ret != 2)
    throw torrent::input_error("Could not parse host.");

  if (port < 1 || port > 65535)
    throw torrent::input_error("Invalid port number.");

  assert(std::this_thread::get_id() == torrent::main_thread::thread_id());

  auto callback_id = torrent::system::make_callback_id();

  // Currently discarding SOCK_STREAM.
  torrent::this_thread::resolver()->resolve_preferred(callback_id, host, AF_UNSPEC, AF_INET, [download, port](torrent::c_sa_shared_ptr sa, int err) {
      if (sa == nullptr) {
        lt_log_print(torrent::LOG_TORRENT_WARN, "could not resolve hostname for added peer: %s", gai_strerror(err));
        return;
      }

      download->download()->add_peer(sa.get(), port);
  });
}

torrent::Object
d_chunks_seen(core::Download* download) {
  const uint8_t* seen = download->download()->chunks_seen();

  if (seen == NULL)
    return std::string();

  uint32_t size = download->download()->file_list()->size_chunks();

  std::string result;
  result.resize(size * 2);

  torrent::utils::transform_to_hex(seen, seen + size, result);

  return result;
}

torrent::Object
f_multicall(core::Download* download, const torrent::Object::list_type& args) {
  if (args.empty())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.
  torrent::Object             resultRaw = torrent::Object::create_list();
  torrent::Object::list_type& result = resultRaw.as_list();
  std::vector<std::string>    regex_list;

  bool use_regex = true;

  if (args.front().is_list())
    for (const auto& o : args.front().as_list())
      regex_list.push_back(o.as_string_c());
  else if (args.front().is_string() && !args.front().as_string().empty())
    regex_list.push_back(args.front().as_string());
  else
    use_regex = false;

  for (const auto& file : *download->file_list()) {
    if (use_regex &&
        std::none_of(regex_list.begin(), regex_list.end(), [&file](const auto& pattern) { return fnmatch(pattern.c_str(), file->path()->as_string().c_str(), 0) == 0; }))
      continue;

    torrent::Object::list_type& row = result.insert(result.end(), torrent::Object::create_list())->as_list();

    for (torrent::Object::list_const_iterator cItr = ++args.begin(); cItr != args.end(); cItr++) {
      const std::string& cmd = cItr->as_string();
      row.push_back(rpc::parse_command(rpc::make_target(file.get()), cmd.c_str(), cmd.c_str() + cmd.size()).first);
    }
  }

  return resultRaw;
}

torrent::Object
t_multicall(core::Download* download, const torrent::Object::list_type& args) {
  if (args.empty())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.

  auto  result_raw = torrent::Object::create_list();
  auto& result     = result_raw.as_list();

  for (uint32_t idx = 0, last = download->tracker_list_size(); idx < last; idx++) {
    auto& row     = result.insert(result.end(), torrent::Object::create_list())->as_list();
    auto  tracker = download->tracker_controller().at(idx);

    if (!tracker.is_valid())
      continue;

    for (auto cItr = ++args.begin(); cItr != args.end(); cItr++) {
      auto& cmd = cItr->as_string();

      row.push_back(rpc::parse_command(rpc::make_target(&tracker), cmd.c_str(), cmd.c_str() + cmd.size()).first);
    }
  }

  return result_raw;
}

torrent::Object
p_multicall(core::Download* download, const torrent::Object::list_type& args) {
  if (args.empty())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.
  auto  resultRaw = torrent::Object::create_list();
  auto& result    = resultRaw.as_list();

  auto*      connection_list = download->connection_list();
  const auto change_counter  = connection_list->change_counter();

  for (const auto& connection : *connection_list) {
    torrent::Object::list_type& row = result.insert(result.end(), torrent::Object::create_list())->as_list();

    for (auto cItr = ++args.begin(); cItr != args.end(); cItr++) {
      const std::string& cmd = cItr->as_string();

      row.push_back(rpc::parse_command(rpc::make_target(connection), cmd.c_str(), cmd.c_str() + cmd.size()).first);

      // Erasing a peer frees it and swaps the last element into its place, so
      // neither this peer nor the iteration survives a change to the list.
      if (connection_list->change_counter() != change_counter)
        throw torrent::input_error("Command changed the connection list during p.multicall.");
    }
  }

  return resultRaw;
}

torrent::Object
p_call_target(const torrent::Object::list_type& args) {
  if (args.empty() || args.begin() + 1 == args.end() || args.begin() + 2 == args.end())
    throw torrent::input_error("Too few arguments.");

  // We ignore the first arg for now, but it will be used for
  // selecting what files to include.

  // Add some pre-parsing of the commands, so we don't spend time
  // parsing and searching command map for every single call.
  torrent::Object::list_const_iterator itr = args.begin();

  core::Download* download = control->core()->download_list()->find_hex_ptr(itr++->as_string().c_str());
  const std::string& peer_id = itr++->as_string();
  const std::string& command_key = itr++->as_string();

  if (download == nullptr)
    throw torrent::input_error("invalid parameters: info-hash not found");

  torrent::HashString hash;

  if (peer_id.size() != 40)
    throw torrent::input_error("invalid argument: peer id target is not 40 bytes long");

  if (torrent::utils::transform_from_hex(peer_id.c_str(), peer_id.c_str() + 40, hash) != hash.end())
    throw torrent::input_error("invalid argument: peer id target is not a hex string");

  torrent::ConnectionList::iterator peerItr = download->connection_list()->find(hash.c_str());

  if (peerItr == download->connection_list()->end())
    throw torrent::input_error("Could not find peer.");

  if (itr == args.end())
    return rpc::commands.call(command_key.c_str());

  if (itr + 1 == args.end())
    return rpc::commands.call(command_key.c_str(), *itr);

  return rpc::commands.call(command_key.c_str(), torrent::Object::create_list_range(itr, args.end()));
}

torrent::Object
download_tracker_insert(core::Download* download, const torrent::Object::list_type& args) {
  if (args.size() != 2)
    throw torrent::input_error("Wrong argument count.");

  int64_t group = 0;

  if (args.front().is_string()) {
    if (!rpc::parse_whole_value_nothrow(args.front().as_string().c_str(), &group))
      throw torrent::input_error("Invalid tracker group number.");
  } else {
    group = args.front().as_value();
  }

  if (group < 0 || group > 32)
    throw torrent::input_error("Tracker group number invalid.");

  download->tracker_controller().add_extra_tracker(group, args.back().as_string());

  return torrent::Object();
}

//
// New download commands and macros:
//

torrent::Object&
download_get_variable(core::Download* download, const char* first_key, const char* second_key = NULL) {
  if (second_key == NULL)
    return download->bencode()->get_key(first_key);

  return download->bencode()->get_key(first_key).get_key(second_key);
}

torrent::Object
download_get_value_or_zero(core::Download* download, const char* first_key, const char* second_key = NULL) {
  auto object = download_get_variable(download, first_key, second_key);

  if (object.is_empty())
    return int64_t(0);

  if (!object.is_value())
    throw torrent::bencode_error("Download variable is not a value.");

  return object;
}

torrent::Object
download_set_variable(core::Download* download, const torrent::Object& rawArgs, const char* first_key, const char* second_key = NULL) {
  if (second_key == NULL)
    return download->bencode()->get_key(first_key) = torrent::object_create_normal(rawArgs);

  return download->bencode()->get_key(first_key).get_key(second_key) = torrent::object_create_normal(rawArgs);
}

torrent::Object
download_set_variable_value(core::Download* download, const torrent::Object::value_type& args,
                            const char* first_key, const char* second_key = NULL) {
  if (second_key == NULL)
    return download->bencode()->get_key(first_key) = args;

  return download->bencode()->get_key(first_key).get_key(second_key) = args;
}

torrent::Object
download_set_variable_value_ifz(core::Download* download, const torrent::Object::value_type& args,
                                const char* first_key, const char* second_key = NULL) {
  torrent::Object& object = second_key == NULL ?
    download->bencode()->get_key(first_key) :
    download->bencode()->get_key(first_key).get_key(second_key);

  if (object.as_value() == 0)
    object = args;

  return object;
}

torrent::Object
download_set_variable_string(core::Download* download, const torrent::Object::string_type& args,
                             const char* first_key, const char* second_key = NULL) {
  if (second_key == NULL)
    return download->bencode()->get_key(first_key) = args;

  return download->bencode()->get_key(first_key).get_key(second_key) = args;
}

//
//
//

torrent::Object
d_list_push_back_string(core::Download* download, const std::string& arg, const char* first_key, const char* second_key) {
  download_get_variable(download, first_key, second_key).as_list().push_back(arg);
  return torrent::Object();
}

torrent::Object
d_list_push_back_unique_string(core::Download* download, const std::string& arg, const char* first_key, const char* second_key) {
  torrent::Object::list_type& list = download_get_variable(download, first_key, second_key).as_list();

  if (std::none_of(list.begin(), list.end(), [arg](const torrent::Object& obj) { return torrent::object_equal(obj, arg); }))
    list.push_back(arg);

  return torrent::Object();
}

torrent::Object
d_list_has(core::Download* download, const torrent::Object& rawArgs, const char* first_key, const char* second_key) {
  const torrent::Object& args = (rawArgs.is_list() && !rawArgs.as_list().empty()) ? rawArgs.as_list().front() : rawArgs;
  torrent::Object::list_type& list = download_get_variable(download, first_key, second_key).as_list();

  return (int64_t)(std::any_of(list.begin(), list.end(), [args](const auto& obj) { return torrent::object_equal(obj, args); }));
}

torrent::Object
d_list_remove(core::Download* download, const torrent::Object& rawArgs, const char* first_key, const char* second_key) {
  const torrent::Object& args = (rawArgs.is_list() && !rawArgs.as_list().empty()) ? rawArgs.as_list().front() : rawArgs;
  torrent::Object::list_type& list = download_get_variable(download, first_key, second_key).as_list();

  list.erase(std::remove_if(list.begin(), list.end(), [args](const torrent::Object& obj) { return torrent::object_equal(obj, args); }), list.end());

  return torrent::Object();
}

#define CMD2_ON_INFO(func) std::bind(&torrent::DownloadInfo::func, std::bind(&core::Download::info, std::placeholders::_1))
#define CMD2_ON_DATA(func) std::bind(&torrent::download_data::func, std::bind(&core::Download::data, std::placeholders::_1))
#define CMD2_ON_DL(func) std::bind(&torrent::Download::func, std::bind(&core::Download::download, std::placeholders::_1))
#define CMD2_ON_FL(func) std::bind(&torrent::FileList::func, std::bind(&core::Download::file_list, std::placeholders::_1))

#define CMD2_BIND_DL std::bind(&core::Download::download, std::placeholders::_1)
#define CMD2_BIND_CL std::bind(&core::Download::connection_list, std::placeholders::_1)
#define CMD2_BIND_FL std::bind(&core::Download::file_list, std::placeholders::_1)
#define CMD2_BIND_PL std::bind(&core::Download::c_peer_list, std::placeholders::_1)
#define CMD2_BIND_TC std::bind(&core::Download::tracker_controller, std::placeholders::_1)

#define CMD2_BIND_INFO std::bind(&core::Download::info, std::placeholders::_1)
#define CMD2_BIND_DATA std::bind(&core::Download::data, std::placeholders::_1)

#define CMD2_DL_VAR_VALUE(key, first_key, second_key)                   \
  CMD2_DL(key, std::bind(&download_get_variable, std::placeholders::_1, first_key, second_key)); \
  CMD2_DL_VALUE_P(key ".set", std::bind(&download_set_variable_value, \
                                             std::placeholders::_1, std::placeholders::_2, \
                                             first_key, second_key));

#define CMD2_DL_VAR_VALUE_PUBLIC(key, first_key, second_key)            \
  CMD2_DL(key, std::bind(&download_get_variable, std::placeholders::_1, first_key, second_key)); \
  CMD2_DL_VALUE(key ".set", std::bind(&download_set_variable_value, \
                                           std::placeholders::_1, std::placeholders::_2, \
                                           first_key, second_key));

#define CMD2_DL_TIMESTAMP(key, first_key, second_key)                   \
  CMD2_DL(key, std::bind(&download_get_variable, std::placeholders::_1, first_key, second_key)); \
  CMD2_DL_VALUE_P(key ".set", std::bind(&download_set_variable_value,   \
                                        std::placeholders::_1, std::placeholders::_2, \
                                        first_key, second_key));        \
  CMD2_DL_VALUE_P(key ".set_if_z", std::bind(&download_set_variable_value_ifz, \
                                             std::placeholders::_1, std::placeholders::_2, \
                                             first_key, second_key));   \
  CMD2_DL(key ".or_zero", [](core::Download* download, auto) {          \
      return download_get_value_or_zero(download, first_key, second_key); }); \
  CMD2_DL_VALUE(key ".elapsed", [](core::Download* download, auto value) { \
      return download_get_value_or_zero(download, first_key, second_key).as_value() > value; });

#define CMD2_DL_VAR_STRING(key, first_key, second_key)                   \
  CMD2_DL(key, std::bind(&download_get_variable, std::placeholders::_1, first_key, second_key)); \
  CMD2_DL_STRING_P(key ".set", std::bind(&download_set_variable_string, \
                                              std::placeholders::_1, std::placeholders::_2, \
                                              first_key, second_key));

#define CMD2_DL_VAR_STRING_PUBLIC(key, first_key, second_key)                   \
  CMD2_DL(key, std::bind(&download_get_variable, std::placeholders::_1, first_key, second_key)); \
  CMD2_DL_STRING(key ".set", std::bind(&download_set_variable_string, \
                                            std::placeholders::_1, std::placeholders::_2, \
                                            first_key, second_key));

int64_t            cg_d_group(core::Download* download);
const std::string& cg_d_group_name(core::Download* download);
void               cg_d_group_set(core::Download* download, const torrent::Object& arg);


thread_local std::string rr_native_capture_erase_hash;
thread_local std::string rr_native_release_start_hash;

// The private journal is the durable authority until same-hash release and finalization.
namespace {
struct RrMigrationFile {
  std::string path;
  int64_t size;
  int64_t priority;
};
bool rr_lower_hex(const std::string& value, size_t length) {
  return value.size() == length && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool rr_original_marker(const std::string& marker, const std::string& local_id) {
  const std::string prefix = "v1:original:";
  const size_t state = prefix.size();
  return marker.size() == state + 1 + 1 + 40 + 1 + 64 &&
      marker.compare(0, state, prefix) == 0 &&
      (marker[state] == '0' || marker[state] == '1') && marker[state + 1] == ':' &&
      marker.compare(state + 2, 40, local_id) == 0 && marker[state + 42] == ':' &&
      rr_lower_hex(marker.substr(state + 43), 64);
}

struct RrOriginalClaim {
  std::string hash;
  std::string local_id;
  std::string incarnation;
  std::string tx;
  std::string user_digest;
  std::string run;
  uint64_t stop_generation = 0;
  std::string baseline_digest;
};

std::string rr_original_v2_marker(const RrOriginalClaim& claim);
bool rr_original_claim_read(const std::string& hash, RrOriginalClaim* claim);
bool rr_journal_exists(const std::string& name,
                       const char* error_message = "RR journal presence cannot be checked.");
uint64_t rr_late_save_to_prefix(core::Download* download, const std::string& prefix,
                                const std::string& hash, const std::string& incarnation);

bool rr_v2_original_marker(const std::string& marker,
                           const std::string& local_id,
                           const std::string& tx) {
  const std::string prefix = "v2:original:";
  const size_t start = prefix.size();
  return marker.size() == start + 1 + 1 + 40 + 1 + 64 + 1 + 32 &&
      marker.compare(0, start, prefix) == 0 &&
      (marker[start] == '0' || marker[start] == '1') && marker[start + 1] == ':' &&
      marker.compare(start + 2, 40, local_id) == 0 && marker[start + 42] == ':' &&
      rr_lower_hex(marker.substr(start + 43, 64), 64) &&
      marker[start + 107] == ':' && marker.substr(start + 108) == tx;
}

struct RrMigrationCapsule {
  std::string hash;
  std::string source_local_id;
  std::string source_incarnation;
  std::string expected_new_hash;
  std::string claim;
  std::string tx;
  std::string candidate_sha256;
  std::vector<std::pair<std::string, int64_t>> limits;
  std::vector<RrMigrationFile> files;
  torrent::Object views;
  int64_t up_total;
  int64_t down_total;
  int64_t state_counter;
  int64_t stop_generation;
};

bool rr_capsule_v2(const RrMigrationCapsule& state) {
  return !state.tx.empty() &&
      rr_v2_original_marker(state.claim, state.source_local_id, state.tx);
}

std::string rr_candidate_claim_marker(const RrMigrationCapsule& state) {
  return std::string(rr_capsule_v2(state) ? "v2:candidate-claim:" :
                     "v1:candidate-claim:") + state.tx;
}

std::string rr_candidate_ready_marker(const RrMigrationCapsule& state) {
  return std::string(rr_capsule_v2(state) ? "v2:candidate-ready:" :
                     "v1:candidate-ready:") + state.tx;
}

// Prototype journal is daemon-owned and keyed only by the daemon's info hash.
// An outstanding journal needs explicit reconciliation before a production API can use it.
int rr_migration_dir() {
  const std::string path = rpc::call_command_string("session.path");
  if (path.empty())
    throw torrent::input_error("RR migration requires a session directory.");
  const int session = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0)
    throw torrent::input_error("RR migration cannot open session directory.");
  const int made = ::mkdirat(session, ".retrackers-migration", 0700);
  if (made < 0 && errno != EEXIST) {
    ::close(session);
    throw torrent::input_error("RR migration cannot create journal directory.");
  }
  if (made == 0 && ::fsync(session) < 0) {
    ::close(session);
    throw torrent::input_error("RR migration journal parent fsync failed; source retained.");
  }
  const int dir = ::openat(session, ".retrackers-migration", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  ::close(session);
  struct stat st;
  if (dir < 0 || ::fstat(dir, &st) < 0 || st.st_uid != ::geteuid() ||
      (st.st_mode & 0777) != 0700) {
    if (dir >= 0) ::close(dir);
    throw torrent::input_error("RR migration journal directory has unsafe ownership or mode.");
  }
  return dir;
}

torrent::Object rr_migration_encode(const RrMigrationCapsule& state) {
  torrent::Object root = torrent::Object::create_map();
  root.insert_key("version", int64_t(3));
  root.insert_key("hash", state.hash);
  root.insert_key("source_local_id", state.source_local_id);
  root.insert_key("source_incarnation", state.source_incarnation);
  root.insert_key("expected_new_hash", state.expected_new_hash);
  root.insert_key("claim", state.claim);
  root.insert_key("tx", state.tx);
  root.insert_key("candidate_sha256", state.candidate_sha256);
  root.insert_key("up_total", state.up_total);
  root.insert_key("down_total", state.down_total);
  root.insert_key("state_counter", state.state_counter);
  root.insert_key("stop_generation", state.stop_generation);
  root.insert_key("views", state.views);
  auto& limits = root.insert_key("limits", torrent::Object::create_map());
  for (const auto& setting : state.limits)
    limits.insert_key(setting.first, setting.second);
  auto& files = root.insert_key("files", torrent::Object::create_list()).as_list();
  for (const auto& file : state.files) {
    torrent::Object item = torrent::Object::create_map();
    item.insert_key("path", file.path);
    item.insert_key("size", file.size);
    item.insert_key("priority", file.priority);
    files.push_back(std::move(item));
  }
  return root;
}

std::string rr_rollback_untie(const std::string& bytes) {
  torrent::Object rtorrent;
  try {
    if (torrent::object_read_bencode_c(bytes.data(), bytes.data() + bytes.size(), &rtorrent) !=
        bytes.data() + bytes.size() || !rtorrent.is_map())
      throw torrent::input_error("RR rollback rtorrent sidecar invalid; source retained.");
  } catch (const torrent::bencode_error&) {
    throw torrent::input_error("RR rollback rtorrent sidecar invalid; source retained.");
  }
  rtorrent.insert_key("tied_to_file", std::string());
  std::ostringstream stream;
  torrent::object_write_bencode(&stream, &rtorrent, 0);
  if (!stream.good())
    throw torrent::input_error("RR rollback rtorrent sidecar serialization failed; source retained.");
  return stream.str();
}

std::vector<std::pair<std::string, std::string>> rr_migration_snapshot(core::Download* source) {
  if (!source->bencode()->has_key_map("info"))
    throw torrent::input_error("RR rollback requires complete source metainfo; source retained.");
  session::DownloadStorer storer(source);
  storer.build_full_streams();
  auto metainfo = storer.torrent_stream();
  auto rtorrent = storer.rtorrent_stream();
  auto resume = storer.libtorrent_stream();
  return {{".rollback.torrent", metainfo->str()},
          {".rollback.torrent.rtorrent", rr_rollback_untie(rtorrent->str())},
          {".rollback.torrent.libtorrent_resume", resume->str()}};
}

void rr_migration_publish(const RrMigrationCapsule& state, core::Download* source) {
  auto encoded = rr_migration_encode(state);
  std::ostringstream stream;
  torrent::object_write_bencode(&stream, &encoded, 0);
  const std::string bytes = stream.str();
  if (!stream.good() || bytes.empty() || bytes.size() > 4 * 1024 * 1024)
    throw torrent::input_error("RR migration journal exceeds size limit.");
  auto files = rr_migration_snapshot(source);
  files.emplace_back(".benc", bytes);
  files.emplace_back(".backup.benc", bytes);
  files.emplace_back(".target", state.hash + "\n");
  size_t total = 0;
  for (const auto& file : files) {
    total += file.second.size();
    if (file.second.empty() || file.second.size() > 64 * 1024 * 1024 || total > 128 * 1024 * 1024)
      throw torrent::input_error("RR rollback snapshot exceeds size limit; source retained.");
  }
  const int dir = rr_migration_dir();
  bool okay = true;
  for (const auto& file : files) {
    const std::string final_name = (file.first == ".target" ? state.expected_new_hash : state.hash) + file.first;
    const std::string temp_name = "." + final_name + ".tmp." + std::to_string(::getpid());
    const int fd = ::openat(dir, temp_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { okay = false; break; }
    for (size_t offset = 0; okay && offset < file.second.size();) {
      const ssize_t n = ::write(fd, file.second.data() + offset, file.second.size() - offset);
      if (n <= 0) okay = false;
      else offset += n;
    }
    if (::fsync(fd) < 0) okay = false;
    if (::close(fd) < 0) okay = false;
    if (okay && ::linkat(dir, temp_name.c_str(), dir, final_name.c_str(), 0) < 0)
      okay = false;
    ::unlinkat(dir, temp_name.c_str(), 0);
    if (!okay) break;
  }
  if (okay && ::fsync(dir) < 0) okay = false;
  ::close(dir);
  if (!okay)
    throw torrent::input_error("RR migration snapshot publication failed; source retained.");
}

std::string rr_migration_read_bytes(const std::string& filename, size_t max_size) {
  const int dir = rr_migration_dir();
  const int fd = ::openat(dir, filename.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  ::close(dir);
  struct stat st;
  if (fd < 0 || ::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0600 ||
      st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > max_size) {
    if (fd >= 0) ::close(fd);
    throw torrent::input_error("RR migration snapshot is missing or unsafe.");
  }
  std::string bytes(static_cast<size_t>(st.st_size), '\0');
  bool okay = true;
  for (size_t offset = 0; okay && offset < bytes.size();) {
    const ssize_t n = ::read(fd, &bytes[offset], bytes.size() - offset);
    if (n <= 0) okay = false;
    else offset += n;
  }
  ::close(fd);
  if (!okay)
    throw torrent::input_error("RR migration snapshot read failed.");
  return bytes;
}


void rr_migration_write_file(int dir, const std::string& name, const std::string& bytes) {
  static unsigned long sequence = 0;
  std::string temporary;
  int fd = -1;
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    temporary = "." + name + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++sequence);
    fd = ::openat(dir, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0)
    throw torrent::input_error("RR migration durable write could not create temporary file.");
  bool okay = true;
  for (size_t offset = 0; okay && offset < bytes.size();) {
    const ssize_t n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n <= 0) okay = false;
    else offset += n;
  }
  if (::fsync(fd) < 0) okay = false;
  if (::close(fd) < 0) okay = false;
  if (okay && ::renameat(dir, temporary.c_str(), dir, name.c_str()) < 0) okay = false;
  ::unlinkat(dir, temporary.c_str(), 0);
  if (!okay)
    throw torrent::input_error("RR migration durable write failed.");
}

bool rr_quiesced_read(const RrMigrationCapsule& state, uint64_t* generation) {
  const int dir = rr_migration_dir();
  struct stat st;
  const int found = ::fstatat(dir, (state.hash + ".quiesced.receipt").c_str(),
                              &st, AT_SYMLINK_NOFOLLOW);
  const int error = errno;
  ::close(dir);
  if (found < 0 && error == ENOENT) return false;
  if (found < 0) throw torrent::input_error("RR quiesced receipt presence unavailable.");
  const std::string bytes = rr_migration_read_bytes(state.hash + ".quiesced.receipt", 256);
  const std::string prefix = "quiesced:" + state.hash + ":" + state.tx + ":" +
      state.source_incarnation + ":" + std::to_string(state.stop_generation) + ":";
  if (!rr_capsule_v2(state) || bytes.compare(0, prefix.size(), prefix) != 0 ||
      bytes.back() != '\n')
    throw torrent::input_error("RR quiesced receipt identity changed.");
  const std::string decimal = bytes.substr(prefix.size(), bytes.size() - prefix.size() - 1);
  if (state.stop_generation >= static_cast<int64_t>(INT64_MAX) ||
      decimal != std::to_string(static_cast<uint64_t>(state.stop_generation) + 1))
    throw torrent::input_error("RR quiesced generation changed.");
  *generation = static_cast<uint64_t>(state.stop_generation) + 1;
  return true;
}

void rr_quiesced_publish(const RrMigrationCapsule& state, uint64_t generation) {
  if (!rr_capsule_v2(state) || state.stop_generation < 0 ||
      generation != static_cast<uint64_t>(state.stop_generation) + 1)
    throw torrent::input_error("RR quiesce generation cannot be published.");
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, state.hash + ".quiesced.receipt",
        "quiesced:" + state.hash + ":" + state.tx + ":" +
        state.source_incarnation + ":" + std::to_string(state.stop_generation) +
        ":" + std::to_string(generation) + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR quiesced directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
}

std::set<std::string> rr_unproven_receipts;
std::set<std::string> rr_committed_hold;

std::string rr_migration_receipt(const std::string& hash) {
  if (rr_unproven_receipts.count(hash)) return {};
  try { return rr_migration_read_bytes(hash + ".receipt", 192); }
  catch (const torrent::input_error&) { return {}; }
}

bool rr_migration_receipt_present(const std::string& hash) {
  return rr_journal_exists(hash + ".receipt",
                           "RR migration receipt presence cannot be checked.");
}

bool rr_migration_rollback_receipt(const std::string& hash) {
  return rr_migration_receipt(hash) == "rolledback:" + hash + "\n";
}

bool rr_migration_committed_receipt(const std::string& old_hash,
                                    const std::string& new_hash) {
  const std::string receipt = rr_migration_receipt(old_hash);
  const std::string prefix = "committed:" + old_hash + ":" + new_hash + ":";
  return receipt.size() == prefix.size() + 41 &&
      receipt.compare(0, prefix.size(), prefix) == 0 && receipt.back() == '\n' &&
      std::all_of(receipt.begin() + prefix.size(), receipt.end() - 1,
          [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); });
}

bool rr_migration_any_commit_receipt(const std::string& hash) {
  const std::string receipt = rr_migration_receipt(hash);
  const std::string prefix = "committed:" + hash + ":";
  if (receipt.size() != prefix.size() + 82 ||
      receipt.compare(0, prefix.size(), prefix) != 0 ||
      receipt[prefix.size() + 40] != ':' || receipt.back() != '\n') return false;
  return std::all_of(receipt.begin() + prefix.size(), receipt.begin() + prefix.size() + 40,
             [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); }) &&
         std::all_of(receipt.begin() + prefix.size() + 41, receipt.end() - 1,
             [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); });
}

void rr_migration_finish_rollback(const std::string& hash) {
  const std::string path = rpc::call_command_string("session.path");
  const int session = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0)
    throw torrent::input_error("RR rollback cannot open session directory.");
  try {
    for (const char* suffix : {"", ".rtorrent", ".libtorrent_resume"}) {
      const std::string bytes = rr_migration_read_bytes(
          hash + ".rollback.torrent" + suffix, 64 * 1024 * 1024);
      rr_migration_write_file(session, hash + ".torrent" + suffix, bytes);
    }
    if (::fsync(session) < 0)
      throw torrent::input_error("RR rollback session directory fsync failed.");
  } catch (...) {
    ::close(session);
    throw;
  }
  ::close(session);
  const int journal = rr_migration_dir();
  rr_unproven_receipts.insert(hash);
  try {
    rr_migration_write_file(journal, hash + ".receipt", "rolledback:" + hash + "\n");
    if (::fsync(journal) < 0)
      throw torrent::input_error("RR rollback receipt directory fsync failed.");
    rr_unproven_receipts.erase(hash);
  } catch (...) {
    ::close(journal);
    throw;
  }
  ::close(journal);
}

RrMigrationCapsule rr_migration_read_one(const std::string& hash, const char* suffix) {
  const std::string bytes = rr_migration_read_bytes(hash + suffix, 4 * 1024 * 1024);
  torrent::Object root;
  try {
    if (torrent::object_read_bencode_c(bytes.data(), bytes.data() + bytes.size(), &root) !=
        bytes.data() + bytes.size() || !root.is_map() ||
        root.get_key_value("version") != 3 || root.get_key_string("hash") != hash ||
        !root.has_key_map("limits") || !root.has_key_list("files") ||
        !root.has_key_list("views"))
      throw torrent::input_error("RR migration journal schema invalid.");
    RrMigrationCapsule state;
    state.hash = hash;
    state.source_local_id = root.get_key_string("source_local_id");
    state.source_incarnation = root.get_key_string("source_incarnation");
    state.expected_new_hash = root.get_key_string("expected_new_hash");
    state.claim = root.get_key_string("claim");
    state.tx = root.get_key_string("tx");
    state.candidate_sha256 = root.get_key_string("candidate_sha256");
    state.up_total = root.get_key_value("up_total");
    state.down_total = root.get_key_value("down_total");
    state.state_counter = root.get_key_value("state_counter");
    state.stop_generation = root.get_key_value("stop_generation");
    state.views = root.get_key("views");
    const bool v2 = rr_capsule_v2(state);
    if (state.source_local_id.size() != 40 || state.expected_new_hash.size() != 40 ||
        !std::all_of(state.expected_new_hash.begin(), state.expected_new_hash.end(),
          [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); }) ||
        !core::valid_incarnation(state.source_incarnation) ||
        (state.tx.empty() ?
          (state.claim.size() != 64 || !state.candidate_sha256.empty()) :
          (!rr_lower_hex(state.tx, 32) || !rr_lower_hex(state.candidate_sha256, 64) ||
           (!rr_original_marker(state.claim, state.source_local_id) && !v2))) ||
        (v2 && state.expected_new_hash != hash) ||
        state.up_total < 0 || state.down_total < 0 || state.state_counter < 0 ||
        state.stop_generation < 0 || (!state.tx.empty() && !v2 && state.stop_generation == 0) ||
        state.views.as_list().size() > 128)
      throw torrent::input_error("RR migration journal values invalid.");
    if (v2) {
      RrOriginalClaim original;
      if (!rr_original_claim_read(hash, &original) ||
          original.local_id != state.source_local_id ||
          original.incarnation != state.source_incarnation ||
          original.tx != state.tx ||
          original.stop_generation != static_cast<uint64_t>(state.stop_generation) ||
          rr_original_v2_marker(original) != state.claim)
        throw torrent::input_error("RR v2 journal preclaim authority changed.");
    }
    for (const auto& view : state.views.as_list())
      if (!view.is_string() || view.as_string().size() > 128)
        throw torrent::input_error("RR migration journal view invalid.");
    const auto& limits = root.get_key("limits");
    for (const char* key : core::retrackers_migration_limits) {
      if (!limits.has_key_value(key))
        throw torrent::input_error("RR migration journal limit missing.");
      state.limits.emplace_back(key, limits.get_key_value(key));
    }
    const auto& files = root.get_key("files").as_list();
    if (files.size() > 10000)
      throw torrent::input_error("RR migration journal has too many files.");
    for (const auto& item : files) {
      if (!item.is_map() || !item.has_key_string("path") ||
          !item.has_key_value("size") || !item.has_key_value("priority"))
        throw torrent::input_error("RR migration journal file invalid.");
      const auto size = item.get_key_value("size");
      const auto priority = item.get_key_value("priority");
      if (size < 0 || priority < 0 || priority > 2)
        throw torrent::input_error("RR migration journal file values invalid.");
      state.files.push_back({item.get_key_string("path"), size, priority});
    }
    return state;
  } catch (const torrent::bencode_error&) {
    throw torrent::input_error("RR migration journal bencode invalid.");
  }
}

RrMigrationCapsule rr_migration_read(const std::string& hash) {
  try {
    return rr_migration_read_one(hash, ".benc");
  } catch (const torrent::input_error&) {
    auto backup = rr_migration_read_one(hash, ".backup.benc");
    control->core()->push_log_std("RR migration primary journal invalid; using backup: " + hash);
    return backup;
  }
}

std::set<std::string> rr_rollback_pending;
std::map<std::string, std::pair<std::string, std::string>> rr_applied_candidates;

torrent::Object rr_migration_rollback(const std::string& hash) {
  if (hash.size() != 40 || !std::all_of(hash.begin(), hash.end(),
      [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); }))
    throw torrent::input_error("RR rollback hash invalid.");
  if (rr_migration_rollback_receipt(hash))
    return std::string("rolledback");
  if (rr_migration_any_commit_receipt(hash))
    return std::string("committed");
  if (rr_migration_receipt_present(hash))
    return std::string("receipt-held");
  if (control->core()->download_list()->find_hex_ptr(hash.c_str()))
    return std::string("occupied");
  for (const auto& applied : rr_applied_candidates) {
    if (applied.second.first != hash) continue;
    auto* candidate = control->core()->download_list()->find_hex_ptr(applied.first.c_str());
    if (!candidate) continue;
    const auto target = rpc::make_target(candidate);
    const auto state = rr_migration_read(hash);
    if (candidate->is_open() || candidate->is_active() ||
        rpc::call_command_string("d.local_id", target) != applied.second.second ||
        retrieve_d_custom(candidate, "retrackers-recovery").as_string() != state.claim)
      return std::string("successor-busy");
    rpc::call_command("d.erase", torrent::Object(), target);
    if (control->core()->download_list()->find_hex_ptr(applied.first.c_str()))
      return std::string("successor-busy");
    break;
  }
  if (rr_rollback_pending.count(hash))
    return std::string("pending");
  for (const char* suffix : {".rollback.torrent", ".rollback.torrent.rtorrent",
                             ".rollback.torrent.libtorrent_resume"}) {
    const std::string bytes = rr_migration_read_bytes(hash + suffix, 64 * 1024 * 1024);
    torrent::Object object;
    try {
      if (torrent::object_read_bencode_c(bytes.data(), bytes.data() + bytes.size(), &object) !=
          bytes.data() + bytes.size() || !object.is_map() ||
          (std::string(suffix) == ".rollback.torrent" && !object.has_key_map("info")))
        throw torrent::input_error("RR rollback snapshot schema invalid.");
    } catch (const torrent::bencode_error&) {
      throw torrent::input_error("RR rollback snapshot bencode invalid.");
    }
  }
  std::string path = rpc::call_command_string("session.path");
  if (path.back() != '/') path += '/';
  path += ".retrackers-migration/" + hash + ".rollback.torrent";
  auto* factory = new core::DownloadFactory(control->core());
  factory->set_session(true);
  factory->set_retrackers_rollback_hash(hash);
  factory->slot_finished([hash, factory]() {
    rr_rollback_pending.erase(hash);
    if (factory->retrackers_rollback_succeeded()) {
      try {
        rr_migration_finish_rollback(hash);
        control->core()->push_log_std("RR rollback receipt durable: " + hash);
      } catch (const torrent::input_error& e) {
        control->core()->push_log_std("RR rollback receipt failed: " + hash + ": " + e.what());
      }
    } else {
      control->core()->push_log_std("RR rollback load failed; pending snapshot retained: " + hash);
    }
    delete factory;
  });
  rr_rollback_pending.insert(hash);
  factory->load(path);
  factory->commit();
  control->core()->push_log_std("RR rollback source reload scheduled: " + hash);
  return std::string("scheduled");
}

void rr_check_claim(core::Download* download, const std::string& claim) {
  const std::string local_id = rpc::call_command_string("d.local_id", rpc::make_target(download));
  const bool valid = (claim.size() == 64 && std::all_of(claim.begin(), claim.end(),
      [](unsigned char c) { return std::isxdigit(c) != 0; })) ||
      rr_original_marker(claim, local_id);
  if (!valid || retrieve_d_custom(download, "retrackers-recovery").as_string() != claim)
    throw torrent::input_error("RR migration claim mismatch.");
  if (download->is_open() || download->is_active())
    throw torrent::input_error("RR migration requires a closed download.");
}

torrent::Object rr_migration_capture(core::Download* source,
                                     const torrent::Object::list_type& args) {
  if (args.size() != 3 && args.size() != 5)
    throw torrent::input_error("RR capture requires local ID, marker, successor hash, optional transaction and digest.");
  const std::string& local_id = args.front().as_string();
  const std::string& claim = args[1].as_string();
  const std::string& expected_new_hash = args[2].as_string();
  if (expected_new_hash.size() != 40 ||
      !std::all_of(expected_new_hash.begin(), expected_new_hash.end(),
          [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); }))
    throw torrent::input_error("RR capture expected successor hash invalid.");
  const bool v2 = claim.compare(0, sizeof("v2:original:") - 1,
                                 "v2:original:") == 0;
  if (v2 && expected_new_hash !=
      rpc::call_command_string("d.hash", rpc::make_target(source)))
    throw torrent::input_error("RR v2 different-hash capture held before source mutation.");
  if (!stop_intent::held(source).empty())
    throw torrent::input_error("RR capture blocked by file-operation claim.");
  if (!v2) rr_check_claim(source, claim);
  const bool transaction = args.size() == 5;
  if (transaction && (!rr_lower_hex(args[3].as_string(), 32) ||
                      !rr_lower_hex(args[4].as_string(), 64) ||
                      (!v2 && !rr_original_marker(claim, local_id)) ||
                      retrieve_d_custom(source, "retrackers-recovery-ack").as_string() != claim))
    throw torrent::input_error("RR capture marker or staged digest invalid.");
  if (v2 && !transaction)
    throw torrent::input_error("RR v2 capture requires native transaction and staged digest.");
  const auto target = rpc::make_target(source);
  if (local_id != rpc::call_command_string("d.local_id", target))
    throw torrent::input_error("RR capture source generation changed.");
  const std::string hash = rpc::call_command_string("d.hash", target);
  RrOriginalClaim original;
  if (v2) {
    if (!rr_original_claim_read(hash, &original) || original.local_id != local_id ||
        original.incarnation != rpc::call_command_string("d.incarnation", target) ||
        original.tx != args[3].as_string() ||
        rr_original_v2_marker(original) != claim ||
        retrieve_d_custom(source, "retrackers-recovery").as_string() != claim ||
        rr_journal_exists(hash + ".released.receipt") ||
        rr_journal_exists(hash + ".benc") ||
        (rpc::call_command_value("d.state", target) != 0) != (original.run == "1"))
      throw torrent::input_error("RR v2 capture original authority changed.");
    uint64_t generation;
    try { generation = stop_intent::counter(session_thread::manager()->path(), hash, false); }
    catch (const std::exception& e) {
      throw torrent::input_error(std::string("RR v2 capture stop generation unavailable: ") + e.what());
    }
    if (generation != original.stop_generation)
      throw torrent::input_error("RR v2 capture newer Stop held before quiesce.");
  }
  RrMigrationCapsule state;
  state.hash = hash;
  if (expected_new_hash != hash &&
      control->core()->download_list()->find_hex_ptr(expected_new_hash.c_str()))
    throw torrent::input_error("RR capture expected successor already occupied.");
  const int session = ::open(rpc::call_command_string("session.path").c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR capture session directory unavailable.");
  struct stat successor_session;
  const int successor_found = ::fstatat(session, (expected_new_hash + ".torrent").c_str(),
                                         &successor_session, AT_SYMLINK_NOFOLLOW);
  const int successor_error = errno;
  ::close(session);
  if (successor_found < 0 && successor_error != ENOENT)
    throw torrent::input_error("RR capture cannot inspect successor session file.");
  if (successor_found == 0 && expected_new_hash != hash)
    throw torrent::input_error("RR capture expected successor session file already exists.");
  state.source_local_id = local_id;
  state.source_incarnation = rpc::call_command_string("d.incarnation", target);
  state.expected_new_hash = expected_new_hash;
  state.claim = claim;
  state.tx = transaction ? args[3].as_string() : std::string();
  state.candidate_sha256 = transaction ? args[4].as_string() : std::string();
  for (const char* key : core::retrackers_migration_limits)
    state.limits.emplace_back(key, rpc::call_command_value(key, target));
  state.views = source->bencode()->get_key("rtorrent").get_key("views");
  if (!state.views.is_list() || state.views.as_list().size() > 128)
    throw torrent::input_error("RR capture invalid views.");
  for (const auto& view : state.views.as_list())
    if (!view.is_string() || view.as_string().size() > 128)
      throw torrent::input_error("RR capture invalid view.");
  if (source->file_list()->size_files() > 10000)
    throw torrent::input_error("RR capture has too many files.");
  for (auto it = source->file_list()->begin(); it != source->file_list()->end(); ++it)
    state.files.push_back({(*it)->path()->as_string(),
                           static_cast<int64_t>((*it)->size_bytes()),
                           static_cast<int64_t>((*it)->priority())});
  state.up_total = source->info()->up_rate()->total();
  state.down_total = source->info()->down_rate()->total();
  state.state_counter = source->bencode()->get_key("rtorrent").get_key_value("state_counter");
  try {
    state.stop_generation = v2 ? static_cast<int64_t>(original.stop_generation) :
        static_cast<int64_t>(stop_intent::counter(
            session_thread::manager()->path(), hash, false));
  } catch (const std::exception& e) {
    throw torrent::input_error(std::string("RR capture stop generation unavailable: ") + e.what());
  }
  if (v2)
    rr_late_save_to_prefix(source, hash + ".claim." + original.tx,
                           hash, original.incarnation);
  rr_migration_publish(state, source); // Full rollback and T-bound intent precede owned stop.
  if (v2) {
    if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
        std::string(cut) == "after-capture-intent") ::_exit(137);
    stop_intent::record(source, true);
    {
      stop_intent::ScopedSuppress suppress;
      rpc::parse_command_single(target, "view.set_visible=stopped");
      rpc::call_command("d.close", torrent::Object(), target);
    }
    uint64_t quiesced;
    try { quiesced = stop_intent::counter(session_thread::manager()->path(), hash, false); }
    catch (const std::exception& e) {
      throw torrent::input_error(std::string("RR v2 quiesce generation unavailable: ") + e.what());
    }
    if (quiesced != static_cast<uint64_t>(state.stop_generation) + 1 ||
        rpc::call_command_value("d.state", target) != 0 ||
        source->is_open() || source->is_active())
      throw torrent::input_error("RR v2 owned quiesce did not close source.");
    rr_quiesced_publish(state, quiesced);
    if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
        std::string(cut) == "after-capture-quiesced-throw")
      throw torrent::input_error("RR test quiesced receipt before source erase.");
    if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
        std::string(cut) == "after-capture-quiesced") ::_exit(137);
    rr_native_capture_erase_hash = hash;
    try { rpc::call_command("d.erase", torrent::Object(), target); }
    catch (...) { rr_native_capture_erase_hash.clear(); throw; }
    rr_native_capture_erase_hash.clear();
    if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
        std::string(cut) == "after-capture-erase") ::_exit(137);
  } else {
    rpc::call_command("d.erase", torrent::Object(), target);
  }
  return int64_t(1);
}

torrent::Object rr_migration_apply(core::Download* candidate,
                                   const torrent::Object::list_type& args) {
  if (!core::RetrackersMigrationCreation::mark_apply(candidate))
    throw torrent::input_error("RR apply requires candidate creation context.");
  if (args.size() != 3)
    throw torrent::input_error("RR apply requires source hash, source local ID and claim.");
  const std::string& source_hash = args[0].as_string();
  const std::string& source_local_id = args[1].as_string();
  const std::string& claim = args[2].as_string();
  const auto target = rpc::make_target(candidate);
  const std::string hash = rpc::call_command_string("d.hash", target);
  if (retrackers_migration_target_source(hash) != source_hash ||
      !retrackers_migration_pending(source_hash))
    throw torrent::input_error("RR apply successor reservation missing.");
  const RrMigrationCapsule state = rr_migration_read(source_hash);
  if (state.tx.empty()) {
    rr_check_claim(candidate, claim);
  } else if (claim != state.tx || candidate->is_open() || candidate->is_active() ||
             retrieve_d_custom(candidate, "retrackers-recovery").as_string() !=
                 rr_candidate_claim_marker(state) ||
             retrieve_d_custom(candidate, "retrackers-recovery-ack").as_string() != "") {
    throw torrent::input_error("RR apply v1 candidate claim mismatch.");
  }
  if (state.expected_new_hash != hash ||
      (state.tx.empty() ? state.claim != claim : state.tx != claim) ||
      state.source_local_id != source_local_id ||
      rpc::call_command_string("d.local_id", target) == source_local_id ||
      rpc::call_command_string("d.incarnation", target) == state.source_incarnation)
    throw torrent::input_error("RR apply generation mismatch.");
  if (candidate->file_list()->size_files() != state.files.size())
    throw torrent::input_error("RR apply file count changed.");
  auto file = candidate->file_list()->begin();
  for (const auto& expected : state.files) {
    if ((*file)->path()->as_string() != expected.path ||
        static_cast<int64_t>((*file)->size_bytes()) != expected.size)
      throw torrent::input_error("RR apply file identity changed.");
    ++file;
  }
  for (const auto& setting : state.limits)
    rpc::call_command((setting.first + ".set").c_str(), setting.second, target);
  candidate->bencode()->get_key("rtorrent").insert_key("views", state.views);
  file = candidate->file_list()->begin();
  for (const auto& expected : state.files) {
    rpc::call_command("f.priority.set", expected.priority, rpc::make_target(&**file));
    ++file;
  }
  rpc::call_command("d.update_priorities", torrent::Object(), target);
  candidate->info()->mutable_up_rate()->set_total(state.up_total);
  candidate->info()->mutable_down_rate()->set_total(state.down_total);
  candidate->bencode()->get_key("rtorrent").insert_key("state_counter", state.state_counter);
  if (!core::RetrackersMigrationCreation::mark_applied(candidate))
    throw torrent::input_error("RR apply lost candidate creation context.");
  rr_applied_candidates[hash] = {source_hash, rpc::call_command_string("d.local_id", target)};
  return int64_t(1);
}
} // namespace

std::string rr_committed_digest(const std::vector<std::pair<std::string, std::string>>& files) {
  torrent::Object payload = torrent::Object::create_list();
  for (const auto& file : files) payload.as_list().push_back(file.second);
  return torrent::utils::transform_to_hex_str(torrent::object_sha1(&payload));
}

torrent::Object rr_migration_commit(core::Download* candidate,
                                    const torrent::Object::list_type& args) {
  if (args.size() != 3)
    throw torrent::input_error("RR commit requires source hash, local ID and claim.");
  const std::string& source_hash = args[0].as_string();
  const std::string& source_local_id = args[1].as_string();
  const std::string& claim = args[2].as_string();
  const auto target = rpc::make_target(candidate);
  const std::string new_hash = rpc::call_command_string("d.hash", target);
  if (rr_migration_committed_receipt(source_hash, new_hash)) return std::string("committed");
  const RrMigrationCapsule state = rr_migration_read(source_hash);
  if (rr_capsule_v2(state)) {
    uint64_t quiesced;
    if (!rr_quiesced_read(state, &quiesced))
      throw torrent::input_error("RR v2 commit missing owned-quiesce receipt.");
    uint64_t current;
    try { current = stop_intent::counter(session_thread::manager()->path(), source_hash, false); }
    catch (const std::exception& e) {
      throw torrent::input_error(std::string("RR v2 commit generation unavailable: ") + e.what());
    }
    if (current != quiesced)
      throw torrent::input_error("RR v2 commit newer Stop held.");
  }
  if (state.tx.empty()) {
    rr_check_claim(candidate, claim);
  } else {
    const std::string ready = rr_candidate_ready_marker(state);
    if (claim != state.tx || candidate->is_open() || candidate->is_active() ||
        retrieve_d_custom(candidate, "retrackers-recovery").as_string() != ready ||
        retrieve_d_custom(candidate, "retrackers-recovery-ack").as_string() != ready)
      throw torrent::input_error("RR commit v1 ready marker or acknowledgement missing.");
  }
  const std::string new_local_id = rpc::call_command_string("d.local_id", target);
  const auto applied = rr_applied_candidates.find(new_hash);
  if (!retrackers_migration_pending(source_hash) ||
      retrackers_migration_target_source(new_hash) != source_hash ||
      state.expected_new_hash != new_hash || state.source_local_id != source_local_id ||
      (state.tx.empty() ? state.claim != claim : state.tx != claim) ||
      rpc::call_command_string("d.incarnation", target) == state.source_incarnation ||
      applied == rr_applied_candidates.end() ||
      applied->second.first != source_hash || applied->second.second != new_local_id)
    throw torrent::input_error("RR commit candidate proof changed.");
  if (source_hash != new_hash &&
      control->core()->download_list()->find_hex_ptr(source_hash.c_str()))
    throw torrent::input_error("RR commit source unexpectedly exists.");
  for (const auto& setting : state.limits)
    if (rpc::call_command_value(setting.first.c_str(), target) != setting.second)
      throw torrent::input_error("RR commit candidate limit changed.");
  if (candidate->file_list()->size_files() != state.files.size() ||
      candidate->info()->up_rate()->total() != state.up_total ||
      candidate->info()->down_rate()->total() != state.down_total ||
      candidate->bencode()->get_key("rtorrent").get_key_value("state_counter") != state.state_counter)
    throw torrent::input_error("RR commit candidate accounting or files changed.");
  std::ostringstream expected_views, actual_views;
  torrent::object_write_bencode(&expected_views, &state.views, 0);
  const auto& views = candidate->bencode()->get_key("rtorrent").get_key("views");
  torrent::object_write_bencode(&actual_views, &views, 0);
  if (!expected_views.good() || !actual_views.good() ||
      expected_views.str() != actual_views.str())
    throw torrent::input_error("RR commit candidate views changed.");
  auto file = candidate->file_list()->begin();
  for (const auto& expected : state.files) {
    if ((*file)->path()->as_string() != expected.path ||
        static_cast<int64_t>((*file)->size_bytes()) != expected.size ||
        static_cast<int64_t>((*file)->priority()) != expected.priority)
      throw torrent::input_error("RR commit candidate file state changed.");
    ++file;
  }
  const auto files = rr_migration_snapshot(candidate);
  const int authority = rr_migration_dir();
  try {
    for (const auto& file_bytes : files)
      rr_migration_write_file(authority, source_hash + ".committed" +
          file_bytes.first.substr(9), file_bytes.second);
    if (::fsync(authority) < 0)
      throw torrent::input_error("RR commit authority directory fsync failed.");
  } catch (...) { ::close(authority); throw; }
  ::close(authority);
  const std::string path = rpc::call_command_string("session.path");
  const int session = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR commit session directory unavailable.");
  try {
    for (const auto& file_bytes : files)
      rr_migration_write_file(session, new_hash + file_bytes.first.substr(9), file_bytes.second);
    if (::fsync(session) < 0)
      throw torrent::input_error("RR commit session directory fsync failed.");
  } catch (...) { ::close(session); throw; }
  ::close(session);
  const int journal = rr_migration_dir();
  rr_unproven_receipts.insert(source_hash);
  try {
    rr_migration_write_file(journal, source_hash + ".receipt",
        "committed:" + source_hash + ":" + new_hash + ":" +
        rr_committed_digest(files) + "\n");
    if (::fsync(journal) < 0)
      throw torrent::input_error("RR commit receipt directory fsync failed.");
    rr_unproven_receipts.erase(source_hash);
  } catch (...) { ::close(journal); throw; }
  ::close(journal);
  return std::string("committed");
}

bool retrackers_migration_pending(const std::string& hash);

namespace {
std::string rr_session_read(const int session, const std::string& name) {
  const int fd = ::openat(session, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  struct stat st;
  if (fd < 0 || ::fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != ::geteuid() || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) {
    if (fd >= 0) ::close(fd);
    throw torrent::input_error("RR committed ordinary session file missing or unsafe.");
  }
  std::string bytes(static_cast<size_t>(st.st_size), '\0');
  bool okay = true;
  for (size_t offset = 0; okay && offset < bytes.size();) {
    const ssize_t n = ::read(fd, &bytes[offset], bytes.size() - offset);
    if (n <= 0) okay = false;
    else offset += n;
  }
  ::close(fd);
  if (!okay) throw torrent::input_error("RR committed ordinary session read failed.");
  return bytes;
}

torrent::Object rr_committed_map(const std::string& bytes) {
  torrent::Object object;
  try {
    if (torrent::object_read_bencode_c(bytes.data(), bytes.data() + bytes.size(), &object) !=
        bytes.data() + bytes.size() || !object.is_map())
      throw torrent::input_error("RR committed session bencode invalid.");
  } catch (const torrent::bencode_error&) {
    throw torrent::input_error("RR committed session bencode invalid.");
  }
  return object;
}

std::string rr_bencode(const torrent::Object& object) {
  std::ostringstream stream;
  torrent::object_write_bencode(&stream, &object, 0);
  if (!stream.good()) throw torrent::input_error("RR committed session serialization failed.");
  return stream.str();
}

void rr_committed_fallback_source(const std::string& old_hash) {
  const auto state = rr_migration_read(old_hash);
  const auto metainfo = rr_committed_map(rr_migration_read_bytes(
      old_hash + ".rollback.torrent", 64 * 1024 * 1024));
  if (!metainfo.has_key_map("info"))
    throw torrent::input_error("RR committed fallback metainfo invalid.");
  const std::string info_sha = torrent::object_sha1(&metainfo.get_key("info"));
  const std::string actual_hash = torrent::utils::transform_to_hex_str(info_sha);
  if (actual_hash != old_hash)
    throw torrent::input_error("RR committed fallback source hash mismatch.");
  const auto sidecar = rr_committed_map(rr_migration_read_bytes(
      old_hash + ".rollback.torrent.rtorrent", 64 * 1024 * 1024));
  if (!sidecar.has_key_map("custom") ||
      !sidecar.get_key("custom").has_key_string("retrackers-recovery") ||
      sidecar.get_key("custom").get_key_string("retrackers-recovery") != state.claim ||
      !sidecar.has_key_value("state") || sidecar.get_key_value("state") < 0 ||
      sidecar.get_key_value("state") > 1 ||
      !sidecar.has_key_value("state_changed") || sidecar.get_key_value("state_changed") <= 0 ||
      !sidecar.has_key_value("total_uploaded") ||
      sidecar.get_key_value("total_uploaded") != state.up_total ||
      !sidecar.has_key_value("total_downloaded") ||
      sidecar.get_key_value("total_downloaded") != state.down_total ||
      !sidecar.has_key_value("state_counter") ||
      sidecar.get_key_value("state_counter") != state.state_counter ||
      !sidecar.has_key_list("views") ||
      rr_bencode(sidecar.get_key("views")) != rr_bencode(state.views) ||
      !sidecar.has_key_map("retrackers_limits"))
    throw torrent::input_error("RR committed fallback source sidecar invalid.");
  const auto& limits = sidecar.get_key("retrackers_limits");
  for (const auto& setting : state.limits)
    if (!limits.has_key_value(setting.first) ||
        limits.get_key_value(setting.first) != setting.second)
      throw torrent::input_error("RR committed fallback source limits invalid.");
  const auto resume = rr_committed_map(rr_migration_read_bytes(
      old_hash + ".rollback.torrent.libtorrent_resume", 64 * 1024 * 1024));
  if (!resume.has_key_list("files") ||
      resume.get_key("files").as_list().size() != state.files.size())
    throw torrent::input_error("RR committed fallback source resume invalid.");
  size_t index = 0;
  for (const auto& file : resume.get_key("files").as_list()) {
    if (!file.is_map() || !file.has_key_value("priority") ||
        file.get_key_value("priority") != state.files[index++].priority)
      throw torrent::input_error("RR committed fallback source priority invalid.");
  }
  rr_migration_finish_rollback(old_hash); // Session triple fsync precedes rollback receipt fsync.
}

std::string rr_committed_marker(const RrMigrationCapsule& state) {
  return state.tx.empty() ? state.claim : rr_candidate_ready_marker(state);
}

bool rr_committed_current_valid(const int session, const std::string& new_hash,
                                const RrMigrationCapsule& state,
                                const std::vector<std::pair<std::string, std::string>>& authority) {
  try {
    const auto metainfo = rr_committed_map(rr_session_read(session, new_hash + ".torrent"));
    const auto saved_metainfo = rr_committed_map(authority[0].second);
    if (!metainfo.has_key_map("info") || !saved_metainfo.has_key_map("info") ||
        rr_bencode(metainfo.get_key("info")) != rr_bencode(saved_metainfo.get_key("info")))
      return false;
    const auto sidecar = rr_committed_map(rr_session_read(session, new_hash + ".torrent.rtorrent"));
    if (!sidecar.has_key_value("total_uploaded") ||
        !sidecar.has_key_value("total_downloaded") ||
        !sidecar.has_key_value("state_counter") || !sidecar.has_key_list("views") ||
        !sidecar.has_key_map("custom") || !sidecar.has_key_map("retrackers_limits") ||
        !sidecar.get_key("custom").has_key_string("retrackers-recovery") ||
        sidecar.get_key("custom").get_key_string("retrackers-recovery") != rr_committed_marker(state) ||
        (state.tx.empty() ? false :
          (!sidecar.get_key("custom").has_key_string("retrackers-recovery-ack") ||
           sidecar.get_key("custom").get_key_string("retrackers-recovery-ack") !=
               rr_committed_marker(state))) ||
        !sidecar.has_key_string("incarnation") ||
        !core::valid_incarnation(sidecar.get_key_string("incarnation")) ||
        sidecar.get_key_string("incarnation") == state.source_incarnation ||
        !sidecar.has_key_value("state") || !sidecar.has_key_value("state_changed") ||
        sidecar.get_key_value("state") < 0 || sidecar.get_key_value("state") > 1 ||
        sidecar.get_key_value("state_changed") <= 0 ||
        sidecar.get_key_value("total_uploaded") < state.up_total ||
        sidecar.get_key_value("total_downloaded") < state.down_total ||
        sidecar.get_key_value("state_counter") < state.state_counter ||
        sidecar.get_key_value("state_counter") > (1 << 20))
      return false;
    const auto& views = sidecar.get_key("views").as_list();
    if (views.size() > 128) return false;
    for (const auto& view : views)
      if (!view.is_string() || view.as_string().size() > 128) return false;
    const auto& limits = sidecar.get_key("retrackers_limits");
    for (const char* key : core::retrackers_migration_limits)
      if (!limits.has_key_value(key) || limits.get_key_value(key) < -1 ||
          limits.get_key_value(key) > 2147483647) return false;
    const auto resume = rr_committed_map(rr_session_read(session, new_hash + ".torrent.libtorrent_resume"));
    if (!resume.has_key_list("files") || resume.get_key("files").as_list().size() != state.files.size())
      return false;
    for (const auto& file : resume.get_key("files").as_list())
      if (!file.is_map() || !file.has_key_value("priority") ||
          file.get_key_value("priority") < 0 || file.get_key_value("priority") > 2)
        return false;
    return true;
  } catch (const torrent::input_error&) { return false; }
}
bool rr_committed_reseed_from_ordinary(const std::string& old_hash,
                                       const std::string& new_hash) {
  const auto state = rr_migration_read(old_hash);
  if (state.expected_new_hash != new_hash ||
      retrackers_migration_target_source(new_hash) != old_hash)
    return false;
  const int session = ::open(rpc::call_command_string("session.path").c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR committed ordinary session directory unavailable.");
  std::vector<std::pair<std::string, std::string>> ordinary;
  try {
    for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"})
      ordinary.push_back({suffix, rr_session_read(session, new_hash + suffix)});
    const auto metainfo = rr_committed_map(ordinary[0].second);
    if (!metainfo.has_key_map("info")) { ::close(session); return false; }
    const std::string info_sha = torrent::object_sha1(&metainfo.get_key("info"));
    const std::string actual_hash = torrent::utils::transform_to_hex_str(info_sha);
    if (actual_hash != new_hash ||
        !rr_committed_current_valid(session, new_hash, state, ordinary)) {
      ::close(session);
      return false;
    }
  } catch (const torrent::input_error&) { ::close(session); return false; }
  catch (...) { ::close(session); throw; }
  ::close(session);
  const int journal = rr_migration_dir();
  try {
    for (const auto& file : ordinary)
      rr_migration_write_file(journal, old_hash + ".committed" + file.first, file.second);
    if (::fsync(journal) < 0)
      throw torrent::input_error("RR committed ordinary reseed directory fsync failed.");
    rr_unproven_receipts.insert(old_hash);
    rr_migration_write_file(journal, old_hash + ".receipt",
        "committed:" + old_hash + ":" + new_hash + ":" +
        rr_committed_digest(ordinary) + "\n");
    if (::fsync(journal) < 0)
      throw torrent::input_error("RR committed ordinary reseed receipt fsync failed.");
    rr_unproven_receipts.erase(old_hash);
  } catch (...) { ::close(journal); throw; }
  ::close(journal);
  return true;
}


struct RrLateReceipt {
  std::string new_hash;
  std::string incarnation;
  uint64_t sequence{};
  std::string digest;
};

bool rr_late_read_receipt(const std::string& old_hash, RrLateReceipt* result) {
  const int dir = rr_migration_dir();
  struct stat st;
  const int found = ::fstatat(dir, (old_hash + ".late.receipt").c_str(), &st, AT_SYMLINK_NOFOLLOW);
  const int lookup_error = errno;
  ::close(dir);
  if (found < 0 && lookup_error == ENOENT) return false;
  if (found < 0) throw torrent::input_error("RR late receipt presence cannot be checked.");
  const std::string bytes = rr_migration_read_bytes(old_hash + ".late.receipt", 256);
  if (bytes.back() != '\n')
    throw torrent::input_error("RR late receipt malformed.");
  std::vector<std::string> fields;
  size_t begin = 0;
  for (size_t end = 0; (end = bytes.find(':', begin)) != std::string::npos; begin = end + 1)
    fields.push_back(bytes.substr(begin, end - begin));
  fields.push_back(bytes.substr(begin, bytes.size() - begin - 1));
  if (fields.size() != 6 || fields[0] != "late" || fields[1] != old_hash ||
      fields[2].size() != 40 || !core::valid_incarnation(fields[3]) ||
      fields[4].empty() || fields[4].size() > 18 ||
      !std::all_of(fields[4].begin(), fields[4].end(), ::isdigit) ||
      fields[5].size() != 40 ||
      !std::all_of(fields[5].begin(), fields[5].end(), [](unsigned char c) {
        return std::isxdigit(c) && !std::islower(c);
      }))
    throw torrent::input_error("RR late receipt malformed.");
  uint64_t sequence = 0;
  for (char digit : fields[4]) sequence = sequence * 10 + (digit - '0');
  if (sequence == 0 || std::to_string(sequence) != fields[4])
    throw torrent::input_error("RR late receipt sequence malformed.");
  *result = {fields[2], fields[3], sequence, fields[5]};
  return true;
}

std::vector<std::pair<std::string, std::string>> rr_late_copy(
    const std::string& old_hash, const std::string& new_hash, uint64_t sequence, char slot) {
  std::vector<std::pair<std::string, std::string>> files;
  const std::string prefix = old_hash + ".late." + std::to_string(sequence) + "." + slot;
  const int session = slot == 'o' ?
      ::open(rpc::call_command_string("session.path").c_str(),
             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
  if (slot == 'o' && session < 0)
    throw torrent::input_error("RR late ordinary session directory unavailable.");
  try {
    for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"}) {
      const std::string bytes = slot == 'o' ?
          rr_session_read(session, new_hash + suffix) :
          rr_migration_read_bytes(prefix + suffix, 64 * 1024 * 1024);
      rr_committed_map(bytes);
      files.push_back({suffix, bytes});
    }
  } catch (...) { if (session >= 0) ::close(session); throw; }
  if (session >= 0) ::close(session);
  return files;
}

std::vector<std::pair<std::string, std::string>> rr_late_select(
    const std::string& old_hash, const std::string& new_hash,
    const RrLateReceipt& receipt, bool marker_free) {
  std::vector<std::pair<std::string, std::string>> files;
  for (char slot : {'a', 'b', 'o'}) {
    try {
      auto copy = rr_late_copy(old_hash, new_hash, receipt.sequence, slot);
      if (rr_committed_digest(copy) != receipt.digest) continue;
      const auto metainfo = rr_committed_map(copy[0].second);
      if (!metainfo.has_key_map("info")) continue;
      const auto info_sha = torrent::object_sha1(&metainfo.get_key("info"));
      const std::string actual_hash = torrent::utils::transform_to_hex_str(info_sha);
      const auto sidecar = rr_committed_map(copy[1].second);
      if (actual_hash != new_hash || !sidecar.has_key_string("incarnation") ||
          sidecar.get_key_string("incarnation") != receipt.incarnation ||
          (marker_free && (!sidecar.has_key_map("custom") ||
            !sidecar.get_key("custom").has_key_string("retrackers-recovery") ||
            !sidecar.get_key("custom").get_key_string("retrackers-recovery").empty() ||
            !sidecar.get_key("custom").has_key_string("retrackers-recovery-ack") ||
            !sidecar.get_key("custom").get_key_string("retrackers-recovery-ack").empty())))
        continue;
      files = std::move(copy);
      break;
    } catch (const torrent::input_error&) { continue; }
  }
  if (files.empty())
    throw torrent::input_error("RR late receipt has no complete matching authority copy.");
  return files;
}

bool rr_late_repair(const std::string& old_hash, const std::string& new_hash) {
  RrLateReceipt receipt;
  if (!rr_late_read_receipt(old_hash, &receipt)) return false;
  const auto state = rr_migration_read(old_hash);
  if (receipt.new_hash != new_hash || receipt.incarnation == state.source_incarnation)
    throw torrent::input_error("RR late receipt owner mismatch.");
  auto files = rr_late_select(old_hash, new_hash, receipt, false);
  const int session = ::open(rpc::call_command_string("session.path").c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR late session directory unavailable.");
  try {
    for (const auto& file : files)
      rr_migration_write_file(session, new_hash + file.first, file.second);
    if (::fsync(session) < 0)
      throw torrent::input_error("RR late ordinary session fsync failed.");
  } catch (...) { ::close(session); throw; }
  ::close(session);
  const int journal = rr_migration_dir();
  try {
    for (char slot : {'a', 'b'})
      for (const auto& file : files)
        rr_migration_write_file(journal, old_hash + ".late." +
            std::to_string(receipt.sequence) + "." + slot + file.first, file.second);
    if (::fsync(journal) < 0)
      throw torrent::input_error("RR late authority repair fsync failed.");
  } catch (...) { ::close(journal); throw; }
  ::close(journal);
  return true;
}

std::map<std::string, std::pair<std::string, std::string>> rr_released_owners;
std::set<std::string> rr_retired_hold_hashes;
std::set<std::string> rr_owner_hold_hashes;

bool rr_journal_exists(const std::string& name, const char* error_message) {
  const int dir = rr_migration_dir();
  struct stat st;
  const int found = ::fstatat(dir, name.c_str(), &st, AT_SYMLINK_NOFOLLOW);
  const int error = errno;
  ::close(dir);
  if (found == 0) return true;
  if (error == ENOENT) return false;
  throw torrent::input_error(error_message);
}

bool rr_owner_read(const std::string& hash, std::string* incarnation, bool* retired) {
  if (!rr_journal_exists(hash + ".owner.current")) return false;
  const std::string bytes = rr_migration_read_bytes(hash + ".owner.current", 128);
  const bool active = bytes.compare(0, 7, "active:") == 0;
  *retired = bytes.compare(0, 8, "retired:") == 0;
  if (!active && !*retired)
    throw torrent::input_error("RR owner pointer state invalid.");
  const size_t prefix = active ? 7 : 8;
  if (bytes.size() != prefix + 40 + 1 + 32 + 1 ||
      bytes.compare(prefix, 40, hash) != 0 || bytes[prefix + 40] != ':' ||
      bytes.back() != '\n')
    throw torrent::input_error("RR owner pointer shape invalid.");
  *incarnation = bytes.substr(prefix + 41, 32);
  if (!core::valid_incarnation(*incarnation))
    throw torrent::input_error("RR owner pointer incarnation invalid.");
  return true;
}

std::string rr_owner_prefix(const std::string& hash, const std::string& incarnation) {
  return hash + ".owner." + incarnation;
}

void rr_owner_publish(const std::string& hash, const std::string& incarnation, bool retired) {
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, hash + ".owner.current",
        std::string(retired ? "retired:" : "active:") + hash + ":" + incarnation + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR owner pointer directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
}

bool rr_retired_present(const std::string& old_hash) {
  return rr_journal_exists(old_hash + ".retired.receipt",
                           "RR retired receipt presence cannot be checked.");
}

void rr_late_prune(const std::string& prefix, uint64_t latest) {
  if (latest < 3) return;
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR late prune scan failed."); }
  const std::string head = prefix + ".late.";
  std::vector<std::string> obsolete;
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.compare(0, head.size(), head) != 0) continue;
    const size_t end = name.find('.', head.size());
    if (end == std::string::npos || end == head.size()) continue;
    const std::string digits = name.substr(head.size(), end - head.size());
    if (digits.size() > 18 || !std::all_of(digits.begin(), digits.end(), ::isdigit)) continue;
    uint64_t sequence = 0;
    for (char digit : digits) sequence = sequence * 10 + (digit - '0');
    if (sequence > 0 && sequence + 1 < latest &&
        (name.compare(end, 10, ".a.torrent") == 0 ||
         name.compare(end, 10, ".b.torrent") == 0))
      obsolete.push_back(name);
  }
  for (const auto& name : obsolete)
    if (::unlinkat(fd, name.c_str(), 0) < 0 && errno != ENOENT) {
      ::closedir(directory);
      throw torrent::input_error("RR late prune removal failed.");
    }
  if (!obsolete.empty() && ::fsync(fd) < 0) {
    ::closedir(directory);
    throw torrent::input_error("RR late prune directory fsync failed.");
  }
  ::closedir(directory);
}

uint64_t rr_late_save_to_prefix(core::Download* download, const std::string& prefix,
                                const std::string& hash, const std::string& incarnation) {
  RrLateReceipt previous;
  const bool has_previous = rr_late_read_receipt(prefix, &previous);
  if (has_previous && (previous.new_hash != hash || previous.incarnation != incarnation))
    throw torrent::input_error("RR late save previous owner mismatch.");
  const uint64_t sequence = has_previous ? previous.sequence + 1 : 1;
  if (sequence == 0)
    throw torrent::input_error("RR late save sequence overflow.");
  session_thread::manager()->drain_download_saves(download);
  session::DownloadStorer storer(download);
  storer.build_full_streams();
  const std::string ordinary = storer.build_path(session_thread::manager()->path());
  auto torrent_stream = storer.torrent_stream();
  auto rtorrent_stream = storer.rtorrent_stream();
  auto resume_stream = storer.libtorrent_stream();
  const std::vector<std::pair<std::string, std::string>> files = {
    {".torrent", torrent_stream->str()},
    {".torrent.rtorrent", rtorrent_stream->str()},
    {".torrent.libtorrent_resume", resume_stream->str()}};
  const std::string journal = session_thread::manager()->path() + ".retrackers-migration/";
  for (char slot : {'a', 'b'})
    session::DownloadStorer::save_and_move_streams(
        journal + prefix + ".late." + std::to_string(sequence) + "." + slot + ".torrent",
        true, torrent_stream.get(), rtorrent_stream.get(), resume_stream.get());
  session::DownloadStorer::save_and_move_streams(ordinary, true,
      torrent_stream.get(), rtorrent_stream.get(), resume_stream.get());
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, prefix + ".late.receipt",
        "late:" + prefix + ":" + hash + ":" + incarnation + ":" +
        std::to_string(sequence) + ":" + rr_committed_digest(files) + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR late receipt directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
  rr_late_prune(prefix, sequence);
  return sequence;
}

std::string rr_original_v2_marker(const RrOriginalClaim& claim) {
  return "v2:original:" + claim.run + ":" + claim.local_id + ":" +
      claim.user_digest + ":" + claim.tx;
}

bool rr_original_claim_read(const std::string& hash, RrOriginalClaim* claim) {
  if (!rr_journal_exists(hash + ".preclaim.receipt")) return false;
  const std::string bytes = rr_migration_read_bytes(hash + ".preclaim.receipt", 320);
  if (bytes.back() != '\n')
    throw torrent::input_error("RR original preclaim receipt malformed.");
  std::vector<std::string> fields;
  size_t begin = 0;
  for (size_t end = 0; (end = bytes.find(':', begin)) != std::string::npos; begin = end + 1)
    fields.push_back(bytes.substr(begin, end - begin));
  fields.push_back(bytes.substr(begin, bytes.size() - begin - 1));
  if (fields.size() != 9 || fields[0] != "preclaim" || fields[1] != hash ||
      fields[2].size() != 40 || !core::valid_incarnation(fields[3]) ||
      !rr_lower_hex(fields[4], 32) || (fields[5] != "0" && fields[5] != "1") ||
      !rr_lower_hex(fields[6], 64) || fields[7].empty() || fields[7].size() > 18 ||
      !std::all_of(fields[7].begin(), fields[7].end(), ::isdigit) ||
      (fields[8].size() != 40 || fields[8].find_first_not_of("0123456789ABCDEF") != std::string::npos))
    throw torrent::input_error("RR original preclaim identity malformed.");
  uint64_t generation = 0;
  for (char digit : fields[7]) {
    if (generation > (static_cast<uint64_t>(INT64_MAX) - (digit - '0')) / 10)
      throw torrent::input_error("RR original preclaim generation overflow.");
    generation = generation * 10 + digit - '0';
  }
  if (std::to_string(generation) != fields[7])
    throw torrent::input_error("RR original preclaim generation noncanonical.");
  *claim = {hash, fields[2], fields[3], fields[4], fields[6], fields[5],
            generation, fields[8]};
  return true;
}

std::vector<std::pair<std::string, std::string>> rr_original_snapshot(core::Download* owner) {
  if (!owner->bencode()->has_key_map("info"))
    throw torrent::input_error("RR original snapshot requires complete metainfo.");
  session::DownloadStorer storer(owner);
  storer.build_full_streams();
  std::vector<std::pair<std::string, std::string>> files = {
      {".torrent", storer.torrent_stream()->str()},
      {".torrent.rtorrent", storer.rtorrent_stream()->str()},
      {".torrent.libtorrent_resume", storer.libtorrent_stream()->str()}};
  size_t total = 0;
  for (const auto& file : files) {
    total += file.second.size();
    if (file.second.empty() || file.second.size() > 64 * 1024 * 1024 ||
        total > 128 * 1024 * 1024)
      throw torrent::input_error("RR original snapshot exceeds recovery reader limit.");
  }
  return files;
}

void rr_original_write_triple(const std::string& prefix,
    const std::vector<std::pair<std::string, std::string>>& files) {
  const int dir = rr_migration_dir();
  try {
    for (const auto& file : files)
      rr_migration_write_file(dir, prefix + file.first, file.second);
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR original private triple directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
}

std::vector<std::pair<std::string, std::string>> rr_original_read_triple(
    const std::string& prefix, const std::string& digest,
    const std::string& hash, const std::string& incarnation) {
  std::vector<std::pair<std::string, std::string>> files;
  for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"}) {
    std::string bytes = rr_migration_read_bytes(prefix + suffix, 64 * 1024 * 1024);
    rr_committed_map(bytes);
    files.emplace_back(suffix, std::move(bytes));
  }
  if (rr_committed_digest(files) != digest)
    throw torrent::input_error("RR original triple digest changed.");
  const auto metainfo = rr_committed_map(files[0].second);
  const auto sidecar = rr_committed_map(files[1].second);
  if (!metainfo.has_key_map("info") || !sidecar.has_key_string("incarnation") ||
      sidecar.get_key_string("incarnation") != incarnation)
    throw torrent::input_error("RR original triple incarnation changed.");
  const auto sha = torrent::object_sha1(&metainfo.get_key("info"));
  const std::string actual_hash = torrent::utils::transform_to_hex_str(sha);
  if (actual_hash != hash)
    throw torrent::input_error("RR original triple hash changed.");
  return files;
}

std::string rr_original_mint_tx() {
  return core::random_hex16<torrent::input_error>(
      "RR original transaction random source unavailable.",
      "RR original transaction random read failed.");
}

torrent::Object rr_begin_original(core::Download* owner,
                                  const torrent::Object::list_type& args) {
  if (args.size() != 3)
    throw torrent::input_error("RR original begin requires local ID, user digest and run bit.");
  const auto target = rpc::make_target(owner);
  const std::string hash = rpc::call_command_string("d.hash", target);
  const std::string local_id = rpc::call_command_string("d.local_id", target);
  const std::string incarnation = rpc::call_command_string("d.incarnation", target);
  const std::string user_digest = args[1].as_string();
  const std::string run = args[2].as_string();
  if (args[0].as_string() != local_id || !core::valid_incarnation(incarnation) ||
      !rr_lower_hex(user_digest, 64) || (run != "0" && run != "1") ||
      (rpc::call_command_value("d.state", target) != 0) != (run == "1"))
    throw torrent::input_error("RR original begin identity invalid.");
  RrOriginalClaim prior;
  if (rr_original_claim_read(hash, &prior)) {
    if (prior.local_id != local_id || prior.incarnation != incarnation ||
        prior.user_digest != user_digest || prior.run != run)
      throw torrent::input_error("RR original begin foreign active generation.");
    uint64_t current_generation;
    try { current_generation = stop_intent::counter(session_thread::manager()->path(), hash, false); }
    catch (const std::exception& e) {
      throw torrent::input_error(std::string("RR original begin stop generation unavailable: ") + e.what());
    }
    if (current_generation != prior.stop_generation)
      throw torrent::input_error("RR original begin stop generation changed; source held.");
    const std::string marker = rr_original_v2_marker(prior);
    const std::string current = retrieve_d_custom(owner, "retrackers-recovery").as_string();
    const std::string ack = retrieve_d_custom(owner, "retrackers-recovery-ack").as_string();
    if ((!current.empty() && current != marker) || (!ack.empty() && ack != marker))
      throw torrent::input_error("RR original begin marker changed; source held.");
    if (current != marker || ack != marker) {
      RrLateReceipt late;
      if (rr_late_read_receipt(hash + ".claim." + prior.tx, &late) ||
          rr_committed_digest(rr_original_snapshot(owner)) != prior.baseline_digest)
        throw torrent::input_error("RR original begin premarker state changed; source held.");
      rr_original_read_triple(hash + ".preclaim", prior.baseline_digest, hash, incarnation);
      auto& custom = owner->bencode()->get_key("rtorrent").insert_preserve_copy(
          "custom", torrent::Object::create_map()).first->second;
      custom.insert_key("retrackers-recovery", marker);
      custom.insert_key("retrackers-recovery-ack", marker);
      session_thread::manager()->save_full_download_durable(owner);
    }
    return marker;
  }
  if (!stop_intent::held(owner).empty() || rr_journal_exists(hash + ".benc") ||
      rr_journal_exists(hash + ".released.receipt") ||
      !retrieve_d_custom(owner, "retrackers-recovery").as_string().empty() ||
      !retrieve_d_custom(owner, "retrackers-recovery-ack").as_string().empty())
    throw torrent::input_error("RR original begin blocked by existing authority or v1 marker.");
  session_thread::manager()->drain_download_saves(owner);
  const auto files = rr_original_snapshot(owner);
  RrOriginalClaim claim{hash, local_id, incarnation, rr_original_mint_tx(),
                        user_digest, run, 0, rr_committed_digest(files)};
  try { claim.stop_generation = stop_intent::counter(session_thread::manager()->path(), hash, false); }
  catch (const std::exception& e) {
    throw torrent::input_error(std::string("RR original begin stop generation unavailable: ") + e.what());
  }
  rr_original_write_triple(hash + ".preclaim", files);
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, hash + ".preclaim.receipt",
        "preclaim:" + hash + ":" + local_id + ":" + incarnation + ":" +
        claim.tx + ":" + run + ":" + user_digest + ":" +
        std::to_string(claim.stop_generation) + ":" + claim.baseline_digest + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR original preclaim directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
  if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
      std::string(cut) == "after-original-claim") ::_exit(137);
  const std::string marker = rr_original_v2_marker(claim);
  auto& custom = owner->bencode()->get_key("rtorrent").insert_preserve_copy(
          "custom", torrent::Object::create_map()).first->second;
  custom.insert_key("retrackers-recovery", marker);
  custom.insert_key("retrackers-recovery-ack", marker);
  session_thread::manager()->save_full_download_durable(owner);
  const int session = ::open(session_thread::manager()->path().c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR original begin session readback unavailable.");
  try {
    const auto saved = rr_committed_map(rr_session_read(session, hash + ".torrent.rtorrent"));
    if (!saved.has_key_string("incarnation") || saved.get_key_string("incarnation") != incarnation ||
        !saved.has_key_map("custom") ||
        saved.get_key("custom").get_key_string("retrackers-recovery") != marker ||
        saved.get_key("custom").get_key_string("retrackers-recovery-ack") != marker)
      throw torrent::input_error("RR original begin durable marker readback changed.");
  } catch (...) { ::close(session); throw; }
  ::close(session);
  return marker;
}

bool rr_late_save(core::Download* download) {
  const auto target = rpc::make_target(download);
  const std::string hash = rpc::call_command_string("d.hash", target);
  const std::string incarnation = rpc::call_command_string("d.incarnation", target);
  RrOriginalClaim claim;
  if (rr_original_claim_read(hash, &claim)) {
    if (claim.incarnation != incarnation)
      throw torrent::input_error("RR original claim save incarnation changed.");
    rr_late_save_to_prefix(download, hash + ".claim." + claim.tx, hash, incarnation);
    return true;
  }
  std::string owner_incarnation;
  bool retired = false;
  if (rr_owner_read(hash, &owner_incarnation, &retired)) {
    if (retired) return false;
    if (owner_incarnation != incarnation)
      throw torrent::input_error("RR owner save incarnation changed.");
    rr_late_save_to_prefix(download, rr_owner_prefix(hash, incarnation), hash, incarnation);
    return true;
  }
  std::string old_hash = retrackers_migration_target_source(hash);
  const auto released = rr_released_owners.find(hash);
  if (old_hash.empty() && released != rr_released_owners.end())
    old_hash = released->second.first;
  if (old_hash.empty() || !rr_migration_committed_receipt(old_hash, hash)) return false;
  if (rr_retired_present(old_hash)) return false;
  const auto state = rr_migration_read(old_hash);
  if (incarnation == state.source_incarnation ||
      (released != rr_released_owners.end() && released->second.second != incarnation))
    throw torrent::input_error("RR late save owner incarnation changed.");
  rr_late_save_to_prefix(download, old_hash, hash, incarnation);
  return true;
}


struct RrOriginalReleased {
  std::string incarnation;
  std::string tx;
  std::string digest;
  uint64_t baseline = 0;
};

bool rr_original_released_read(const std::string& hash, RrOriginalReleased* result) {
  if (!rr_journal_exists(hash + ".released.receipt")) return false;
  const std::string bytes = rr_migration_read_bytes(hash + ".released.receipt", 256);
  if (bytes.compare(0, sizeof("released-original:") - 1, "released-original:") != 0)
    return false;
  if (bytes.back() != '\n')
    throw torrent::input_error("RR original released receipt malformed.");
  std::vector<std::string> fields;
  size_t begin = 0;
  for (size_t end = 0; (end = bytes.find(':', begin)) != std::string::npos; begin = end + 1)
    fields.push_back(bytes.substr(begin, end - begin));
  fields.push_back(bytes.substr(begin, bytes.size() - begin - 1));
  if (fields.size() != 6 || fields[0] != "released-original" || fields[1] != hash ||
      !core::valid_incarnation(fields[2]) || !rr_lower_hex(fields[3], 32) ||
      (fields[4].size() != 40 || fields[4].find_first_not_of("0123456789ABCDEF") != std::string::npos) || fields[5].empty() || fields[5].size() > 18 ||
      !std::all_of(fields[5].begin(), fields[5].end(), ::isdigit))
    throw torrent::input_error("RR original released receipt identity malformed.");
  uint64_t baseline = 0;
  for (char digit : fields[5]) {
    if (baseline > (static_cast<uint64_t>(INT64_MAX) - (digit - '0')) / 10)
      throw torrent::input_error("RR original released baseline overflow.");
    baseline = baseline * 10 + digit - '0';
  }
  if (std::to_string(baseline) != fields[5])
    throw torrent::input_error("RR original released baseline noncanonical.");
  *result = {fields[2], fields[3], fields[4], baseline};
  return true;
}

void rr_original_publish_released(const RrOriginalClaim& claim,
    const std::vector<std::pair<std::string, std::string>>& files,
    uint64_t baseline) {
  rr_original_write_triple(claim.hash + ".released", files);
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, claim.hash + ".released.receipt",
        "released-original:" + claim.hash + ":" + claim.incarnation + ":" +
        claim.tx + ":" + rr_committed_digest(files) + ":" +
        std::to_string(baseline) + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR original release receipt directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
}

void rr_original_release_repair(const std::string& hash) {
  RrOriginalClaim claim;
  RrOriginalReleased released;
  if (!rr_original_claim_read(hash, &claim) ||
      !rr_original_released_read(hash, &released) ||
      released.incarnation != claim.incarnation || released.tx != claim.tx)
    throw torrent::input_error("RR original release claim/receipt mismatch.");
  auto files = rr_original_read_triple(hash + ".released", released.digest,
                                      hash, claim.incarnation);
  auto sidecar = rr_committed_map(files[1].second);
  if (!sidecar.has_key_map("custom") ||
      !sidecar.get_key("custom").has_key_string("retrackers-recovery") ||
      !sidecar.get_key("custom").has_key_string("retrackers-recovery-ack") ||
      !sidecar.get_key("custom").get_key_string("retrackers-recovery").empty() ||
      !sidecar.get_key("custom").get_key_string("retrackers-recovery-ack").empty())
    throw torrent::input_error("RR original released marker not cleared.");
  const std::string prefix = hash + ".claim." + claim.tx;
  RrLateReceipt late;
  uint64_t selected_baseline = released.baseline;
  if (rr_late_read_receipt(prefix, &late)) {
    if (late.new_hash != hash || late.incarnation != claim.incarnation ||
        late.sequence < released.baseline)
      throw torrent::input_error("RR original later-save authority changed.");
    if (late.sequence > released.baseline) {
      files = rr_late_select(prefix, hash, late, true);
      selected_baseline = late.sequence;
    }
  } else if (released.baseline != 0) {
    throw torrent::input_error("RR original release baseline disappeared.");
  }
  sidecar = rr_committed_map(files[1].second);
  uint64_t current_generation;
  try { current_generation = stop_intent::counter(session_thread::manager()->path(), hash, false); }
  catch (const std::exception& e) {
    throw torrent::input_error(std::string("RR original release stop generation unavailable: ") + e.what());
  }
  const int64_t saved_generation = sidecar.has_key_value("stop_generation") ?
      sidecar.get_key_value("stop_generation") : static_cast<int64_t>(claim.stop_generation);
  if (saved_generation < 0 || current_generation < static_cast<uint64_t>(saved_generation))
    throw torrent::input_error("RR original release stop generation regressed.");
  if (current_generation > static_cast<uint64_t>(saved_generation)) {
    sidecar.insert_key("state", int64_t(0));
    sidecar.insert_key("stop_generation", static_cast<int64_t>(current_generation));
    files[1].second = rr_bencode(sidecar);
    rr_original_publish_released(claim, files, selected_baseline);
  }
  const int session = ::open(session_thread::manager()->path().c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR original release session unavailable.");
  try {
    std::string ordinary_incarnation;
    try {
      const auto ordinary_sidecar = rr_committed_map(
          rr_session_read(session, hash + ".torrent.rtorrent"));
      if (ordinary_sidecar.has_key_string("incarnation") &&
          core::valid_incarnation(ordinary_sidecar.get_key_string("incarnation")))
        ordinary_incarnation = ordinary_sidecar.get_key_string("incarnation");
    } catch (const torrent::input_error&) { /* Missing or torn ordinary data has no authority. */ }
    if (!ordinary_incarnation.empty() && ordinary_incarnation != claim.incarnation)
      throw torrent::input_error("RR original release foreign same-hash incarnation held.");
    bool matching = false;
    try {
      std::vector<std::pair<std::string, std::string>> ordinary;
      for (const auto& file : files)
        ordinary.emplace_back(file.first, rr_session_read(session, hash + file.first));
      matching = rr_committed_digest(ordinary) == rr_committed_digest(files);
    } catch (const torrent::input_error&) { matching = false; }
    if (!matching) {
      for (const auto& file : files)
        rr_migration_write_file(session, hash + file.first, file.second);
      if (::fsync(session) < 0)
        throw torrent::input_error("RR original release ordinary fsync failed.");
    }
  } catch (...) { ::close(session); throw; }
  ::close(session);
}

std::set<std::string> rr_release_hold;
std::set<std::string> rr_unproven_release_receipts;
std::set<std::string> rr_unproven_finalized_receipts;

bool rr_release_present(const std::string& hash) {
  if (rr_unproven_release_receipts.count(hash)) return false;
  const int dir = rr_migration_dir();
  struct stat st;
  const int found = ::fstatat(dir, (hash + ".released.receipt").c_str(), &st, AT_SYMLINK_NOFOLLOW);
  const int lookup_error = errno;
  ::close(dir);
  if (found == 0) return true;
  if (lookup_error == ENOENT) return false;
  throw torrent::input_error("RR release receipt presence cannot be checked.");
}

void rr_release_repair(const std::string& hash) {
  RrOriginalReleased original;
  if (rr_original_released_read(hash, &original)) {
    rr_original_release_repair(hash);
    return;
  }
  const auto state = rr_migration_read(hash);
  const std::string receipt = rr_migration_read_bytes(hash + ".released.receipt", 256);
  std::vector<std::pair<std::string, std::string>> files;
  for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"}) {
    auto bytes = rr_migration_read_bytes(hash + ".released" + suffix, 64 * 1024 * 1024);
    rr_committed_map(bytes);
    files.push_back({suffix, std::move(bytes)});
  }
  const bool committed = rr_migration_committed_receipt(hash, state.expected_new_hash) &&
      !rr_migration_rollback_receipt(hash);
  const std::string owner_hash = committed ? state.expected_new_hash : hash;
  const auto sidecar = rr_committed_map(files[1].second);
  const std::string incarnation = sidecar.has_key_string("incarnation") ?
      sidecar.get_key_string("incarnation") : std::string();
  const auto separator = receipt.rfind(':');
  if (separator == std::string::npos || receipt.empty() || receipt.back() != '\n')
    throw torrent::input_error("RR released receipt sequence missing.");
  const std::string baseline_text = receipt.substr(separator + 1, receipt.size() - separator - 2);
  if (baseline_text.empty() || baseline_text.size() > 18 ||
      !std::all_of(baseline_text.begin(), baseline_text.end(), ::isdigit))
    throw torrent::input_error("RR released receipt sequence invalid.");
  uint64_t baseline = 0;
  for (char digit : baseline_text) baseline = baseline * 10 + (digit - '0');
  if (std::to_string(baseline) != baseline_text)
    throw torrent::input_error("RR released receipt sequence noncanonical.");
  const std::string expected = std::string(committed ? "released-committed:" : "released:") +
      hash + ":" + (committed ? owner_hash + ":" : std::string()) +
      incarnation + ":" + state.tx + ":" + rr_committed_digest(files) + ":" +
      baseline_text + "\n";
  if (receipt != expected || !core::valid_incarnation(incarnation) ||
      (committed ? incarnation == state.source_incarnation :
                   incarnation != state.source_incarnation))
    throw torrent::input_error("RR released owner receipt or authority mismatch.");
  const auto metainfo = rr_committed_map(files[0].second);
  if (!metainfo.has_key_map("info"))
    throw torrent::input_error("RR released source metainfo invalid.");
  const auto info_sha = torrent::object_sha1(&metainfo.get_key("info"));
  const std::string actual_hash = torrent::utils::transform_to_hex_str(info_sha);
  if (actual_hash != owner_hash || !sidecar.has_key_string("incarnation") ||
      sidecar.get_key_string("incarnation") != incarnation ||
      !sidecar.has_key_map("custom") ||
      !sidecar.get_key("custom").has_key_string("retrackers-recovery") ||
      !sidecar.get_key("custom").has_key_string("retrackers-recovery-ack") ||
      !sidecar.get_key("custom").get_key_string("retrackers-recovery").empty() ||
      !sidecar.get_key("custom").get_key_string("retrackers-recovery-ack").empty())
    throw torrent::input_error("RR released source identity or marker invalid.");
  if (committed) {
    RrLateReceipt late;
    if (rr_late_read_receipt(hash, &late)) {
      if (late.new_hash != owner_hash || late.incarnation != incarnation ||
          late.sequence < baseline)
        throw torrent::input_error("RR released later-save owner or sequence invalid.");
      if (late.sequence > baseline)
        files = rr_late_select(hash, owner_hash, late, true);
    } else if (baseline != 0) {
      throw torrent::input_error("RR released baseline late receipt disappeared.");
    }
  }
  const int reservation = rr_migration_dir();
  struct stat reserved;
  const int target_found = ::fstatat(reservation, (owner_hash + ".target").c_str(),
                                     &reserved, AT_SYMLINK_NOFOLLOW);
  const int target_error = errno;
  ::close(reservation);
  if (target_found < 0 && target_error != ENOENT)
    throw torrent::input_error("RR released reservation cannot be checked.");
  const int session = ::open(rpc::call_command_string("session.path").c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR released session directory unavailable.");
  try {
    bool ordinary_valid = false;
    if (target_found < 0) {
      try {
        const auto current_meta = rr_committed_map(rr_session_read(session, owner_hash + ".torrent"));
        const auto current_sidecar = rr_committed_map(rr_session_read(session, owner_hash + ".torrent.rtorrent"));
        const auto current_resume = rr_committed_map(rr_session_read(session, owner_hash + ".torrent.libtorrent_resume"));
        ordinary_valid = current_meta.has_key_map("info") &&
            rr_bencode(current_meta.get_key("info")) == rr_bencode(metainfo.get_key("info")) &&
            current_sidecar.has_key_string("incarnation") &&
            current_sidecar.get_key_string("incarnation") == incarnation &&
            current_sidecar.has_key_map("custom") &&
            current_sidecar.get_key("custom").has_key_string("retrackers-recovery") &&
            current_sidecar.get_key("custom").get_key_string("retrackers-recovery").empty() &&
            current_sidecar.get_key("custom").has_key_string("retrackers-recovery-ack") &&
            current_sidecar.get_key("custom").get_key_string("retrackers-recovery-ack").empty() &&
            current_resume.has_key_list("files") &&
            rr_committed_map(files[2].second).get_key("files").as_list().size() ==
                current_resume.get_key("files").as_list().size() &&
            rr_committed_digest({{".torrent", rr_session_read(session, owner_hash + ".torrent")},
                                 {".torrent.rtorrent", rr_session_read(session, owner_hash + ".torrent.rtorrent")},
                                 {".torrent.libtorrent_resume", rr_session_read(session, owner_hash + ".torrent.libtorrent_resume")}}) ==
                rr_committed_digest(files);
      } catch (const torrent::input_error&) { ordinary_valid = false; }
    }
    if (!ordinary_valid) {
      for (const auto& file : files)
        rr_migration_write_file(session, owner_hash + file.first, file.second);
      if (::fsync(session) < 0)
        throw torrent::input_error("RR released session directory fsync failed.");
    }
  } catch (...) { ::close(session); throw; }
  ::close(session);
  if (committed) rr_released_owners[owner_hash] = {hash, incarnation};
  if (target_found < 0) return;
  const int dir = rr_migration_dir();
  const int removed = ::unlinkat(dir, (owner_hash + ".target").c_str(), 0);
  const int remove_error = errno;
  if ((removed < 0 && remove_error != ENOENT) || ::fsync(dir) < 0) {
    ::close(dir);
    throw torrent::input_error("RR released reservation removal failed.");
  }
  ::close(dir);
}

bool rr_prepared_read(const std::string& hash, std::string* tx,
                      std::string* incarnation) {
  if (!rr_journal_exists(hash + ".prepared.receipt")) return false;
  const std::string bytes = rr_migration_read_bytes(hash + ".prepared.receipt", 160);
  const std::string head = "prepared:" + hash + ":";
  if (bytes.size() != head.size() + 32 + 1 + 32 + 1 ||
      bytes.compare(0, head.size(), head) != 0 ||
      bytes[head.size() + 32] != ':' || bytes.back() != '\n')
    throw torrent::input_error("RR prepared receipt invalid.");
  *tx = bytes.substr(head.size(), 32);
  *incarnation = bytes.substr(head.size() + 33, 32);
  if (!rr_lower_hex(*tx, 32) || !core::valid_incarnation(*incarnation))
    throw torrent::input_error("RR prepared receipt identity invalid.");
  return true;
}

void rr_prepare_publish(const std::string& hash, const std::string& tx,
                        const std::string& incarnation) {
  std::string previous_tx, previous_incarnation;
  if (rr_prepared_read(hash, &previous_tx, &previous_incarnation)) {
    if (previous_tx != tx || previous_incarnation != incarnation)
      throw torrent::input_error("RR prepared receipt owner changed.");
    return;
  }
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, hash + ".prepared.receipt",
        "prepared:" + hash + ":" + tx + ":" + incarnation + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR prepared receipt directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
}

bool rr_finalized_read(const std::string& hash, std::string* tx,
                       std::string* incarnation);
std::string rr_finalize_owner(core::Download* owner,
                              const torrent::Object::list_type& args);

void rr_complete_same_hash_release(core::Download* owner,
                                   const std::string& hash,
                                   const std::string& tx,
                                   const std::string& incarnation) {
  rr_prepare_publish(hash, tx, incarnation);
  if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
      std::string(cut) == "after-samehash-prepared") ::_exit(137);
  // This RPC runs on the daemon main thread. After the durable prepare,
  // removing wa fences every later callback owned by this worker.
  auto* storage = control->object_storage();
  const auto key = rpc::object_storage::key_type::from_c_str("rr.receipts.v1");
  const auto entry = storage->find(key);
  if (entry != storage->end()) {
    if (!(entry->second.flags & rpc::object_storage::flag_multi_type))
      throw torrent::input_error("RR worker ledger has wrong type.");
    std::vector<std::string> names;
    const std::string suffix = ":" + tx;
    for (const auto& item : entry->second.object.as_map())
      if (item.first.size() > suffix.size() &&
          item.first.compare(item.first.size() - suffix.size(), suffix.size(), suffix) == 0)
        names.push_back(item.first);
    for (const auto& name : names)
      storage->erase_str_multi_key("rr.receipts.v1", name);
  }
  rpc::call_command("schedule.remove", "rr-lf-" + tx);
  rpc::call_command("schedule.remove", "rr-rf-" + tx);
  torrent::Object::list_type final_args;
  final_args.emplace_back(hash);
  final_args.emplace_back(tx);
  final_args.emplace_back(incarnation);
  rr_finalize_owner(owner, final_args);
}

torrent::Object rr_finish_original(core::Download* owner,
                                   const torrent::Object::list_type& args) {
  if (args.size() != 2 || !rr_lower_hex(args[0].as_string(), 32) ||
      !core::valid_incarnation(args[1].as_string()))
    throw torrent::input_error("RR original finish requires transaction and incarnation.");
  const auto target = rpc::make_target(owner);
  const std::string hash = rpc::call_command_string("d.hash", target);
  const std::string incarnation = rpc::call_command_string("d.incarnation", target);
  const std::string tx = args[0].as_string();
  if (incarnation != args[1].as_string())
    throw torrent::input_error("RR original finish owner incarnation changed.");
  std::string finalized_tx, finalized_incarnation;
  if (rr_finalized_read(hash, &finalized_tx, &finalized_incarnation) &&
      finalized_tx == tx && finalized_incarnation == incarnation)
    return std::string("finalized");
  RrOriginalClaim claim;
  if (!rr_original_claim_read(hash, &claim) || claim.tx != tx ||
      claim.incarnation != incarnation ||
      claim.local_id != rpc::call_command_string("d.local_id", target))
    throw torrent::input_error("RR original finish claim identity changed.");
  if (rr_journal_exists(hash + ".benc") || !stop_intent::held(owner).empty())
    throw torrent::input_error("RR original finish blocked by capture or file-operation claim.");
  uint64_t current_generation;
  try { current_generation = stop_intent::counter(session_thread::manager()->path(), hash, false); }
  catch (const std::exception& e) {
    throw torrent::input_error(std::string("RR original finish stop generation unavailable: ") + e.what());
  }
  if (current_generation < claim.stop_generation ||
      (current_generation > claim.stop_generation &&
       rpc::call_command_value("d.state", target) != 0))
    throw torrent::input_error("RR original finish newer stop generation requires stopped owner.");
  RrOriginalReleased released;
  if (rr_original_released_read(hash, &released)) {
    rr_original_release_repair(hash);
    rr_complete_same_hash_release(owner, hash, tx, incarnation);
    return std::string("finalized");
  }
  const std::string marker = rr_original_v2_marker(claim);
  if (retrieve_d_custom(owner, "retrackers-recovery").as_string() != marker ||
      retrieve_d_custom(owner, "retrackers-recovery-ack").as_string() != marker)
    throw torrent::input_error("RR original finish marker or ack changed.");
  session_thread::manager()->drain_download_saves(owner);
  const std::string prefix = hash + ".claim." + tx;
  const uint64_t baseline = rr_late_save_to_prefix(owner, prefix, hash, incarnation);
  auto& custom = owner->bencode()->get_key("rtorrent").insert_preserve_copy(
          "custom", torrent::Object::create_map()).first->second;
  custom.insert_key("retrackers-recovery", std::string());
  custom.insert_key("retrackers-recovery-ack", std::string());
  try {
    rr_original_publish_released(claim, rr_original_snapshot(owner), baseline);
  } catch (...) {
    if (!rr_journal_exists(hash + ".released.receipt")) {
      custom.insert_key("retrackers-recovery", marker);
      custom.insert_key("retrackers-recovery-ack", marker);
    }
    throw;
  }
  if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut) {
    if (std::string(cut) == "after-original-release") ::_exit(137);
    if (std::string(cut) == "after-original-release-throw")
      throw torrent::input_error("RR test release-repair failure after durable receipt.");
  }
  rr_original_release_repair(hash);
  rr_complete_same_hash_release(owner, hash, tx, incarnation);
  return std::string("finalized");
}

torrent::Object rr_release_owner(core::Download* owner,
                                 const torrent::Object::list_type& args,
                                 const bool committed) {
  if (args.size() != 2)
    throw torrent::input_error("RR release requires transaction and incarnation.");
  const std::string owner_hash = rpc::call_command_string("d.hash", rpc::make_target(owner));
  const std::string old_hash = committed ? retrackers_migration_target_source(owner_hash) : owner_hash;
  if (old_hash.empty()) throw torrent::input_error("RR release reservation missing.");
  const auto state = rr_migration_read(old_hash);
  const std::string incarnation = rpc::call_command_string("d.incarnation", rpc::make_target(owner));
  if (args[0].as_string() != state.tx || args[1].as_string() != incarnation ||
      (committed ?
          (!rr_migration_committed_receipt(old_hash, owner_hash) ||
           incarnation == state.source_incarnation) :
          (!rr_migration_rollback_receipt(old_hash) ||
           incarnation != state.source_incarnation)))
    throw torrent::input_error("RR release owner or receipt mismatch.");
  if (rr_release_present(old_hash)) {
    rr_release_repair(old_hash);
    if (owner_hash == old_hash)
      rr_complete_same_hash_release(owner, old_hash, state.tx, incarnation);
    return std::string("released");
  }
  if (committed) {
    const auto sidecar = rr_committed_map(rr_migration_read_bytes(
        old_hash + ".committed.torrent.rtorrent", 64 * 1024 * 1024));
    if (!sidecar.has_key_string("incarnation") ||
        sidecar.get_key_string("incarnation") != incarnation)
      throw torrent::input_error("RR release committed incarnation changed.");
  }
  const std::string marker = committed ? rr_committed_marker(state) : state.claim;
  if (retrieve_d_custom(owner, "retrackers-recovery").as_string() != marker ||
      retrieve_d_custom(owner, "retrackers-recovery-ack").as_string() != marker)
    throw torrent::input_error("RR release marker changed.");
  if (!stop_intent::held(owner).empty())
    throw torrent::input_error("RR release blocked by file-operation claim.");
  const bool v2 = rr_capsule_v2(state);
  uint64_t owned_generation = static_cast<uint64_t>(state.stop_generation);
  if (v2 && !rr_quiesced_read(state, &owned_generation))
    throw torrent::input_error("RR v2 release missing owned-quiesce receipt.");
  if ((committed && state.claim.compare(0, sizeof("v1:original:1:") - 1,
                                        "v1:original:1:") == 0) ||
      (v2 && state.claim.compare(0, sizeof("v2:original:1:") - 1,
                                 "v2:original:1:") == 0)) {
    uint64_t current;
    try { current = stop_intent::counter(session_thread::manager()->path(), owner_hash, false); }
    catch (const std::exception& e) {
      throw torrent::input_error(std::string("RR release stop generation unavailable: ") + e.what());
    }
    if (current < owned_generation ||
        (current > owned_generation && rpc::call_command_value("d.state", rpc::make_target(owner)) != 0))
      throw torrent::input_error("RR release newer Stop or generation regression held.");
    if (current == owned_generation &&
        rpc::call_command_value("d.state", rpc::make_target(owner)) == 0) {
      const std::string token = stop_intent::token(
          rpc::call_command_string("d.local_id", rpc::make_target(owner)), current);
      const std::string prior_release_start = rr_native_release_start_hash;
      if (v2) rr_native_release_start_hash = owner_hash;
      torrent::Object started;
      try {
        started = rpc::call_command("d.start_if_stop_generation", token,
                                    rpc::make_target(owner));
      } catch (...) { rr_native_release_start_hash = prior_release_start; throw; }
      rr_native_release_start_hash = prior_release_start;
      if (started.as_value() != 1)
        throw torrent::input_error("RR release owner start CAS failed.");
    }
  }
  session_thread::manager()->drain_download_saves(owner);
  RrLateReceipt prior;
  const uint64_t baseline = rr_late_read_receipt(old_hash, &prior) ? prior.sequence : 0;
  if (baseline && (prior.new_hash != owner_hash || prior.incarnation != incarnation))
    throw torrent::input_error("RR release baseline late owner changed.");
  if (v2) {
    uint64_t snapshot_generation;
    try { snapshot_generation = stop_intent::counter(
        session_thread::manager()->path(), owner_hash, false); }
    catch (const std::exception& e) {
      throw torrent::input_error(std::string("RR release snapshot stop generation unavailable: ") + e.what());
    }
    if (snapshot_generation < owned_generation ||
        (snapshot_generation > owned_generation &&
         rpc::call_command_value("d.state", rpc::make_target(owner)) != 0))
      throw torrent::input_error("RR release snapshot newer Stop or generation regression held.");
    owner->bencode()->get_key("rtorrent").insert_key(
        "stop_generation", static_cast<int64_t>(snapshot_generation));
  }
  auto& custom = owner->bencode()->get_key("rtorrent").get_key("custom");
  custom.insert_key("retrackers-recovery", std::string());
  custom.insert_key("retrackers-recovery-ack", std::string());
  try {
    session::DownloadStorer storer(owner);
    storer.build_full_streams();
    auto torrent_stream = storer.torrent_stream();
    auto rtorrent_stream = storer.rtorrent_stream();
    auto resume_stream = storer.libtorrent_stream();
    const std::vector<std::pair<std::string, std::string>> files = {
      {".torrent", torrent_stream->str()},
      {".torrent.rtorrent", rtorrent_stream->str()},
      {".torrent.libtorrent_resume", resume_stream->str()}};
    const std::string journal = session_thread::manager()->path() + ".retrackers-migration/";
    session::DownloadStorer::save_and_move_streams(journal + old_hash + ".released.torrent",
        true, torrent_stream.get(), rtorrent_stream.get(), resume_stream.get());
    const int dir = rr_migration_dir();
    rr_unproven_release_receipts.insert(old_hash);
    try {
      rr_migration_write_file(dir, old_hash + ".released.receipt",
          std::string(committed ? "released-committed:" : "released:") + old_hash +
          ":" + (committed ? owner_hash + ":" : std::string()) + incarnation +
          ":" + state.tx + ":" + rr_committed_digest(files) + ":" +
          std::to_string(baseline) + "\n");
      static bool release_rename_test_cut_fired = false;
      if (const char* cut = ::getenv("RR_MIG_TEST_CUT"); cut &&
          std::string(cut) == "after-committed-release-rename-throw" &&
          !release_rename_test_cut_fired) {
        release_rename_test_cut_fired = true;
        throw torrent::input_error("RR test released receipt before directory fsync.");
      }
      if (::fsync(dir) < 0)
        throw torrent::input_error("RR released receipt directory fsync failed.");
      rr_unproven_release_receipts.erase(old_hash);
    } catch (...) { ::close(dir); throw; }
    ::close(dir);
  } catch (...) {
    if (!rr_release_present(old_hash)) {
      custom.insert_key("retrackers-recovery", marker);
      custom.insert_key("retrackers-recovery-ack", marker);
    }
    throw;
  }
  rr_release_repair(old_hash);
  if (owner_hash == old_hash)
    rr_complete_same_hash_release(owner, old_hash, state.tx, incarnation);
  return std::string("released");
}

torrent::Object rr_release_rolledback(core::Download* owner,
                                     const torrent::Object::list_type& args) {
  return rr_release_owner(owner, args, false);
}

torrent::Object rr_release_committed(core::Download* owner,
                                    const torrent::Object::list_type& args) {
  return rr_release_owner(owner, args, true);
}

void rr_retire_released(core::Download* owner) {
  const auto target = rpc::make_target(owner);
  const std::string owner_hash = rpc::call_command_string("d.hash", target);
  const auto mapping = rr_released_owners.find(owner_hash);
  const std::string incarnation = rpc::call_command_string("d.incarnation", target);
  std::string owner_incarnation;
  bool retired = false;
  if (rr_owner_read(owner_hash, &owner_incarnation, &retired)) {
    if (retired) return; // A fresh same-hash owner does not inherit the old tombstone.
    if (owner_incarnation != incarnation)
      throw torrent::input_error("RR erase owner pointer identity changed.");
    rr_owner_publish(owner_hash, incarnation, true);
    if (mapping != rr_released_owners.end()) rr_released_owners.erase(mapping);
    return;
  }
  if (mapping == rr_released_owners.end()) return;
  const std::string old_hash = mapping->second.first;
  if (mapping->second.second != incarnation)
    throw torrent::input_error("RR erase released owner identity changed.");
  if (!rr_release_present(old_hash))
    throw torrent::input_error("RR erase released receipt missing.");
  const int dir = rr_migration_dir();
  try {
    rr_migration_write_file(dir, old_hash + ".retired.receipt",
        "retired:" + old_hash + ":" + owner_hash + ":" + incarnation + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR erase retirement directory fsync failed.");
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
  rr_released_owners.erase(mapping);
}

void rr_retired_repair(const std::string& old_hash) {
  const auto state = rr_migration_read(old_hash);
  const std::string owner_hash = state.expected_new_hash;
  const auto sidecar = rr_committed_map(rr_migration_read_bytes(
      old_hash + ".released.torrent.rtorrent", 64 * 1024 * 1024));
  const std::string incarnation = sidecar.get_key_string("incarnation");
  const std::string expected = "retired:" + old_hash + ":" + owner_hash + ":" + incarnation + "\n";
  if (!core::valid_incarnation(incarnation) ||
      rr_migration_read_bytes(old_hash + ".retired.receipt", 160) != expected)
    throw torrent::input_error("RR retired owner receipt invalid.");
  const int session = ::open(rpc::call_command_string("session.path").c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) throw torrent::input_error("RR retired session directory unavailable.");
  try {
    struct stat st;
    if (::fstatat(session, (owner_hash + ".torrent").c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
      const auto ordinary = rr_committed_map(rr_session_read(session, owner_hash + ".torrent.rtorrent"));
      const std::string current = ordinary.get_key_string("incarnation");
      if (!core::valid_incarnation(current))
        throw torrent::input_error("RR retired ordinary incarnation invalid.");
      if (current == incarnation) {
        for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"}) {
          if (::unlinkat(session, (owner_hash + suffix).c_str(), 0) < 0 && errno != ENOENT)
            throw torrent::input_error("RR retired ordinary removal failed.");
        }
        if (::fsync(session) < 0)
          throw torrent::input_error("RR retired ordinary directory fsync failed.");
      }
    } else if (errno != ENOENT) {
      throw torrent::input_error("RR retired ordinary presence cannot be checked.");
    }
  } catch (...) { ::close(session); throw; }
  ::close(session);
}

bool rr_finalized_read(const std::string& hash, std::string* tx,
                       std::string* incarnation) {
  if (rr_unproven_finalized_receipts.count(hash)) return false;
  if (!rr_journal_exists(hash + ".finalized.receipt")) return false;
  const std::string bytes = rr_migration_read_bytes(hash + ".finalized.receipt", 160);
  const std::string head = "finalized:" + hash + ":";
  if (bytes.size() != head.size() + 32 + 1 + 32 + 1 ||
      bytes.compare(0, head.size(), head) != 0 ||
      bytes[head.size() + 32] != ':' || bytes.back() != '\n')
    throw torrent::input_error("RR finalized receipt invalid.");
  *tx = bytes.substr(head.size(), 32);
  *incarnation = bytes.substr(head.size() + 33, 32);
  if (!rr_lower_hex(*tx, 32) || !core::valid_incarnation(*incarnation))
    throw torrent::input_error("RR finalized receipt identity invalid.");
  return true;
}

// The finalized receipt is durable before any old transaction name is removed.
void rr_finalize_cleanup(const std::string& hash, const std::string& tx,
                         const std::string& incarnation) {
  RrOriginalClaim active;
  if (rr_original_claim_read(hash, &active) && active.tx != tx)
    return; // A newer T owns the fixed H-scoped preclaim and its private baseline.
  std::string current_tx, current_incarnation, pointer_incarnation;
  bool retired = false;
  if (!rr_finalized_read(hash, &current_tx, &current_incarnation) ||
      current_tx != tx || current_incarnation != incarnation ||
      !rr_owner_read(hash, &pointer_incarnation, &retired) ||
      pointer_incarnation != incarnation)
    throw torrent::input_error("RR finalized owner authority mismatch.");
  if (!retired) {
    RrLateReceipt backup;
    const std::string prefix = rr_owner_prefix(hash, incarnation);
    if (!rr_late_read_receipt(prefix, &backup) || backup.new_hash != hash ||
        backup.incarnation != incarnation)
      throw torrent::input_error("RR finalized owner backup missing.");
    rr_late_select(prefix, hash, backup, true);
  }
  if (rr_journal_exists(hash + ".benc")) {
    const auto state = rr_migration_read(hash);
    if (state.tx != tx) return; // A newer transaction owns these fixed names.
  }
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR final cleanup scan failed."); }
  const std::string head = hash + ".";
  std::vector<std::string> obsolete;
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.compare(0, head.size(), head) == 0 &&
        name.compare(0, hash.size() + 6, hash + ".owner") != 0 &&
        name != hash + ".finalized.receipt")
      obsolete.push_back(name);
  }
  for (const auto& name : obsolete)
    if (::unlinkat(fd, name.c_str(), 0) < 0 && errno != ENOENT) {
      ::closedir(directory);
      throw torrent::input_error("RR final cleanup removal failed.");
    }
  if (!obsolete.empty() && ::fsync(fd) < 0) {
    ::closedir(directory);
    throw torrent::input_error("RR final cleanup directory fsync failed.");
  }
  ::closedir(directory);
  const int prune_fd = rr_migration_dir();
  DIR* prune_directory = ::fdopendir(prune_fd);
  if (!prune_directory) { ::close(prune_fd); throw torrent::input_error("RR owner prune scan failed."); }
  const std::string owner_head = hash + ".owner.";
  std::vector<std::string> old_owner_files;
  while (auto* entry = ::readdir(prune_directory)) {
    const std::string name = entry->d_name;
    if (name.compare(0, owner_head.size(), owner_head) != 0 ||
        name.size() <= owner_head.size() + 32 ||
        name[owner_head.size() + 32] != '.') continue;
    const std::string other_incarnation = name.substr(owner_head.size(), 32);
    if (core::valid_incarnation(other_incarnation) && other_incarnation != incarnation)
      old_owner_files.push_back(name);
  }
  for (const auto& name : old_owner_files)
    if (::unlinkat(prune_fd, name.c_str(), 0) < 0 && errno != ENOENT) {
      ::closedir(prune_directory);
      throw torrent::input_error("RR old owner prune removal failed.");
    }
  if (!old_owner_files.empty() && ::fsync(prune_fd) < 0) {
    ::closedir(prune_directory);
    throw torrent::input_error("RR old owner prune directory fsync failed.");
  }
  ::closedir(prune_directory);
  rr_owner_hold_hashes.erase(hash);
}

void rr_finalize_repair_startup() {
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR final receipt scan failed."); }
  const std::string suffix = ".finalized.receipt";
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 40 + suffix.size() || name.compare(40, suffix.size(), suffix) != 0)
      continue;
    const std::string hash = name.substr(0, 40);
    if (!std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
          return std::isxdigit(c) && !std::islower(c); })) continue;
    try {
      std::string tx, incarnation;
      rr_finalized_read(hash, &tx, &incarnation);
      rr_finalize_cleanup(hash, tx, incarnation);
    } catch (const torrent::input_error& e) {
      rr_owner_hold_hashes.insert(hash);
      control->core()->push_log_std("RR finalized owner held: " + hash + ": " + e.what());
    }
  }
  ::closedir(directory);
}

void rr_owner_repair_startup() {
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR owner pointer scan failed."); }
  const std::string suffix = ".owner.current";
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 40 + suffix.size() || name.compare(40, suffix.size(), suffix) != 0)
      continue;
    const std::string hash = name.substr(0, 40);
    if (!std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
          return std::isxdigit(c) && !std::islower(c); })) continue;
    if (rr_owner_hold_hashes.count(hash)) continue;
    try {
      RrOriginalClaim active_claim;
      if (rr_original_claim_read(hash, &active_claim)) continue;
      if (rr_journal_exists(hash + ".benc") &&
          !rr_journal_exists(hash + ".prepared.receipt")) continue;
      std::string incarnation;
      bool retired = false;
      if (!rr_owner_read(hash, &incarnation, &retired)) continue;
      const int session = ::open(rpc::call_command_string("session.path").c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (session < 0) throw torrent::input_error("RR owner session directory unavailable.");
      try {
        bool ordinary_exists = false;
        struct stat st;
        if (::fstatat(session, (hash + ".torrent").c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
          ordinary_exists = true;
        else if (errno != ENOENT)
          throw torrent::input_error("RR owner ordinary presence cannot be checked.");
        std::string ordinary_incarnation;
        if (ordinary_exists) {
          try {
            const auto ordinary = rr_committed_map(
                rr_session_read(session, hash + ".torrent.rtorrent"));
            ordinary_incarnation = ordinary.get_key_string("incarnation");
            if (!core::valid_incarnation(ordinary_incarnation))
              throw torrent::input_error("RR owner ordinary incarnation invalid.");
          } catch (const torrent::input_error&) {
            if (retired) throw; // Erasure cannot identify a sidecar-less re-add.
            ordinary_incarnation.clear(); // Active owner can restore from its durable backup.
          }
        }
        if (retired) {
          if (ordinary_exists && ordinary_incarnation == incarnation) {
            for (const char* ext : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"})
              if (::unlinkat(session, (hash + ext).c_str(), 0) < 0 && errno != ENOENT)
                throw torrent::input_error("RR retired owner ordinary removal failed.");
            if (::fsync(session) < 0)
              throw torrent::input_error("RR retired owner directory fsync failed.");
          }
        } else {
          if (ordinary_exists && !ordinary_incarnation.empty() &&
              ordinary_incarnation != incarnation)
            throw torrent::input_error("RR owner ordinary generation changed.");
          const std::string prefix = rr_owner_prefix(hash, incarnation);
          RrLateReceipt backup;
          if (!rr_late_read_receipt(prefix, &backup) || backup.new_hash != hash ||
              backup.incarnation != incarnation)
            throw torrent::input_error("RR owner backup receipt missing.");
          const auto files = rr_late_select(prefix, hash, backup, true);
          bool ordinary_matches = ordinary_exists;
          if (ordinary_matches) {
            try {
              std::vector<std::pair<std::string, std::string>> current;
              for (const auto& file : files)
                current.push_back({file.first, rr_session_read(session, hash + file.first)});
              ordinary_matches = rr_committed_digest(current) == backup.digest;
            } catch (const torrent::input_error&) { ordinary_matches = false; }
          }
          if (!ordinary_matches) {
            for (const auto& file : files)
              rr_migration_write_file(session, hash + file.first, file.second);
            if (::fsync(session) < 0)
              throw torrent::input_error("RR owner ordinary repair fsync failed.");
          }
          rr_released_owners[hash] = {hash, incarnation};
        }
      } catch (...) { ::close(session); throw; }
      ::close(session);
    } catch (const torrent::input_error& e) {
      rr_owner_hold_hashes.insert(hash);
      control->core()->push_log_std("RR owner backup held: " + hash + ": " + e.what());
    }
  }
  ::closedir(directory);
}

std::string rr_finalize_owner(core::Download* owner,
                              const torrent::Object::list_type& args) {
  if (args.size() != 3) throw torrent::input_error("RR finalize requires old hash, transaction and incarnation.");
  const std::string& old_hash = args[0].as_string();
  const std::string& tx = args[1].as_string();
  const std::string& incarnation = args[2].as_string();
  if (old_hash.size() != 40 || !rr_lower_hex(tx, 32) ||
      !core::valid_incarnation(incarnation))
    throw torrent::input_error("RR finalize identity invalid.");
  const auto target = rpc::make_target(owner);
  const std::string owner_hash = rpc::call_command_string("d.hash", target);
  if (owner_hash != old_hash || rpc::call_command_string("d.incarnation", target) != incarnation)
    throw torrent::input_error("RR finalize owner changed.");
  std::string finalized_tx, finalized_incarnation;
  if (rr_finalized_read(old_hash, &finalized_tx, &finalized_incarnation)) {
    if (finalized_tx == tx && finalized_incarnation == incarnation) {
      rr_finalize_cleanup(old_hash, tx, incarnation);
      return "finalized";
    }
    RrOriginalClaim next_claim;
    RrOriginalReleased next_release;
    const bool next_original = rr_original_claim_read(old_hash, &next_claim) &&
        rr_original_released_read(old_hash, &next_release) &&
        next_claim.tx == tx && next_claim.incarnation == incarnation &&
        next_release.tx == tx && next_release.incarnation == incarnation;
    const bool next_capsule = rr_journal_exists(old_hash + ".benc") &&
        rr_migration_read(old_hash).tx == tx;
    if (!next_original && !next_capsule)
      throw torrent::input_error("RR finalize foreign generation refused.");
  }
  RrOriginalReleased original;
  if (rr_original_released_read(old_hash, &original)) {
    RrOriginalClaim claim;
    if (!rr_original_claim_read(old_hash, &claim) || claim.tx != tx ||
        claim.incarnation != incarnation || original.tx != tx ||
        original.incarnation != incarnation || owner_hash != old_hash)
      throw torrent::input_error("RR finalize original release owner mismatch.");
  } else {
    const auto state = rr_migration_read(old_hash);
    if (state.tx != tx || state.expected_new_hash != owner_hash ||
        !rr_release_present(old_hash))
      throw torrent::input_error("RR finalize released receipt mismatch.");
  }
  std::string prepared_tx, prepared_incarnation;
  if (!rr_prepared_read(old_hash, &prepared_tx, &prepared_incarnation) ||
      prepared_tx != tx || prepared_incarnation != incarnation)
    throw torrent::input_error("RR finalize prepared receipt missing or changed.");
  rr_release_repair(old_hash);
  const auto* ledger = control->object_storage();
  const auto key = rpc::object_storage::key_type::from_c_str("rr.receipts.v1");
  const auto entry = ledger->find(key);
  if (entry != ledger->end() &&
      (!(entry->second.flags & rpc::object_storage::flag_multi_type) ||
       entry->second.object.has_key("wa:" + tx)))
    throw torrent::input_error("RR finalize worker ledger still active.");
  if (!retrieve_d_custom(owner, "retrackers-recovery").as_string().empty() ||
      !retrieve_d_custom(owner, "retrackers-recovery-ack").as_string().empty())
    throw torrent::input_error("RR finalize marker still active.");
  rr_late_save_to_prefix(owner, rr_owner_prefix(owner_hash, incarnation), owner_hash, incarnation);
  rr_owner_publish(owner_hash, incarnation, false);
  const int dir = rr_migration_dir();
  rr_unproven_finalized_receipts.insert(old_hash);
  try {
    rr_migration_write_file(dir, old_hash + ".finalized.receipt",
        "finalized:" + old_hash + ":" + tx + ":" + incarnation + "\n");
    if (::fsync(dir) < 0)
      throw torrent::input_error("RR finalized receipt directory fsync failed.");
    rr_unproven_finalized_receipts.erase(old_hash);
  } catch (...) { ::close(dir); throw; }
  ::close(dir);
  rr_finalize_cleanup(old_hash, tx, incarnation);
  return "finalized";
}

bool rr_prepared_repair_loaded(core::Download* owner) {
  const std::string hash = rpc::call_command_string("d.hash", rpc::make_target(owner));
  try {
    const bool prepared = rr_journal_exists(hash + ".prepared.receipt");
    if (rr_committed_hold.count(hash) || rr_release_hold.count(hash) ||
        rr_owner_hold_hashes.count(hash)) return false;
    if (!prepared &&
        (!rr_migration_committed_receipt(hash, hash) || rr_release_present(hash)))
      return true;
    if (!prepared) {
      // The worker ledger is volatile; a cold load must release a durable same-hash commit.
      const auto state = rr_migration_read(hash);
      if (!rr_capsule_v2(state))
        throw torrent::input_error("RR committed loaded owner lacks v2 authority.");
      torrent::Object::list_type args;
      args.emplace_back(state.tx);
      args.emplace_back(rpc::call_command_string("d.incarnation", rpc::make_target(owner)));
      rr_release_committed(owner, args);
      return true;
    }
    std::string tx, incarnation;
    if (!rr_prepared_read(hash, &tx, &incarnation) ||
        rpc::call_command_string("d.incarnation", rpc::make_target(owner)) != incarnation)
      throw torrent::input_error("RR prepared owner missing or changed.");
    torrent::Object::list_type args;
    args.emplace_back(hash);
    args.emplace_back(tx);
    args.emplace_back(incarnation);
    RrOriginalReleased original;
    if (rr_original_released_read(hash, &original)) {
      rr_complete_same_hash_release(owner, hash, tx, incarnation);
    } else {
      const auto state = rr_migration_read(hash);
      if (state.expected_new_hash == hash && state.tx == tx && rr_release_present(hash))
        rr_complete_same_hash_release(owner, hash, tx, incarnation);
      else
        rr_finalize_owner(owner, args);
    }
    return true;
  } catch (const torrent::input_error& e) {
    rr_owner_hold_hashes.insert(hash);
    control->core()->push_log_std("RR session migration replay held: " + hash + ": " + e.what());
    return false;
  }
}

void rr_original_repair_preload() {
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR original claim scan failed."); }
  const std::string suffix = ".preclaim.receipt";
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 40 + suffix.size() ||
        name.compare(40, suffix.size(), suffix) != 0) continue;
    const std::string hash = name.substr(0, 40);
    if (!std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
          return std::isxdigit(c) && !std::islower(c); })) continue;
    try {
      RrOriginalClaim claim;
      if (!rr_original_claim_read(hash, &claim)) continue;
      bool captured = false;
      bool quiesced = false;
      uint64_t owned_generation = claim.stop_generation;
      if (rr_journal_exists(hash + ".benc")) {
        const auto state = rr_migration_read(hash);
        if (!rr_capsule_v2(state) || state.hash != hash ||
            state.source_incarnation != claim.incarnation ||
            state.source_local_id != claim.local_id || state.tx != claim.tx ||
            state.claim != rr_original_v2_marker(claim))
          throw torrent::input_error("RR original capture authority changed.");
        if (rr_migration_any_commit_receipt(hash)) continue;
        if (rr_migration_receipt_present(hash) && !rr_migration_rollback_receipt(hash))
          throw torrent::input_error("RR original capture receipt unrecognized.");
        quiesced = rr_quiesced_read(state, &owned_generation);
        captured = true;
      }
      RrOriginalReleased released;
      if (!rr_original_released_read(hash, &released)) {
        auto files = rr_original_read_triple(hash + ".preclaim",
            claim.baseline_digest, hash, claim.incarnation);
        uint64_t baseline = 0;
        const std::string prefix = hash + ".claim." + claim.tx;
        RrLateReceipt late;
        if (rr_late_read_receipt(prefix, &late)) {
          if (late.new_hash != hash || late.incarnation != claim.incarnation)
            throw torrent::input_error("RR original claim later-save identity changed.");
          files = rr_late_select(prefix, hash, late, false);
          baseline = late.sequence;
        }
        auto sidecar = rr_committed_map(files[1].second);
        auto& custom = sidecar.insert_preserve_copy(
            "custom", torrent::Object::create_map()).first->second;
        const std::string current = custom.has_key_string("retrackers-recovery") ?
            custom.get_key_string("retrackers-recovery") : std::string();
        const std::string ack = custom.has_key_string("retrackers-recovery-ack") ?
            custom.get_key_string("retrackers-recovery-ack") : std::string();
        const std::string marker = rr_original_v2_marker(claim);
        if ((!current.empty() && current != marker) || (!ack.empty() && ack != marker))
          throw torrent::input_error("RR original claim saved marker changed.");
        custom.insert_key("retrackers-recovery", std::string());
        custom.insert_key("retrackers-recovery-ack", std::string());
        uint64_t current_generation;
        try { current_generation = stop_intent::counter(session_thread::manager()->path(), hash, false); }
        catch (const std::exception& e) {
          throw torrent::input_error(std::string("RR original claim stop generation unavailable: ") + e.what());
        }
        if (current_generation < claim.stop_generation ||
            (quiesced && current_generation < owned_generation))
          throw torrent::input_error("RR original claim stop generation regressed.");
        if (captured && quiesced && current_generation == owned_generation) {
          // Only the fsynced quiesce receipt attributes Gq to the daemon.
          sidecar.insert_key("state", int64_t(claim.run == "1"));
        } else if (current_generation > claim.stop_generation) {
          sidecar.insert_key("state", int64_t(0));
        }
        // Before quiesce, the latest T-bound sidecar owns later Start/Open/Close.
        sidecar.insert_key("stop_generation", static_cast<int64_t>(current_generation));
        files[1].second = rr_bencode(sidecar);
        rr_original_publish_released(claim, files, baseline);
      }
      rr_original_release_repair(hash);
      rr_prepare_publish(hash, claim.tx, claim.incarnation);
    } catch (const torrent::input_error& e) {
      rr_owner_hold_hashes.insert(hash);
      control->core()->push_log_std("RR original preclaim held: " + hash + ": " + e.what());
    }
  }
  ::closedir(directory);
}

void rr_repair_released_startup_impl() {
  if (rpc::call_command_string("session.path").empty()) return;
  rr_original_repair_preload();
  rr_finalize_repair_startup();
  rr_owner_repair_startup();
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR released receipt scan failed."); }
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 40 + sizeof(".released.receipt") - 1 ||
        name.compare(40, sizeof(".released.receipt") - 1, ".released.receipt") != 0) continue;
    const std::string hash = name.substr(0, 40);
    if (!std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
          return std::isxdigit(c) && !std::islower(c); })) continue;
    try {
      RrOriginalReleased original;
      if (rr_original_released_read(hash, &original)) {
        RrOriginalClaim claim;
        if (!rr_original_claim_read(hash, &claim) || claim.tx != original.tx ||
            claim.incarnation != original.incarnation)
          throw torrent::input_error("RR original startup release claim changed.");
        rr_original_release_repair(hash);
        rr_prepare_publish(hash, claim.tx, claim.incarnation);
        continue;
      }
      if (rr_retired_present(hash)) {
        const auto state = rr_migration_read(hash);
        rr_retired_hold_hashes.insert(state.expected_new_hash);
        rr_retired_repair(hash);
        rr_retired_hold_hashes.erase(state.expected_new_hash);
      } else {
        const auto state = rr_migration_read(hash);
        std::string prepared_tx, prepared_incarnation, pointer_incarnation;
        bool retired = false;
        const bool prepared = rr_prepared_read(hash, &prepared_tx, &prepared_incarnation);
        const bool pointer = rr_owner_read(hash, &pointer_incarnation, &retired);
        if (prepared && (prepared_tx != state.tx ||
            (pointer && prepared_incarnation != pointer_incarnation)))
          throw torrent::input_error("RR prepared startup authority mismatch.");
        if (!(prepared && pointer && !retired && !rr_owner_hold_hashes.count(hash)))
          rr_release_repair(hash);
        if (state.expected_new_hash == hash && !prepared) {
          const auto sidecar = rr_committed_map(rr_migration_read_bytes(
              hash + ".released.torrent.rtorrent", 64 * 1024 * 1024));
          const std::string incarnation = sidecar.get_key_string("incarnation");
          if (!core::valid_incarnation(incarnation))
            throw torrent::input_error("RR released startup incarnation invalid.");
          rr_prepare_publish(hash, state.tx, incarnation);
        }
      }
    } catch (const torrent::input_error& e) {
      rr_release_hold.insert(hash);
      control->core()->push_log_std("RR released source held: " + hash + ": " + e.what());
    }
  }
  ::closedir(directory);
}

std::string rr_migration_worker_receipt(const std::string& old_hash) {
  if (old_hash.size() != 40 ||
      !std::all_of(old_hash.begin(), old_hash.end(), [](unsigned char c) {
        return std::isxdigit(c) && !std::islower(c);
      })) return "held";
  if (rr_release_hold.count(old_hash) || rr_retired_present(old_hash)) return "held";
  const bool released = rr_release_present(old_hash);
  const bool rolled_back = rr_migration_rollback_receipt(old_hash);
  const bool committed = !rolled_back && rr_migration_any_commit_receipt(old_hash);
  if (!rolled_back && !committed) return rr_migration_receipt_present(old_hash) ?
      "held" : (retrackers_migration_pending(old_hash) ? "pending" : "none");
  if (rr_committed_hold.count(old_hash)) return "held";
  const auto state = rr_migration_read(old_hash);
  if (state.tx.empty() || state.hash != old_hash ||
      (committed && state.expected_new_hash.empty())) return "held";
  const std::string owner_hash = rolled_back ? old_hash : state.expected_new_hash;
  if (released && !retrackers_migration_target_source(owner_hash).empty()) return "held";
  auto* owner = control->core()->download_list()->find_hex_ptr(owner_hash.c_str());
  if (!owner) return "held";
  const auto target = rpc::make_target(owner);
  const std::string incarnation = rpc::call_command_string("d.incarnation", target);
  if (rolled_back ? incarnation != state.source_incarnation :
                    incarnation == state.source_incarnation)
    return "held";
  RrLateReceipt late;
  const bool has_late = committed && rr_late_read_receipt(old_hash, &late);
  if (has_late && (late.new_hash != owner_hash || late.incarnation != incarnation))
    return "held";
  return "v1|" + std::string(released ? "released" :
      (rolled_back ? "rolledback" : "committed")) + "|" +
      old_hash + "|" + owner_hash + "|" + state.tx + "|" +
      state.source_incarnation + "|" + (rolled_back ? std::string() : incarnation) + "|" +
      state.source_local_id + "|" + state.claim + "|" + state.candidate_sha256 + "|" +
      std::to_string(has_late ? late.sequence : 0);
}

} // namespace

bool retrackers_migration_original_claim_active(const std::string& hash) {
  if (hash.size() != 40 || rpc::call_command_string("session.path").empty()) return false;
  RrOriginalClaim claim;
  return rr_original_claim_read(hash, &claim);
}

bool retrackers_migration_live_original(const std::string& hash,
                                         const std::string& source,
                                         const RrMigrationCapsule& state) {
  if (hash != source) return false;
  auto* live = control->core()->download_list()->find_hex_ptr(hash.c_str());
  return live && rpc::call_command_string("d.incarnation", rpc::make_target(live)) ==
      state.source_incarnation;
}

bool retrackers_migration_uncommitted_candidate(const std::string& hash) {
  const std::string source = retrackers_migration_target_source(hash);
  if (source.empty() || !retrackers_migration_pending(source)) return false;
  const auto state = rr_migration_read(source);
  return !retrackers_migration_live_original(hash, source, state);
}

bool retrackers_migration_candidate_activity_held(const std::string& hash) {
  if (hash == rr_native_release_start_hash) return false;
  const std::string source = retrackers_migration_target_source(hash);
  if (source.empty()) return false;
  const auto state = rr_migration_read(source);
  if (retrackers_migration_live_original(hash, source, state)) {
    if (!rr_capsule_v2(state)) return false;
    std::string tx, incarnation;
    if (rr_finalized_read(source, &tx, &incarnation) &&
        tx == state.tx && incarnation == state.source_incarnation) return false;
    // A durable Gq owns Stop until the exact original transaction is terminal.
    uint64_t generation;
    return rr_quiesced_read(state, &generation);
  }
  if (retrackers_migration_pending(source)) return true;
  return rr_capsule_v2(state) && !rr_release_present(source) &&
      state.expected_new_hash == hash;
}

bool retrackers_migration_late_save(core::Download* download) {
  return rr_late_save(download);
}

void retrackers_migration_retire_released(core::Download* download) {
  rr_retire_released(download);
}

void retrackers_migration_repair_released_startup() {
  rr_repair_released_startup_impl();
}

bool retrackers_migration_repair_prepared_loaded(core::Download* owner) {
  return rr_prepared_repair_loaded(owner);
}

void retrackers_migration_repair_committed_startup() {
  if (rpc::call_command_string("session.path").empty()) return;
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) { ::close(fd); throw torrent::input_error("RR committed receipt scan failed."); }
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 40 + sizeof(".receipt") - 1 ||
        name.compare(40, sizeof(".receipt") - 1, ".receipt") != 0) continue;
    const std::string old_hash = name.substr(0, 40);
    if (!std::all_of(old_hash.begin(), old_hash.end(),
          [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); })) continue;
    if (rr_migration_rollback_receipt(old_hash) || rr_release_present(old_hash)) continue;
    if (!rr_migration_any_commit_receipt(old_hash)) {
      rr_committed_hold.insert(old_hash);
      control->core()->push_log_std("RR migration receipt invalid; both owners held: " + old_hash);
      continue;
    }
    const std::string receipt = rr_migration_receipt(old_hash);
    const std::string new_hash = receipt.substr(std::string("committed:").size() + 40 + 1, 40);
    try {
      if (rr_late_repair(old_hash, new_hash)) {
        control->core()->push_log_std("RR late committed owner restored: " + new_hash);
        continue;
      }
    } catch (const torrent::input_error& e) {
      rr_committed_hold.insert(old_hash);
      control->core()->push_log_std("RR late committed owner held: " + old_hash + ": " + e.what());
      continue;
    }
    try {
      const auto state = rr_migration_read(old_hash);
      if (state.expected_new_hash != new_hash ||
          retrackers_migration_target_source(new_hash) != old_hash)
        throw torrent::input_error("RR committed receipt and target mismatch.");
      std::vector<std::pair<std::string, std::string>> authority;
      for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"}) {
        const std::string bytes = rr_migration_read_bytes(old_hash + ".committed" + suffix,
                                                          64 * 1024 * 1024);
        rr_committed_map(bytes);
        authority.push_back({suffix, bytes});
      }
      const std::string expected_digest = receipt.substr(
          std::string("committed:").size() + 40 + 1 + 40 + 1, 40);
      if (rr_committed_digest(authority) != expected_digest)
        throw torrent::input_error("RR committed authority digest mismatch.");
      const std::string path = rpc::call_command_string("session.path");
      const int session = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (session < 0) throw torrent::input_error("RR committed session directory unavailable.");
      try {
        if (!rr_committed_current_valid(session, new_hash, state, authority)) {
          for (const auto& file_bytes : authority)
            rr_migration_write_file(session, new_hash + file_bytes.first, file_bytes.second);
          if (::fsync(session) < 0)
            throw torrent::input_error("RR committed repair directory fsync failed.");
          control->core()->push_log_std("RR committed session repaired from durable authority: " + new_hash);
        }
      } catch (...) { ::close(session); throw; }
      ::close(session);
    } catch (const torrent::input_error& e) {
      try {
        if (rr_committed_reseed_from_ordinary(old_hash, new_hash)) {
          control->core()->push_log_std("RR committed authority reseeded from valid owner: " + new_hash);
          continue;
        }
        rr_committed_fallback_source(old_hash);
        control->core()->push_log_std("RR committed owner restored as source: " + old_hash +
                                      ": " + e.what());
      } catch (const torrent::input_error& fallback) {
        rr_committed_hold.insert(old_hash);
        control->core()->push_log_std("RR committed owner held: " + old_hash + ": " +
                                      e.what() + "; " + fallback.what());
      }
    }
  }
  ::closedir(directory);
}

void retrackers_migration_recover_startup() {
  if (rpc::call_command_string("session.path").empty()) return;
  const int fd = rr_migration_dir();
  DIR* directory = ::fdopendir(fd);
  if (!directory) {
    ::close(fd);
    throw torrent::input_error("RR migration cannot scan pending rollback snapshots.");
  }
  const std::string suffix = ".rollback.torrent";
  while (auto* entry = ::readdir(directory)) {
    const std::string name = entry->d_name;
    if (name.size() != 40 + suffix.size() || name.compare(40, suffix.size(), suffix) != 0)
      continue;
    const std::string hash = name.substr(0, 40);
    if (!std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
          return std::isxdigit(c) && !std::islower(c); }))
      continue;
    if (!retrackers_migration_pending(hash) || rr_owner_hold_hashes.count(hash) ||
        rr_journal_exists(hash + ".prepared.receipt")) continue;
    try {
      rr_migration_rollback(hash);
    } catch (const torrent::input_error& e) {
      control->core()->push_log_std("RR startup rollback refused: " + hash + ": " + e.what());
    }
  }
  ::closedir(directory);
}

std::string retrackers_migration_target_source(const std::string& hash) {
  if (hash.size() != 40 || rpc::call_command_string("session.path").empty()) return {};
  const int dir = rr_migration_dir();
  struct stat st;
  const int found = ::fstatat(dir, (hash + ".target").c_str(), &st, AT_SYMLINK_NOFOLLOW);
  const int lookup_error = errno;
  ::close(dir);
  if (found < 0 && lookup_error == ENOENT) return {};
  const std::string bytes = rr_migration_read_bytes(hash + ".target", 128);
  if (bytes.size() != 41 || bytes.back() != '\n' ||
      !std::all_of(bytes.begin(), bytes.begin() + 40,
          [](unsigned char c) { return std::isxdigit(c) && !std::islower(c); }))
    throw torrent::input_error("RR successor reservation invalid.");
  return bytes.substr(0, 40);
}

bool retrackers_migration_prepared_session(core::Download* download,
                                           const std::string& hash) {
  if (rr_release_hold.count(hash) || rr_owner_hold_hashes.count(hash)) return false;
  try {
    std::string tx, incarnation;
    if (!rr_prepared_read(hash, &tx, &incarnation) || !rr_release_present(hash))
      return false;
    const auto target = rpc::make_target(download);
    RrOriginalReleased original;
    if (rr_original_released_read(hash, &original)) {
      RrOriginalClaim claim;
      // libtorrent rotates local peer ID at restart; the private full triple binds H/I.
      return rr_original_claim_read(hash, &claim) && claim.tx == tx &&
          claim.incarnation == incarnation && original.tx == tx &&
          original.incarnation == incarnation &&
          rpc::call_command_string("d.incarnation", target) == incarnation &&
          retrieve_d_custom(download, "retrackers-recovery").as_string().empty() &&
          retrieve_d_custom(download, "retrackers-recovery-ack").as_string().empty();
    }
    const auto state = rr_migration_read(hash);
    return state.hash == hash && state.expected_new_hash == hash &&
        state.tx == tx && rpc::call_command_string("d.incarnation", target) == incarnation &&
        retrieve_d_custom(download, "retrackers-recovery").as_string().empty() &&
        retrieve_d_custom(download, "retrackers-recovery-ack").as_string().empty();
  } catch (const torrent::input_error&) { return false; }
}

bool retrackers_migration_source_reserved(const std::string& hash) {
  if (retrackers_migration_original_claim_active(hash)) return true;
  if (rr_retired_hold_hashes.count(hash) || rr_owner_hold_hashes.count(hash) ||
      rr_release_hold.count(hash)) return true;
  if (rpc::call_command_string("session.path").empty()) return false;
  if (rr_journal_exists(hash + ".prepared.receipt")) return true;
  const int dir = rr_migration_dir();
  struct stat st;
  const bool found = ::fstatat(dir, (hash + ".rollback.torrent").c_str(), &st,
                               AT_SYMLINK_NOFOLLOW) == 0;
  ::close(dir);
  if (found && rr_release_present(hash) && !rr_release_hold.count(hash) &&
      retrackers_migration_target_source(rr_migration_read(hash).expected_new_hash).empty())
    return false;
  return found && !rr_migration_rollback_receipt(hash);
}

bool retrackers_migration_committed(const std::string& old_hash,
                                    const std::string& new_hash) {
  return !rr_committed_hold.count(old_hash) && !rr_release_hold.count(old_hash) &&
         rr_migration_committed_receipt(old_hash, new_hash);
}

bool retrackers_migration_restored_source(const std::string& hash) {
  if (!rr_migration_rollback_receipt(hash)) return false;
  const int session = ::open(rpc::call_command_string("session.path").c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) return false;
  bool matches = true;
  try {
    for (const char* suffix : {".torrent", ".torrent.rtorrent", ".torrent.libtorrent_resume"})
      if (rr_session_read(session, hash + suffix) !=
          rr_migration_read_bytes(hash + ".rollback" + suffix, 64 * 1024 * 1024))
        matches = false;
  } catch (const torrent::input_error&) { matches = false; }
  ::close(session);
  return matches;
}

std::string retrackers_migration_candidate_digest(const std::string& hash) {
  return rr_migration_read(hash).candidate_sha256;
}

bool retrackers_migration_pending(const std::string& hash) {
  const std::string path = rpc::call_command_string("session.path");
  if (path.empty()) return false;
  const int session = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (session < 0) return false;
  const int dir = ::openat(session, ".retrackers-migration", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  ::close(session);
  if (dir < 0) return false;
  struct stat st;
  const bool pending = ::fstatat(dir, (hash + ".rollback.torrent").c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
  ::close(dir);
  return pending && !rr_migration_receipt_present(hash);
}

std::string
stop_with_generation(core::Download* download, bool require_durable = false) {
  const auto token = stop_intent::record(download, require_durable);
  stop_intent::ScopedSuppress suppress;
  rpc::parse_command_single(rpc::make_target(download), "view.set_visible=stopped");
  return token;
}

int64_t
set_directory_if_stop_close_claim(core::Download* download, const torrent::Object::list_type& args, bool base) {
  if (args.size() != 2 || !args[0].is_string() || !args[1].is_string())
    throw torrent::input_error("stop claim: expected token and directory");
  bool current = false;
  const auto claim = stop_intent::finish_claim(download, args[0].as_string(), current);
  // A later user Stop revokes activity restoration, not the moved payload's path.
  if (claim.token.empty()) return 0;
  if (base) download->set_base_directory(args[1].as_string());
  else download->set_directory(args[1].as_string());
  return 1;
}

int64_t
replay_stop_close_terminal(core::Download* download, const std::string& job) {
  auto claim = stop_intent::lease(download);
  if (claim.job != job) return 0;
  if (claim.phase != 'T' && claim.phase != 'D') return 0;
  const auto [session, hash] = stop_intent::identity(download);
  uint64_t generation;
  try { generation = stop_intent::counter(session, hash, false); }
  catch (const std::exception& e) { throw torrent::input_error(std::string("stop claim: ") + e.what()); }
  const bool current = stop_intent::token(claim.token.substr(0, 32), generation) == claim.token;
  if (claim.phase == 'D') {
    // Stopped/open is a runtime state; the durable DONE receipt permits only its owner to reopen it.
    if (current && claim.mode == 'O') {
      if (download->file_list()->root_dir() != claim.path ||
          rpc::call_command_value("d.state", rpc::make_target(download)) != 0)
        throw torrent::input_error("stop claim: DONE open replay found a different path or state");
      if (!download->is_open()) {
        stop_intent::ScopedOwnerRestore owner;
        rpc::call_command("d.open", torrent::Object(), rpc::make_target(download));
      }
    }
    return claim.mode == 'S' || claim.mode == 'O' ? 1 : 0;
  }
  if (current && (claim.mode == 'S' || claim.mode == 'O') && stop_intent::checker_owned(download))
    throw torrent::input_error("stop claim: checker ownership appeared before terminal replay");
  {
    stop_intent::ScopedOwnerRestore owner;
    // The final effective root survives a stale saved session independently of run intent.
    if (download->file_list()->root_dir() != claim.path)
      download->set_base_directory(claim.path);
    const auto target = rpc::make_target(download);
    if (current && claim.mode == 'S') {
      if (rpc::call_command_value("d.state", target) == 0)
        rpc::call_command("d.start", torrent::Object(), target);
      if (!download->is_active())
        control->core()->download_list()->resume_default(download);
    }
    if (current && claim.mode == 'O' && !download->is_open())
      rpc::call_command("d.open", torrent::Object(), target);
    if (claim.mode == 'S' && current && rpc::call_command_value("d.state", target) == 0)
      throw torrent::input_error("stop claim: terminal start did not restore run state");
    if (claim.mode == 'O' && current && !download->is_open())
      throw torrent::input_error("stop claim: terminal open did not restore open state");
  }
  session_thread::manager()->save_full_download_durable(download);
  stop_intent::publish_done(download, claim);
  return current && (claim.mode == 'S' || claim.mode == 'O') ? 1 : 0;
}

void
initialize_command_download() {
  CMD2_ANY_STRING("system.retrackers.rollback", [](auto, const auto& hash) { return rr_migration_rollback(hash); });
  CMD2_DL_LIST("d.retrackers.finalize", rr_finalize_owner);
  CMD2_ANY_STRING("system.retrackers.prepared", [](auto, const auto& hash) {
    std::string tx;
    std::string incarnation;
    return rr_prepared_read(hash, &tx, &incarnation) ?
        "prepared:" + tx + ":" + incarnation : std::string("none");
  });
  CMD2_ANY_STRING("system.retrackers.finalized", [](auto, const auto& hash) {
    std::string tx;
    std::string incarnation;
    return rr_finalized_read(hash, &tx, &incarnation) ?
        "finalized:" + tx + ":" + incarnation : std::string("none");
  });
  CMD2_DL_LIST("d.retrackers.begin_original", rr_begin_original);
  CMD2_DL_LIST("d.retrackers.finish_original", rr_finish_original);
  CMD2_DL_LIST("d.retrackers.release_rollback", rr_release_rolledback);
  CMD2_DL_LIST("d.retrackers.release_committed", rr_release_committed);
  CMD2_ANY_STRING("system.retrackers.receipt", [](auto, const auto& hash) {
    return rr_migration_worker_receipt(hash);
  });
  CMD2_ANY_STRING("system.retrackers.status", [](auto, const auto& hash) {
    std::string owner_incarnation;
    bool owner_retired = false;
    const bool owner_pointer = rr_owner_read(hash, &owner_incarnation, &owner_retired);
    std::string finalized_tx;
    std::string finalized_incarnation;
    const bool finalized = rr_finalized_read(hash, &finalized_tx, &finalized_incarnation);
    const bool prepared = rr_journal_exists(hash + ".prepared.receipt");
    RrOriginalClaim original_claim;
    if (rr_original_claim_read(hash, &original_claim))
      return rr_owner_hold_hashes.count(hash) ? std::string("original-held") :
          rr_release_present(hash) ? std::string(prepared ? "prepared-original" : "released-original") :
          std::string("claimed-original");
    return rr_owner_hold_hashes.count(hash) ? std::string("owner-held") :
           owner_pointer ? std::string(owner_retired ? "retired" :
               finalized ? "finalized" : "finalize-pending") :
           rr_release_hold.count(hash) ? std::string("release-held") :
           rr_retired_present(hash) ? std::string("retired") :
           rr_release_present(hash) ?
             (retrackers_migration_target_source(rr_migration_read(hash).expected_new_hash).empty() ?
               std::string(prepared ? "prepared" : "released") :
               std::string("release-pending")) :
           rr_migration_rollback_receipt(hash) ? std::string("rolledback") :
           rr_committed_hold.count(hash) ? std::string("committed-hold") :
           rr_migration_any_commit_receipt(hash) ? std::string("committed") :
           retrackers_migration_pending(hash) ? std::string("pending") : std::string("none");
  });
  CMD2_DL_LIST("d.retrackers.capture_erase", rr_migration_capture);
  CMD2_DL_LIST("d.retrackers.apply", rr_migration_apply);
  CMD2_DL_LIST("d.retrackers.commit", rr_migration_commit);
  CMD2_DL("d.hash",                           [](auto* download, auto) { return torrent::utils::transform_to_hex_str(download->info()->hash()); });
  CMD2_DL("d.incarnation",                    [](auto* download, auto) { return download->bencode()->get_key("rtorrent").get_key_string("incarnation"); });
  CMD2_DL("d.local_id",                       [](auto* download, auto) { return torrent::utils::transform_to_hex_str(download->info()->local_id()); });
  CMD2_DL("d.local_id_html",                  [](auto* download, auto) { return torrent::utils::copy_escape_html_str(download->info()->local_id()); });
  CMD2_DL("d.bitfield",                       [](auto* download, auto) { return torrent::utils::transform_to_hex_str(*download->download()->file_list()->bitfield()); });
  CMD2_DL("d.base_path",                      [](auto* download, auto) { return retrieve_d_base_path(download).str(); });
  CMD2_DL("d.base_path.hex",                  [](auto* download, auto) { return retrieve_d_base_path(download).object_hex(); });
  CMD2_DL("d.base_path.base64",               [](auto* download, auto) { return retrieve_d_base_path(download).object_base64(); });
  CMD2_DL("d.base_path.base64_as_binary",     [](auto* download, auto) { return retrieve_d_base_path(download).object_base64_as_binary(); });
  CMD2_DL("d.base_path.as_binary",            [](auto* download, auto) { return retrieve_d_base_path(download).object_as_binary(); });
  CMD2_DL("d.base_path.or_base64",            [](auto* download, auto) { return retrieve_d_base_path(download).object_utf8_or_base64(); });
  CMD2_DL("d.base_path.or_as_binary",         [](auto* download, auto) { return retrieve_d_base_path(download).object_utf8_or_as_binary(); });
  CMD2_DL("d.base_path.realpath.or_empty",    [](auto* download, auto) { return resolve_path(retrieve_d_base_path(download).str()); });
  CMD2_DL("d.base_path.realpath.or_throw",    [](auto* download, auto) { return resolve_path_or_throw(retrieve_d_base_path(download).str()); });
  CMD2_DL("d.base_filename",                  [](auto* download, auto) { return retrieve_d_base_filename(download).str(); });
  CMD2_DL("d.base_filename.hex",              [](auto* download, auto) { return retrieve_d_base_filename(download).object_hex(); });
  CMD2_DL("d.base_filename.base64",           [](auto* download, auto) { return retrieve_d_base_filename(download).object_base64(); });
  CMD2_DL("d.base_filename.base64_as_binary", [](auto* download, auto) { return retrieve_d_base_filename(download).object_base64_as_binary(); });
  CMD2_DL("d.base_filename.as_binary",        [](auto* download, auto) { return retrieve_d_base_filename(download).object_as_binary(); });
  CMD2_DL("d.base_filename.or_base64",        [](auto* download, auto) { return retrieve_d_base_filename(download).object_utf8_or_base64(); });
  CMD2_DL("d.base_filename.or_as_binary",     [](auto* download, auto) { return retrieve_d_base_filename(download).object_utf8_or_as_binary(); });

  CMD2_DL("d.name",                           [](auto* download, auto) { return download->info()->name().str(); });
  CMD2_DL("d.name.hex",                       [](auto* download, auto) { return download->info()->name().object_hex(); });
  CMD2_DL("d.name.base64",                    [](auto* download, auto) { return download->info()->name().object_base64(); });
  CMD2_DL("d.name.base64_as_binary",          [](auto* download, auto) { return download->info()->name().object_base64_as_binary(); });
  CMD2_DL("d.name.as_binary",                 [](auto* download, auto) { return download->info()->name().object_as_binary(); });
  CMD2_DL("d.name.or_base64",                 [](auto* download, auto) { return download->info()->name().object_utf8_or_base64(); });
  CMD2_DL("d.name.or_as_binary",              [](auto* download, auto) { return download->info()->name().object_utf8_or_as_binary(); });
  CMD2_DL("d.creation_date",                  [](auto* download, auto) { return download->info()->creation_date(); });
  CMD2_DL("d.load_date",                      [](auto* download, auto) { return download->info()->load_date(); });

  //
  // Network related:
  //

  CMD2_DL         ("d.up.rate",           [](auto* download, auto) { return download->info()->up_rate()->rate(); });
  CMD2_DL         ("d.up.total",          [](auto* download, auto) { return download->info()->up_rate()->total(); });
  CMD2_DL         ("d.down.rate",         [](auto* download, auto) { return download->info()->down_rate()->rate(); });
  CMD2_DL         ("d.down.total",        [](auto* download, auto) { return download->info()->down_rate()->total(); });
  CMD2_DL         ("d.skip.rate",         [](auto* download, auto) { return download->info()->skip_rate()->rate(); });
  CMD2_DL         ("d.skip.total",        [](auto* download, auto) { return download->info()->skip_rate()->total(); });

  CMD2_DL         ("d.peer_exchange",     [](auto* download, auto)       { return download->info()->is_pex_enabled(); });
  CMD2_DL_VALUE_V ("d.peer_exchange.set", [](auto* download, auto value) { download->download()->set_pex_enabled(value); });

  CMD2_DL_LIST    ("d.create_link", std::bind(&apply_d_change_link, std::placeholders::_1, std::placeholders::_2, 0));
  CMD2_DL_LIST    ("d.delete_link", std::bind(&apply_d_change_link, std::placeholders::_1, std::placeholders::_2, 1));
  CMD2_DL         ("d.delete_tied", std::bind(&apply_d_delete_tied, std::placeholders::_1));

  CMD2_DL("d.start", [](core::Download* download, const torrent::Object&) {
    if (retrackers_migration_candidate_activity_held(
        rpc::call_command_string("d.hash", rpc::make_target(download))))
      throw torrent::input_error("RR candidate start held until durable release.");
    stop_intent::require_unleased(download);
    return rpc::parse_command_multiple_std("d.hashing_failed.set=0 ;view.set_visible=started", rpc::make_target(download));
  });
  // Both entry points pass through the same daemon-owned durable intent barrier.
  CMD2_DL("d.stop", [](core::Download* download, const torrent::Object&) {
    if (retrackers_migration_uncommitted_candidate(
        rpc::call_command_string("d.hash", rpc::make_target(download))))
      throw torrent::input_error("RR candidate stop held until apply and commit.");
    stop_with_generation(download);
    return int64_t(0);
  });
  CMD2_DL("d.stop_claim", [](core::Download* download, const torrent::Object&) {
    stop_intent::require_unleased(download);
    return stop_with_generation(download, true);
  });
  CMD2_DL_STRING("d.stop_close_claim", [](core::Download* download, const std::string& job) {
    if (job.size() != 32 || job.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw torrent::input_error("stop claim: expected a 32-character lowercase hex job id");
    if (rpc::call_command_string("session.path").empty())
      throw torrent::input_error("stop claim: session directory required");
    const auto target = rpc::make_target(download);
    auto claim = stop_intent::lease(download);
    if (!claim.token.empty()) {
      if (claim.job != job) throw torrent::input_error("stop claim: reserved by another job");
      if (claim.phase == 'T' || claim.phase == 'D')
        return std::string(claim.phase == 'D' ? "done|" : "terminal|") +
               claim.mode + "|" + stop_intent::receipt(claim);
    } else {
      if (stop_intent::checker_owned(download))
        throw torrent::input_error("stop claim: checker ownership is active");
      const int state = rpc::call_command_value("d.state", target) != 0;
      const int open = download->is_open();
      const int active = download->is_active();
      const auto local_id = rpc::call_command_string("d.local_id", target);
      const auto stop_token = stop_intent::next_claim_token(download, local_id);
      claim = stop_intent::create_lease(download, stop_token, job, state, open, active, local_id);
    }
    stop_intent::ensure_claim_generation(download, claim);
    // A retry after SIGKILL can see the saved session's stale open/running state.
    stop_intent::ScopedSuppress suppress;
    rpc::parse_command_single(target, "view.set_visible=stopped");
    rpc::call_command("d.close", torrent::Object(), target);
    if (rpc::call_command_value("d.state", target) != 0 || download->is_open() || download->is_active())
      throw torrent::input_error("stop claim: Stop+Close did not quiesce download");
    return stop_intent::receipt(claim);
  });
  CMD2_DL_STRING("d.stop_close_claim_state", [](core::Download* download, const std::string& job) {
    if (job.size() != 32 || job.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw torrent::input_error("stop claim: expected a 32-character lowercase hex job id");
    const auto claim = stop_intent::lease(download);
    if (claim.token.empty()) return std::string("absent");
    if (claim.job != job) throw torrent::input_error("stop claim: reserved by another job");
    const std::string status = claim.phase == 'D' ? "done|" : claim.phase == 'T' ? "terminal|" : "active|";
    return status + (claim.phase == 'A' ? "" : std::string(1, claim.mode) + "|") +
           stop_intent::receipt(claim);
  });
  CMD2_DL_STRING("d.replay_stop_close_claim", [](core::Download* download, const std::string& job) {
    return replay_stop_close_terminal(download, job);
  });
  CMD2_DL_STRING("d.start_if_stop_close_claim", [](core::Download* download, const std::string& expected) {
    auto prior = stop_intent::lease(download);
    if ((prior.phase == 'T' || prior.phase == 'D') && prior.token == expected)
      return replay_stop_close_terminal(download, prior.job);
    bool current = false;
    const auto claim = stop_intent::finish_claim(download, expected, current);
    if (claim.token.empty()) return int64_t(0);
    if (claim.state != 1) throw torrent::input_error("stop claim: original download was not started");
    const char mode = current ? 'S' : 'K';
    stop_intent::publish_terminal(download, claim, mode, download->file_list()->root_dir());
    return replay_stop_close_terminal(download, claim.job);
  });
  CMD2_DL_STRING("d.open_if_stop_close_claim", [](core::Download* download, const std::string& expected) {
    auto prior = stop_intent::lease(download);
    if ((prior.phase == 'T' || prior.phase == 'D') && prior.token == expected)
      return replay_stop_close_terminal(download, prior.job);
    bool current = false;
    const auto claim = stop_intent::finish_claim(download, expected, current);
    if (claim.token.empty()) return int64_t(0);
    if (claim.state != 0 || claim.open != 1 || claim.active != 0)
      throw torrent::input_error("stop claim: original download was not stopped and open");
    const char mode = current ? 'O' : 'K';
    stop_intent::publish_terminal(download, claim, mode, download->file_list()->root_dir());
    return replay_stop_close_terminal(download, claim.job);
  });
  CMD2_DL_STRING("d.release_stop_close_claim", [](core::Download* download, const std::string& expected) {
    auto claim = stop_intent::lease(download);
    if (claim.token != expected || expected.empty()) return int64_t(0);
    if (claim.phase == 'A')
      stop_intent::publish_terminal(download, claim, 'R', download->file_list()->root_dir());
    replay_stop_close_terminal(download, claim.job);
    return int64_t(1);
  });
  CMD2_DL_STRING("d.ack_stop_close_claim", [](core::Download* download, const std::string& job) {
    const auto claim = stop_intent::lease(download);
    if (claim.token.empty()) return int64_t(0);
    if (claim.job != job) throw torrent::input_error("stop claim: reserved by another job");
    if (claim.phase != 'D') throw torrent::input_error("stop claim: terminal save is not complete");
    return int64_t(stop_intent::release_lease(download, claim.token));
  });
  CMD2_DL("d.stop_generation", [](core::Download* download, const torrent::Object&) {
    auto target = rpc::make_target(download);
    try { return stop_intent::token(rpc::call_command_string("d.local_id", target),
      stop_intent::counter(session_thread::manager()->path(), rpc::call_command_string("d.hash", target), false)); }
    catch (const std::exception& e) { throw torrent::input_error(std::string("stop intent: ") + e.what()); }
  });
  CMD2_DL_STRING("d.start_if_stop_generation", [](core::Download* download, const std::string& expected) {
    auto target = rpc::make_target(download);
    std::string current;
    try { current = stop_intent::token(rpc::call_command_string("d.local_id", target),
      stop_intent::counter(session_thread::manager()->path(), rpc::call_command_string("d.hash", target), false)); }
    catch (const std::exception& e) { throw torrent::input_error(std::string("stop intent: ") + e.what()); }
    if (!stop_intent::held(download).empty() || expected != current || expected.empty() ||
        expected.substr(expected.size() - 16) == "0000000000000000" || rpc::call_command_value("d.state", target) != 0)
      return int64_t(0);
    rpc::call_command("d.start", torrent::Object(), target);
    return int64_t(1);
  });
  CMD2_DL("d.try_start", [](core::Download* download, const torrent::Object&) {
    if (retrackers_migration_candidate_activity_held(
        rpc::call_command_string("d.hash", rpc::make_target(download))))
      throw torrent::input_error("RR candidate start held until durable release.");
    stop_intent::require_unleased(download);
    return rpc::parse_command_single(rpc::make_target(download),
      "branch=\"or={d.hashing_failed=,d.ignore_commands=}\",{},{view.set_visible=started}");
  });
  CMD2_FUNC_SINGLE("d.try_stop",  "branch=d.ignore_commands=, {}, {view.set_visible=stopped}");
  CMD2_DL("d.try_close", [](core::Download* download, const torrent::Object&) {
    auto target = rpc::make_target(download);
    if (rpc::call_command_value("d.ignore_commands", target) != 0)
      return torrent::Object();
    stop_intent::record(download);
    stop_intent::ScopedSuppress suppress;
    rpc::parse_command_single(target, "view.set_visible=stopped");
    rpc::call_command("d.close", torrent::Object(), target);
    return torrent::Object();
  });

  //
  // Control functinos:
  //

  CMD2_DL         ("d.is_open",               [](auto* download, auto) { return download->info()->is_open(); });
  CMD2_DL         ("d.is_active",             [](auto* download, auto) { return download->info()->is_active(); });
  CMD2_DL         ("d.is_hash_checked",       std::bind(&torrent::Download::is_hash_checked, CMD2_BIND_DL));
  CMD2_DL         ("d.is_hash_checking",      std::bind(&torrent::Download::is_hash_checking, CMD2_BIND_DL));
  CMD2_DL         ("d.is_multi_file",         std::bind(&torrent::FileList::is_multi_file, CMD2_BIND_FL));
  CMD2_DL         ("d.is_private",            [](auto* download, auto) { return download->info()->is_private(); });
  CMD2_DL         ("d.is_pex_active",         [](auto* download, auto) { return download->info()->is_pex_active(); });
  CMD2_DL         ("d.is_partially_done",     CMD2_ON_DATA(is_partially_done));
  CMD2_DL         ("d.is_not_partially_done", CMD2_ON_DATA(is_not_partially_done));
  CMD2_DL         ("d.is_meta",               [](auto* download, auto) { return download->info()->is_meta_download(); });

  CMD2_DL_V("d.resume", [](core::Download* download, const torrent::Object&) {
    stop_intent::require_unleased(download);
    control->core()->download_list()->resume_default(download);
  });
  CMD2_DL_V("d.pause", [](core::Download* download, const torrent::Object&) {
    if (!stop_intent::suppressed()) stop_intent::record(download);
    stop_intent::ScopedSuppress suppress;
    control->core()->download_list()->pause_default(download);
  });
  CMD2_DL_V("d.open", [](core::Download* download, const torrent::Object&) {
    stop_intent::require_unleased(download);
    control->core()->download_list()->open_throw(download);
  });
  CMD2_DL_V("d.close", [](core::Download* download, const torrent::Object&) {
    if (!stop_intent::suppressed()) stop_intent::record(download);
    stop_intent::ScopedSuppress suppress;
    control->core()->download_list()->close_throw(download);
  });
  CMD2_DL_V("d.close.directly", [](core::Download* download, const torrent::Object&) {
    if (!stop_intent::suppressed()) stop_intent::record(download);
    stop_intent::ScopedSuppress suppress;
    control->core()->download_list()->close_files(download);
  });
  CMD2_DL_V       ("d.erase",      std::bind(&core::DownloadList::erase_ptr, control->core()->download_list(), std::placeholders::_1));
  CMD2_DL_V("d.check_hash", [](core::Download* download, const torrent::Object&) {
    stop_intent::require_unleased(download);
    control->core()->download_list()->check_hash(download);
  });

  CMD2_DL_V       ("d.save_resume",       [](core::Download* download, auto) { session_thread::manager()->save_resume_download(download); });
  CMD2_DL_V       ("d.save_full_session", [](core::Download* download, auto) {
    session_thread::manager()->save_full_download(download);
  });

  CMD2_DL_V       ("d.update_priorities", CMD2_ON_DL(update_priorities));

  CMD2_DL_STRING_V("add_peer",   std::bind(&apply_d_add_peer, std::placeholders::_1, std::placeholders::_2));

  //
  // Custom settings:
  //

  CMD2_DL_STRING("d.custom",       std::bind(&retrieve_d_custom, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_STRING("d.custom_throw", std::bind(&retrieve_d_custom_throw, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_LIST  ("d.custom.set",   std::bind(&apply_d_custom, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_LIST  ("d.custom.if_z",  std::bind(&retrieve_d_custom_if_z, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_LIST  ("d.custom.keys",  std::bind(&retrieve_d_custom_map, std::placeholders::_1, true, std::placeholders::_2));
  CMD2_DL_LIST  ("d.custom.items", std::bind(&retrieve_d_custom_map, std::placeholders::_1, false, std::placeholders::_2));

  CMD2_DL("d.custom1", std::bind(&download_get_variable, std::placeholders::_1, "rtorrent", "custom1"));
  CMD2_DL_STRING("d.custom1.set", [](core::Download* download, const std::string& value) {
    if (stop_intent::checker_service_label(value) && !stop_intent::held(download).empty())
      throw torrent::input_error("stop claim: checker service label refused during file operation");
    return download_set_variable_string(download, value, "rtorrent", "custom1");
  });
  CMD2_DL_VAR_STRING_PUBLIC("d.custom2", "rtorrent", "custom2");
  CMD2_DL_VAR_STRING_PUBLIC("d.custom3", "rtorrent", "custom3");
  CMD2_DL_VAR_STRING_PUBLIC("d.custom4", "rtorrent", "custom4");
  CMD2_DL_VAR_STRING_PUBLIC("d.custom5", "rtorrent", "custom5");

  // 0 - stopped
  // 1 - started
  CMD2_DL("d.state", std::bind(&download_get_variable, std::placeholders::_1, "rtorrent", "state"));
  CMD2_DL_VALUE_P("d.state.set", [](core::Download* download, int64_t value) {
    if (value != 0 && !(value == 1 && stop_intent::terminal_activity_allowed(download)))
      stop_intent::require_unleased(download);
    return download_set_variable_value(download, value, "rtorrent", "state");
  });
  CMD2_DL_VAR_VALUE("d.complete",   "rtorrent", "complete");

  CMD2_FUNC_SINGLE ("d.incomplete", "not=(d.complete)");

  // 0 off
  // 1 scheduled, being controlled by a download scheduler. Includes a priority.
  // 3 forced off
  // 2 forced on
  CMD2_DL_VAR_VALUE("d.mode", "rtorrent", "mode");

  // 0 - Not hashing
  // 1 - Normal hashing
  // 2 - Download finished, hashing
  // 3 - Rehashing
  CMD2_DL_VAR_VALUE("d.hashing", "rtorrent", "hashing");

  // 'tied_to_file' is the file the download is associated with, and
  // can be changed by the user.
  //
  // 'loaded_file' is the file this instance of the torrent was loaded
  // from, and should not be changed.
  CMD2_DL_VAR_STRING_PUBLIC("d.tied_to_file", "rtorrent", "tied_to_file");
  CMD2_DL_VAR_STRING("d.loaded_file",  "rtorrent", "loaded_file");

  CMD2_DL("d.tied_to_file.realpath.or_empty", [](auto* download, auto) { return resolve_path(rpc::convert_to_string(download_get_variable(download, "rtorrent", "tied_to_file"))); });
  CMD2_DL("d.tied_to_file.realpath.or_throw", [](auto* download, auto) { return resolve_path_or_throw(rpc::convert_to_string(download_get_variable(download, "rtorrent", "tied_to_file"))); });
  CMD2_DL("d.loaded_file.realpath.or_empty",  [](auto* download, auto) { return resolve_path(rpc::convert_to_string(download_get_variable(download, "rtorrent", "loaded_file"))); });
  CMD2_DL("d.loaded_file.realpath.or_throw",  [](auto* download, auto) { return resolve_path_or_throw(rpc::convert_to_string(download_get_variable(download, "rtorrent", "loaded_file"))); });

  // The "state_changed" variable is required to be a valid unix time
  // value, it indicates the last time the torrent changed its state,
  // resume/pause.
  CMD2_DL_VAR_VALUE("d.state_changed",          "rtorrent", "state_changed");
  CMD2_DL_VAR_VALUE("d.state_counter",          "rtorrent", "state_counter");
  CMD2_DL_VAR_VALUE_PUBLIC("d.ignore_commands", "rtorrent", "ignore_commands");

  CMD2_DL_TIMESTAMP("d.timestamp.started",      "rtorrent", "timestamp.started");
  CMD2_DL_TIMESTAMP("d.timestamp.finished",     "rtorrent", "timestamp.finished");

  CMD2_DL         ("d.connection_current",     [](auto* d, auto)     { return torrent::option_to_c_str_or_throw(torrent::OPTION_CONNECTION_TYPE, d->download()->connection_type()); });
  CMD2_DL_STRING_V("d.connection_current.set", [](auto* d, auto arg) { apply_d_connection_type(d, arg); });

  CMD2_DL_VAR_STRING("d.connection_leech",      "rtorrent", "connection_leech");
  CMD2_DL_VAR_STRING("d.connection_seed",       "rtorrent", "connection_seed");

  CMD2_DL       ("d.up.choke_heuristics",       [](auto* d, auto)     { return torrent::option_to_c_str_or_throw(torrent::OPTION_CHOKE_HEURISTICS, d->download()->upload_choke_heuristic()); });
  CMD2_DL_STRING("d.up.choke_heuristics.set",   std::bind(&apply_d_choke_heuristics, std::placeholders::_1, std::placeholders::_2, false));
  CMD2_DL       ("d.down.choke_heuristics",     [](auto* d, auto)     { return torrent::option_to_c_str_or_throw(torrent::OPTION_CHOKE_HEURISTICS, d->download()->download_choke_heuristic()); });
  CMD2_DL_STRING("d.down.choke_heuristics.set", std::bind(&apply_d_choke_heuristics, std::placeholders::_1, std::placeholders::_2, true));

  CMD2_DL_VAR_STRING("d.up.choke_heuristics.leech", "rtorrent", "choke_heuristics.up.leech");
  CMD2_DL_VAR_STRING("d.up.choke_heuristics.seed",  "rtorrent", "choke_heuristics.up.seed");
  CMD2_DL_VAR_STRING("d.down.choke_heuristics.leech", "rtorrent", "choke_heuristics.down.leech");
  CMD2_DL_VAR_STRING("d.down.choke_heuristics.seed",  "rtorrent", "choke_heuristics.down.seed");

  CMD2_DL         ("d.hashing_failed",     std::bind(&core::Download::is_hash_failed, std::placeholders::_1));
  CMD2_DL_VALUE_V ("d.hashing_failed.set", std::bind(&core::Download::set_hash_failed, std::placeholders::_1, std::placeholders::_2));

  CMD2_DL         ("d.views",                  std::bind(&download_get_variable, std::placeholders::_1, "rtorrent", "views"));
  CMD2_DL         ("d.views.has",              std::bind(&d_list_has, std::placeholders::_1, std::placeholders::_2, "rtorrent", "views"));
  CMD2_DL         ("d.views.remove",           std::bind(&d_list_remove, std::placeholders::_1, std::placeholders::_2, "rtorrent", "views"));
  CMD2_DL_STRING  ("d.views.push_back",        std::bind(&d_list_push_back_string, std::placeholders::_1, std::placeholders::_2, "rtorrent", "views"));
  CMD2_DL_STRING  ("d.views.push_back_unique", std::bind(&d_list_push_back_unique_string, std::placeholders::_1, std::placeholders::_2, "rtorrent", "views"));

  // This command really needs to be improved, so we have proper
  // logging support.
  CMD2_DL         ("d.message",     std::bind(&core::Download::message, std::placeholders::_1));
  CMD2_DL_STRING_V("d.message.set", std::bind(&core::Download::set_message, std::placeholders::_1, std::placeholders::_2));

  CMD2_DL         ("d.max_file_size",       CMD2_ON_FL(max_file_size));
  CMD2_DL_VALUE_V ("d.max_file_size.set",   std::bind(&torrent::FileList::set_max_file_size, CMD2_BIND_FL, std::placeholders::_2));

  CMD2_DL         ("d.peers_min",           std::bind(&torrent::ConnectionList::min_size, CMD2_BIND_CL));
  CMD2_DL_VALUE_V ("d.peers_min.set",       std::bind(&torrent::ConnectionList::set_min_size, CMD2_BIND_CL, std::placeholders::_2));
  CMD2_DL         ("d.peers_max",           std::bind(&torrent::ConnectionList::max_size, CMD2_BIND_CL));
  CMD2_DL_VALUE_V ("d.peers_max.set",       std::bind(&torrent::ConnectionList::set_max_size, CMD2_BIND_CL, std::placeholders::_2));
  CMD2_DL         ("d.uploads_max",         std::bind(&torrent::Download::uploads_max, CMD2_BIND_DL));
  CMD2_DL_VALUE_V ("d.uploads_max.set",     std::bind(&torrent::Download::set_uploads_max, CMD2_BIND_DL, std::placeholders::_2));
  CMD2_DL         ("d.uploads_min",         std::bind(&torrent::Download::uploads_min, CMD2_BIND_DL));
  CMD2_DL_VALUE_V ("d.uploads_min.set",     std::bind(&torrent::Download::set_uploads_min, CMD2_BIND_DL, std::placeholders::_2));
  CMD2_DL         ("d.downloads_max",         std::bind(&torrent::Download::downloads_max, CMD2_BIND_DL));
  CMD2_DL_VALUE_V ("d.downloads_max.set",     std::bind(&torrent::Download::set_downloads_max, CMD2_BIND_DL, std::placeholders::_2));
  CMD2_DL         ("d.downloads_min",         std::bind(&torrent::Download::downloads_min, CMD2_BIND_DL));
  CMD2_DL_VALUE_V ("d.downloads_min.set",     std::bind(&torrent::Download::set_downloads_min, CMD2_BIND_DL, std::placeholders::_2));
  CMD2_DL         ("d.peers_connected",     std::bind(&torrent::ConnectionList::size, CMD2_BIND_CL));
  CMD2_DL         ("d.peers_not_connected", std::bind(&torrent::PeerList::available_list_size, CMD2_BIND_PL));

  CMD2_DL         ("d.peers_complete",      CMD2_ON_DL(peers_complete));
  CMD2_DL         ("d.peers_accounted",     CMD2_ON_DL(peers_accounted));

  CMD2_DL_V       ("d.disconnect.seeders",        std::bind(&torrent::ConnectionList::erase_seeders, CMD2_BIND_CL));

  CMD2_DL         ("d.accepting_seeders",         CMD2_ON_INFO(is_accepting_seeders));
  CMD2_DL_V       ("d.accepting_seeders.enable",  std::bind(&torrent::DownloadInfo::public_set_flags,   CMD2_BIND_INFO, torrent::DownloadInfo::flag_accepting_seeders));
  CMD2_DL_V       ("d.accepting_seeders.disable", std::bind(&torrent::DownloadInfo::public_unset_flags, CMD2_BIND_INFO, torrent::DownloadInfo::flag_accepting_seeders));

  CMD2_DL         ("d.throttle_name",     std::bind(&download_get_variable, std::placeholders::_1, "rtorrent", "throttle_name"));
  CMD2_DL_STRING_V("d.throttle_name.set", std::bind(&core::Download::set_throttle_name, std::placeholders::_1, std::placeholders::_2));

  CMD2_DL         ("d.bytes_done",     CMD2_ON_DL(bytes_done));
  CMD2_DL         ("d.ratio",          std::bind(&retrieve_d_ratio, std::placeholders::_1));
  CMD2_DL         ("d.chunks_hashed",  CMD2_ON_DL(chunks_hashed));
  CMD2_DL         ("d.free_diskspace", [](auto* download, auto) { return download->file_list()->free_diskspace_no_cache(); });

  CMD2_DL         ("d.size_files",     CMD2_ON_FL(size_files));
  CMD2_DL         ("d.size_bytes",     CMD2_ON_FL(size_bytes));
  CMD2_DL         ("d.size_chunks",    CMD2_ON_FL(size_chunks));
  CMD2_DL         ("d.chunk_size",     CMD2_ON_FL(chunk_size));
  CMD2_DL         ("d.size_pex",       CMD2_ON_DL(size_pex));
  CMD2_DL         ("d.max_size_pex",   CMD2_ON_DL(max_size_pex));

  CMD2_DL         ("d.chunks_seen",      std::bind(&d_chunks_seen, std::placeholders::_1));

  CMD2_DL         ("d.completed_bytes",  CMD2_ON_FL(completed_bytes));
  CMD2_DL         ("d.completed_chunks", CMD2_ON_FL(completed_chunks));
  CMD2_DL         ("d.left_bytes",       CMD2_ON_FL(left_bytes));

  CMD2_DL         ("d.wanted_chunks",    CMD2_ON_DATA(wanted_chunks));

  // Do not exposre d.tracker_announce.force to regular users.
  CMD2_DL_V       ("d.tracker_announce",       std::bind(&torrent::Download::manual_request, CMD2_BIND_DL, false));
  CMD2_DL_V       ("d.tracker_announce.force", std::bind(&torrent::Download::manual_request, CMD2_BIND_DL, true));

  CMD2_DL         ("d.tracker_numwant",      std::bind(&torrent::tracker::TrackerControllerWrapper::numwant, CMD2_BIND_TC));
  CMD2_DL_VALUE_V ("d.tracker_numwant.set",  std::bind(&torrent::tracker::TrackerControllerWrapper::set_numwant, CMD2_BIND_TC, std::placeholders::_2));
  // TODO: Deprecate 'd.tracker_focus'.
  CMD2_DL         ("d.tracker_focus",        std::bind(&core::Download::tracker_list_size, std::placeholders::_1));
  CMD2_DL         ("d.tracker_size",         std::bind(&core::Download::tracker_list_size, std::placeholders::_1));

  CMD2_DL         ("d.tracker.has_active",            std::bind(&torrent::tracker::TrackerControllerWrapper::has_active_trackers, CMD2_BIND_TC));
  CMD2_DL         ("d.tracker.has_active_not_scrape", std::bind(&torrent::tracker::TrackerControllerWrapper::has_active_trackers_not_scrape, CMD2_BIND_TC));
  CMD2_DL         ("d.tracker.has_usable",            std::bind(&torrent::tracker::TrackerControllerWrapper::has_usable_trackers, CMD2_BIND_TC));
  CMD2_DL_LIST    ("d.tracker.insert",                std::bind(&download_tracker_insert, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_VALUE_V ("d.tracker.send_scrape",           [](auto download, uint64_t arg) { download->tracker_controller().scrape_request(arg); });

  CMD2_DL         ("d.directory",                   CMD2_ON_FL(root_dir));
  CMD2_DL         ("d.directory.realpath.or_empty", [](auto* download, auto)     { return resolve_path(download->file_list()->root_dir()); });
  CMD2_DL         ("d.directory.realpath.or_throw", [](auto* download, auto)     { return resolve_path_or_throw(download->file_list()->root_dir()); });
  CMD2_DL_STRING_V("d.directory.set", [](auto* download, auto arg) {
    if (!stop_intent::restoring_path()) stop_intent::require_unleased(download);
    download->set_directory(arg);
  });
  CMD2_DL_STRING_V("d.directory.base.set", [](auto* download, auto arg) {
    if (!stop_intent::restoring_path()) stop_intent::require_unleased(download);
    download->set_base_directory(arg);
  });
  CMD2_DL_LIST("d.directory.set_if_stop_close_claim", [](core::Download* download, const torrent::Object::list_type& args) {
    return set_directory_if_stop_close_claim(download, args, false);
  });
  CMD2_DL_LIST("d.directory.base.set_if_stop_close_claim", [](core::Download* download, const torrent::Object::list_type& args) {
    return set_directory_if_stop_close_claim(download, args, true);
  });

  CMD2_DL         ("d.priority",     std::bind(&core::Download::priority, std::placeholders::_1));
  CMD2_DL         ("d.priority_str", std::bind(&retrieve_d_priority_str, std::placeholders::_1));
  CMD2_DL_VALUE_V ("d.priority.set", std::bind(&core::Download::set_priority, std::placeholders::_1, std::placeholders::_2));

  // CMD2_DL         ("d.group",     std::bind(&torrent::resource_manager_entry::group,
  //                                           std::bind(&torrent::ResourceManager::entry_at, torrent::resource_manager(),
  //                                                     std::bind(&core::Download::main, std::placeholders::_1))));

  // CMD2_DL_V       ("d.group.set", std::bind(&torrent::ResourceManager::set_group,
  //                                           torrent::resource_manager(),
  //                                           std::bind(&torrent::ResourceManager::find_throw, torrent::resource_manager(),
  //                                                     std::bind(&core::Download::main, std::placeholders::_1)),
  //                                           CG_GROUP_INDEX()));

  CMD2_DL         ("d.group",      std::bind(&cg_d_group, std::placeholders::_1));
  CMD2_DL         ("d.group.name", std::bind(&cg_d_group_name, std::placeholders::_1));
  CMD2_DL_V       ("d.group.set",  std::bind(&cg_d_group_set, std::placeholders::_1, std::placeholders::_2));

  CMD2_DL_LIST    ("f.multicall", std::bind(&f_multicall, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_LIST    ("p.multicall", std::bind(&p_multicall, std::placeholders::_1, std::placeholders::_2));
  CMD2_DL_LIST    ("t.multicall", std::bind(&t_multicall, std::placeholders::_1, std::placeholders::_2));

  CMD2_ANY_LIST   ("p.call_target", std::bind(&p_call_target, std::placeholders::_2));

  rpc::rpc.mark_safe("add_peer");
  rpc::rpc.mark_safe("d.hash");
  rpc::rpc.mark_safe("d.incarnation");
  rpc::rpc.mark_safe("d.local_id");
  rpc::rpc.mark_safe("d.local_id_html");
  rpc::rpc.mark_safe("d.bitfield");
  rpc::rpc.mark_safe("d.base_path");
  rpc::rpc.mark_safe("d.base_path.realpath.or_empty");
  rpc::rpc.mark_safe("d.base_path.realpath.or_throw");
  rpc::rpc.mark_safe("d.directory.realpath.or_empty");
  rpc::rpc.mark_safe("d.directory.realpath.or_throw");
  rpc::rpc.mark_safe("d.tied_to_file.realpath.or_empty");
  rpc::rpc.mark_safe("d.tied_to_file.realpath.or_throw");
  rpc::rpc.mark_safe("d.loaded_file.realpath.or_empty");
  rpc::rpc.mark_safe("d.loaded_file.realpath.or_throw");
  rpc::rpc.mark_safe("d.base_path.hex");
  rpc::rpc.mark_safe("d.base_path.base64");
  rpc::rpc.mark_safe("d.base_path.base64_as_binary");
  rpc::rpc.mark_safe("d.base_path.as_binary");
  rpc::rpc.mark_safe("d.base_path.or_base64");
  rpc::rpc.mark_safe("d.base_path.or_as_binary");
  rpc::rpc.mark_safe("d.base_filename");
  rpc::rpc.mark_safe("d.base_filename.hex");
  rpc::rpc.mark_safe("d.base_filename.base64");
  rpc::rpc.mark_safe("d.base_filename.base64_as_binary");
  rpc::rpc.mark_safe("d.base_filename.as_binary");
  rpc::rpc.mark_safe("d.base_filename.or_base64");
  rpc::rpc.mark_safe("d.base_filename.or_as_binary");
  rpc::rpc.mark_safe("d.name");
  rpc::rpc.mark_safe("d.name.hex");
  rpc::rpc.mark_safe("d.name.base64");
  rpc::rpc.mark_safe("d.name.base64_as_binary");
  rpc::rpc.mark_safe("d.name.as_binary");
  rpc::rpc.mark_safe("d.name.or_base64");
  rpc::rpc.mark_safe("d.name.or_as_binary");
  rpc::rpc.mark_safe("d.directory");
  rpc::rpc.mark_safe("d.creation_date");
  rpc::rpc.mark_safe("d.load_date");
  rpc::rpc.mark_safe("d.up.rate");
  rpc::rpc.mark_safe("d.up.total");
  rpc::rpc.mark_safe("d.down.rate");
  rpc::rpc.mark_safe("d.down.total");
  rpc::rpc.mark_safe("d.skip.rate");
  rpc::rpc.mark_safe("d.skip.total");
  rpc::rpc.mark_safe("d.is_open");
  rpc::rpc.mark_safe("d.is_active");
  rpc::rpc.mark_safe("d.is_hash_checked");
  rpc::rpc.mark_safe("d.is_hash_checking");
  rpc::rpc.mark_safe("d.is_multi_file");
  rpc::rpc.mark_safe("d.is_private");
  rpc::rpc.mark_safe("d.is_pex_active");
  rpc::rpc.mark_safe("d.is_partially_done");
  rpc::rpc.mark_safe("d.is_not_partially_done");
  rpc::rpc.mark_safe("d.is_meta");
  rpc::rpc.mark_safe("d.peer_exchange");
  rpc::rpc.mark_safe("d.resume");
  rpc::rpc.mark_safe("d.pause");
  rpc::rpc.mark_safe("d.open");
  rpc::rpc.mark_safe("d.close");
  rpc::rpc.mark_safe("d.close.directly");
  rpc::rpc.mark_safe("d.erase");
  rpc::rpc.mark_safe("d.check_hash");
  rpc::rpc.mark_safe("d.save_resume");
  rpc::rpc.mark_safe("d.save_full_session");
  rpc::rpc.mark_safe("d.update_priorities");
  rpc::rpc.mark_safe("d.custom");
  rpc::rpc.mark_safe("d.custom1");
  rpc::rpc.mark_safe("d.custom2");
  rpc::rpc.mark_safe("d.custom3");
  rpc::rpc.mark_safe("d.custom4");
  rpc::rpc.mark_safe("d.custom5");
  rpc::rpc.mark_safe("d.size_bytes");
  rpc::rpc.mark_safe("d.size_chunks");
  rpc::rpc.mark_safe("d.size_pex");
  rpc::rpc.mark_safe("d.completed_bytes");
  rpc::rpc.mark_safe("d.complete");
  rpc::rpc.mark_safe("d.timestamp.started");
  rpc::rpc.mark_safe("d.timestamp.finished");
  rpc::rpc.mark_safe("d.bytes_done");
  rpc::rpc.mark_safe("d.peers_accounted");
  rpc::rpc.mark_safe("d.chunks_hashed");
  rpc::rpc.mark_safe("d.tracker_size");
  rpc::rpc.mark_safe("d.completed_chunks");
  rpc::rpc.mark_safe("d.left_bytes");
  rpc::rpc.mark_safe("d.chunk_size");
  rpc::rpc.mark_safe("d.priority");
  rpc::rpc.mark_safe("d.priority_str");
  rpc::rpc.mark_safe("d.state");
  rpc::rpc.mark_safe("d.state_changed");
  rpc::rpc.mark_safe("d.state_counter");
  rpc::rpc.mark_safe("d.connection_current");
  rpc::rpc.mark_safe("d.connection_leech");
  rpc::rpc.mark_safe("d.connection_seed");
  rpc::rpc.mark_safe("d.throttle_name");
  rpc::rpc.mark_safe("d.uploads_max");
  rpc::rpc.mark_safe("d.downloads_max");
  rpc::rpc.mark_safe("d.peers_min");
  rpc::rpc.mark_safe("d.peers_max");
  rpc::rpc.mark_safe("d.peers_connected");
  rpc::rpc.mark_safe("d.peers_not_connected");
  rpc::rpc.mark_safe("d.peers_complete");
  rpc::rpc.mark_safe("d.tracker_numwant");
  rpc::rpc.mark_safe("d.tracker_focus");
  rpc::rpc.mark_safe("d.message");
  rpc::rpc.mark_safe("d.hashing");
  rpc::rpc.mark_safe("d.hashing_failed");
  rpc::rpc.mark_safe("d.free_diskspace");
  rpc::rpc.mark_safe("d.views");
  rpc::rpc.mark_safe("d.views.remove");
  rpc::rpc.mark_safe("d.views.push_back_unique");
  rpc::rpc.mark_safe("d.ratio");
  rpc::rpc.mark_safe("f.multicall");
  rpc::rpc.mark_safe("p.multicall");
  rpc::rpc.mark_safe("p.call_target");
  rpc::rpc.mark_safe("t.multicall");
}
