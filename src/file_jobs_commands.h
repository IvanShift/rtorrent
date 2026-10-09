#pragma once

// This implementation is included after the existing retrackers owners. It
// deliberately reuses their user digest, process watcher and claim decisions.
namespace {
using MoveRecord = core::file_jobs::Record;
std::map<std::string,std::string> move_launch_retries;
std::string move_session() { return rpc::call_command_string("session.path"); }
std::string move_hash(core::Download* d) { return rpc::call_command_string("d.hash",rpc::make_target(d)); }
void move_user(const std::string& user) {
  if(user.size()>128 || user.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-")!=std::string::npos)
    throw torrent::input_error("autotools: move user invalid");
}
MoveRecord move_require(core::Download* d,const std::string& queue) {
  MoveRecord r;
  if(!core::file_jobs::read(move_session(),move_hash(d),&r) || r.queue!=queue)
    throw torrent::input_error("autotools: move queue changed");
  return r;
}
std::string move_state(const std::string& hash) {
  try {
    MoveRecord r;
    if(!core::file_jobs::read(move_session(),hash,&r)) return "none";
    const auto lease=stop_intent::read_lease(move_session(),hash);
    core::RetrackersRetry retry;
    if(!r.retry.empty() && core::file_jobs::retry_parse(r.retry,r.queue,&retry) && retry.terminal())
      return (!lease.token.empty() ? "held:" : "terminal:"+r.user+":"+r.queue+":")+retry.reason;
    if(!lease.token.empty()) {
      if(lease.job!=r.queue) return "held:superseded-by-user";
      return "leased:"+r.user+":"+r.queue+":"+lease.token;
    }
    return "queued:"+r.user+":"+r.queue+":"+r.local+":"+r.retry;
  } catch(const std::exception&) { return "held:record-malformed"; }
}
void move_enqueue_clear_refusal(core::Download* d) {
  auto& custom=d->bencode()->get_key("rtorrent").insert_preserve_copy("custom",torrent::Object::create_map()).first->second;
  custom.erase_key("autotools-enqueue-refusal"); custom.erase_key("autotools-native-outcome");
  if(d->message().rfind("autotools: move enqueue refused:",0)==0 ||
     d->message().rfind("autotools: move held: record-unreadable;",0)==0) d->set_message({});
}
std::string move_enqueue(core::Download* d,const std::string& user) {
  move_user(user);
  const auto target=rpc::make_target(d);
  const auto hash=move_hash(d), session=move_session(), digest=rr_user_digest(user);
  if(session.empty() || rpc::call_command_value("d.is_meta",target)!=0 || rpc::call_command_value("d.complete",target)!=1)
    throw torrent::input_error("autotools: move requires complete non-metadata download and session");
  MoveRecord r;
  bool exists;
  try { exists=core::file_jobs::read(session,hash,&r); }
  catch(const std::exception&) {
    core::file_jobs::held[hash]="record-unreadable";
    d->set_message("autotools: move held: record-unreadable; inspect session .move record");
    throw torrent::input_error("autotools: move record-unreadable");
  }
  if(exists && r.user!=digest) {
    d->set_message("autotools: move refused: owner-changed; existing job retained");
    throw torrent::input_error("autotools: move owner-changed");
  }
  try {
    if(exists) {
      // Visibility alone does not attest a previous rename whose directory
      // sync failed. Retry the durability boundary before acknowledging Q.
      core::file_jobs::confirm_publication(session);
      move_enqueue_clear_refusal(d);
      return "exists:"+r.queue;
    }
    r={digest,rr_original_mint_tx(),
       rpc::call_command_string("d.local_id",target),{},static_cast<uint64_t>(::time(nullptr))};
    core::file_jobs::write(session,hash,r,true);
    move_enqueue_clear_refusal(d);
    return "queued:"+r.queue;
  } catch(const std::exception&) {
    d->bencode()->get_key("rtorrent").insert_preserve_copy("custom",torrent::Object::create_map()).first->second.insert_key("autotools-enqueue-refusal","session-storage-failed");
    d->set_message("autotools: move enqueue refused: session-storage-failed; inspect session directory and retry enqueue");
    throw torrent::input_error("autotools: move enqueue refused; inspect session .move record");
  }
}
std::string move_begin(core::Download* d,const torrent::Object::list_type& args) {
  if(args.size()!=2 || !args[0].is_string() || !args[1].is_string()) throw torrent::input_error("autotools: move begin needs Q and incarnation");
  const auto r=move_require(d,args[0].as_string());
  const auto target=rpc::make_target(d);
  if(rpc::call_command_string("d.incarnation",target)!=args[1].as_string()) return "busy:incarnation-changed";
  core::RetrackersRetry retry;
  if(!r.retry.empty() && core::file_jobs::retry_parse(r.retry,r.queue,&retry) && retry.terminal()) return "busy:terminal";
  const auto lease=stop_intent::lease(d);
  if(!lease.token.empty() && lease.job!=r.queue) return "busy:stop-lease";
  if(lease.token.empty() && (rr_queued_original(move_hash(d))!="none" || stop_intent::checker_owned(d))) return "busy:source-not-ready";
  const std::string receipt=rpc::call_command("d.stop_close_claim",r.queue,target).as_string();
  if(receipt.rfind("done|",0)==0 || receipt.rfind("terminal|",0)==0) return receipt;
  return "active|"+receipt;
}
void move_log(const std::string& h,const std::string& outcome,const std::string& reason) {
  lt_log_print(torrent::LOG_NOTICE,"autotools: move %s %s: %s",h.c_str(),outcome.c_str(),reason.c_str());
}
void move_supersede(core::Download* d,const MoveRecord& r) {
  const auto h=move_hash(d);
  core::file_jobs::remove(move_session(),h,r.queue);
  d->bencode()->get_key("rtorrent").insert_preserve_copy("custom",torrent::Object::create_map()).first->second.insert_key("autotools-native-outcome","superseded-by-user");
  move_log(h,"cancelled","superseded-by-user");
}
void move_retry(const std::string& h, MoveRecord& r,const std::string& next) {
  r.retry=next; core::file_jobs::write(move_session(),h,r);
  core::RetrackersRetry parsed; core::file_jobs::retry_parse(next,r.queue,&parsed);
  move_log(h,parsed.terminal()?"retries-exhausted":"waiting",parsed.reason);
}
std::string move_defer(core::Download* d,const torrent::Object::list_type& args) {
  if(args.size()!=3 || !std::all_of(args.begin(),args.end(),[](const auto& a){return a.is_string();}))
    throw torrent::input_error("autotools: move defer needs Q, expected and next retry");
  auto r=move_require(d,args[0].as_string());
  if(!stop_intent::lease(d).token.empty()) throw torrent::input_error("autotools: move defer blocked by lease");
  core::RetrackersRetry prior,next;
  if(r.retry!=args[1].as_string() || (!r.retry.empty() && !core::file_jobs::retry_parse(r.retry,r.queue,&prior)) ||
      !core::file_jobs::retry_parse(args[2].as_string(),r.queue,&next)) throw torrent::input_error("autotools: move retry CAS changed");
  const unsigned count=r.retry.empty()?0:prior.count;
  const bool wait=core::retrackers_retry_wait_reason(next.reason);
  if(count>=6 || (wait ? next.count!=count : next.count!=count+1))
    throw torrent::input_error("autotools: move retry must count once or wait");
  const uint64_t now=::time(nullptr);
  const uint64_t delay=wait?30:60*(uint64_t(1)<<(next.count-1));
  // Clock skew tolerance allows the application to compute its receipt before
  // the RPC; it cannot force immediate repeated counted attempts.
  if(!next.terminal() && (next.due+5<now+delay || next.due>now+delay+120))
    throw torrent::input_error("autotools: move retry due invalid");
  move_retry(move_hash(d),r,args[2].as_string());
  move_launch_retries.erase("move:"+move_hash(d)+":"+r.queue);
  return "deferred";
}
std::string move_retire(core::Download* d,const std::string& q,const std::string& reason) {
  MoveRecord r; const auto h=move_hash(d);
  if(!core::file_jobs::read(move_session(),h,&r)) return "none";
  if(r.queue!=q) throw torrent::input_error("autotools: move queue changed");
  if(!stop_intent::lease(d).token.empty()) throw torrent::input_error("autotools: move retirement blocked by lease");
  core::file_jobs::remove(move_session(),h,q); move_launch_retries.erase("move:"+h+":"+q);
  move_log(h,reason=="finished"?"finished":"cancelled",reason);
  if(reason=="finished" && core::migration_test_cut("after-move-finish")) ::_exit(137);
  return reason=="finished"?"finished":"cancelled";
}
void move_recover_exit(const core::retrackers_process_pool::Exit& exit) {
  const auto launched=move_launch_retries.find(exit.identity);
  if(launched==move_launch_retries.end()) return;
  const auto expected=launched->second; move_launch_retries.erase(launched);
  const auto h=exit.identity.substr(5,40), q=exit.identity.substr(46);
  try {
    MoveRecord r;
    if(!core::file_jobs::read(move_session(),h,&r) || r.queue!=q || r.retry!=expected) return;
    core::RetrackersRetry retry;
    if(!r.retry.empty()) core::file_jobs::retry_parse(r.retry,q,&retry);
    if(retry.terminal()) return;
    const bool success=exit.status>=0 && WIFEXITED(exit.status) && WEXITSTATUS(exit.status)==0;
    retry.reason=success?"source-not-ready":"worker-exit";
    if(!success) ++retry.count;
    retry.due=retry.terminal()?0:static_cast<uint64_t>(::time(nullptr))+(success?30:60*(uint64_t(1)<<(retry.count-1)));
    move_retry(h,r,core::retrackers_retry_format(q,retry));
  } catch(const std::exception&) { move_log(h,"held","storage-failed"); }
}
std::string move_dispatch(const std::string& user) {
  move_user(user);
  const auto session=move_session(); if(session.empty()) return "held";
  const auto digest=rr_user_digest(user);
  // Copy keys because durable retirement updates the index.
  std::vector<std::string> hashes;
  for(const auto& [h,r]:core::file_jobs::records) if(r.user==digest) hashes.push_back(h);
  for(const auto& h:hashes) {
    try {
      MoveRecord r; if(!core::file_jobs::read(session,h,&r)) continue;
      auto* d=control->core()->download_list()->find_hex_ptr(h.c_str()); if(!d) continue;
      const auto claim=stop_intent::lease(d);
      if(!claim.token.empty() && claim.job!=r.queue) {
        move_supersede(d,r); continue;
      }
      core::RetrackersRetry retry;
      if(!r.retry.empty() && core::file_jobs::retry_parse(r.retry,r.queue,&retry) && retry.terminal()) continue;
      if(claim.token.empty() && static_cast<uint64_t>(::time(nullptr))>=r.enqueued+86400) {
        core::file_jobs::remove(session,h,r.queue);
        d->bencode()->get_key("rtorrent").insert_preserve_copy("custom",torrent::Object::create_map()).first->second.insert_key("autotools-native-outcome","expired");
        move_log(h,"cancelled","expired"); continue;
      }
      if(!r.retry.empty() && retry.due>static_cast<uint64_t>(::time(nullptr))) continue;
      if(claim.token.empty() && (rr_queued_original(h)!="none" || stop_intent::checker_owned(d))) continue;
      const auto identity="move:"+h+":"+r.queue;
      if(core::retrackers_process_pool::active(identity)) continue;
      const auto result=core::retrackers_process_pool::launch(identity,
          {core::retrackers_process_pool::php_executable,core::retrackers_process_pool::application_root+"/plugins/autotools/move_worker.php",h,r.queue,user,
           rpc::call_command_string("d.incarnation",rpc::make_target(d))},false);
      if(result=="spawned") { move_launch_retries[identity]=r.retry; rr_watch_workers_schedule(); }
      return result;
    } catch(const std::exception&) { move_log(h,"held","storage-failed"); return "held"; }
  }
  return "idle";
}
void initialize_file_job_commands() {
  CMD2_ANY("system.autotools.move_queue",[](auto,auto){return int64_t(1);});
  CMD2_ANY_STRING("system.autotools.move_state",[](auto,const auto& h){return move_state(h);});
  CMD2_ANY_STRING("system.autotools.move_owner",[](auto,const auto& h){
    MoveRecord r;
    return core::file_jobs::read(move_session(),h,&r)?r.user:std::string();
  });
  CMD2_ANY_STRING("system.autotools.move_dispatch",[](auto,const auto& u){return move_dispatch(u);});
  CMD2_DL_STRING("d.autotools.move_enqueue",[](auto* d,const auto& u){return move_enqueue(d,u);});
  CMD2_DL_LIST("d.autotools.move_begin",move_begin);
  CMD2_DL_LIST("d.autotools.move_defer",move_defer);
  CMD2_DL_STRING("d.autotools.move_finish",[](auto* d,const auto& q){return move_retire(d,q,"finished");});
  CMD2_DL_LIST("d.autotools.move_cancel",[](auto* d,const torrent::Object::list_type& a){
    if(a.size()!=2 || !a[0].is_string() || !a[1].is_string()) throw torrent::input_error("autotools: move cancel needs Q and reason");
    const auto reason=a[1].as_string();
    const std::set<std::string> allowed({"configuration-declined","source-outside-downloads","skip-filter-match","destination-outside-finished-root","manifest-path-invalid","source-name-mismatch","manifest-empty"});
    if(!allowed.count(reason)) throw torrent::input_error("autotools: move cancel reason invalid");
    return move_retire(d,a[0].as_string(),reason);
  });
  CMD2_ANY_STRING("system.autotools.move_reset",[](auto,const auto& h){
    MoveRecord r; core::RetrackersRetry retry;
    if(!core::file_jobs::read(move_session(),h,&r) || !core::file_jobs::retry_parse(r.retry,r.queue,&retry) || !retry.terminal())
      throw torrent::input_error("autotools: move reset requires terminal record");
    r.retry.clear(); core::file_jobs::write(move_session(),h,r); return std::string("reset");
  });
  CMD2_DL("d.stop_close_claim_held",[](auto* d,auto){return stop_intent::lease(d).token;});
}
}
