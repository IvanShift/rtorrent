#ifndef RTORRENT_CORE_RETRACKERS_QUEUE_RECEIPTS_H
#define RTORRENT_CORE_RETRACKERS_QUEUE_RECEIPTS_H

#include <cstdint>
#include <string>

// Wire formats shared with the ruTorrent retrackers plugin. The application
// side is plugins/retrackers/update.php: RetrackersQueueRetry (MAX_ATTEMPTS,
// REASONS, parse/next) and the rr.receipts.v1 ledger conditions that build
// 'wp:' . $localId. A change here needs the same change there.
namespace core {

// r1:Q:count:due:reason, compared and set by d.retrackers.defer_original.
inline constexpr unsigned retrackers_retry_max_attempts = 6;
inline constexpr const char* retrackers_retry_reasons[] = {
  "capture-refused", "transport-unavailable", "source-not-ready", "preflight-refused"};

struct RetrackersRetry {
  unsigned    count = 0; // 1 through retrackers_retry_max_attempts.
  uint64_t    due = 0;   // Unix time; 0 exactly for the terminal attempt.
  std::string reason;

  bool terminal() const { return count == retrackers_retry_max_attempts; }
};

// The one parser of a retry receipt bound to queue token Q. It accepts a
// single-digit count 1..6, a canonical decimal due of at most ten digits that
// is 0 if and only if the count is terminal, and one of the four reasons; the
// whole receipt is at most 100 bytes.
template <size_t N>
inline bool queue_retry_parse(const std::string& retry, const std::string& queue_token,
                              RetrackersRetry* result, const char* const (&reasons)[N],
                              bool allow_initial_wait = false) {
  const std::string prefix = "r1:" + queue_token + ":";
  if (retry.size() > 100 || retry.compare(0, prefix.size(), prefix) != 0)
    return false;
  const size_t count_at = prefix.size();
  const size_t due_at = count_at + 2;
  if (retry.size() <= due_at || retry[count_at] < (allow_initial_wait ? '0' : '1') ||
      retry[count_at] > static_cast<char>('0' + retrackers_retry_max_attempts) ||
      retry[count_at + 1] != ':')
    return false;
  const size_t due_end = retry.find(':', due_at);
  if (due_end == std::string::npos || due_end == due_at || due_end - due_at > 10)
    return false;
  const std::string due = retry.substr(due_at, due_end - due_at);
  if (due.find_first_not_of("0123456789") != std::string::npos ||
      (due.size() > 1 && due[0] == '0'))
    return false;
  RetrackersRetry parsed;
  parsed.count = static_cast<unsigned>(retry[count_at] - '0');
  for (char digit : due)
    parsed.due = parsed.due * 10 + static_cast<uint64_t>(digit - '0');
  if (parsed.terminal() != (parsed.due == 0))
    return false;
  parsed.reason = retry.substr(due_end + 1);
  bool known = false;
  for (const char* reason : reasons)
    known = known || parsed.reason == reason;
  if (!known || (parsed.count == 0 && parsed.reason != "source-not-ready" &&
                 parsed.reason != "transport-unavailable"))
    return false;
  if (result != nullptr)
    *result = parsed;
  return true;
}

inline bool retrackers_retry_parse(const std::string& retry, const std::string& queue_token,
                                   RetrackersRetry* result) {
  return queue_retry_parse(retry, queue_token, result, retrackers_retry_reasons);
}

// Temporary unreadiness is a wait, not a failure: a deferral for one of these
// reasons may keep its attempt count. Advertised by system.retrackers.retry_waits.
inline bool retrackers_retry_wait_reason(const std::string& reason) {
  return reason == "source-not-ready" || reason == "transport-unavailable";
}

// The defer_original transition from the current receipt (nullptr when empty)
// to the next one. The first receipt has count 1. A later one either spends
// exactly one attempt, the sixth being terminal with due 0, or, for a wait
// reason, keeps its count with a nonzero due; a wait is never terminal and a
// counted reason always spends an attempt. Nothing follows a terminal receipt.
inline bool retrackers_retry_transition_valid(const RetrackersRetry* prior, const RetrackersRetry& next) {
  const unsigned count = prior != nullptr ? prior->count : 0;
  if (count >= retrackers_retry_max_attempts) return false;
  if (next.count == count + 1) return true;
  return prior != nullptr && next.count == count && next.due != 0 &&
         retrackers_retry_wait_reason(next.reason);
}

// The receipt after one counted attempt, as the application computes it
// (RetrackersQueueRetry::next without a wait): one attempt more, due
// now + 2^count seconds, the sixth terminal with due 0. False after terminal.
inline bool retrackers_retry_counted_next(const RetrackersRetry* prior, uint64_t now,
                                          const std::string& reason, RetrackersRetry* next) {
  const unsigned count = prior != nullptr ? prior->count : 0;
  if (count >= retrackers_retry_max_attempts) return false;
  next->count = count + 1;
  next->due = next->terminal() ? 0 : now + (uint64_t(1) << next->count);
  next->reason = reason;
  return true;
}

inline std::string retrackers_retry_format(const std::string& queue_token, const RetrackersRetry& retry) {
  return "r1:" + queue_token + ":" + std::to_string(retry.count) + ":" + std::to_string(retry.due) +
         ":" + retry.reason;
}

// Application pending receipt of local ID L in the rr.receipts.v1 ledger.
inline constexpr const char* retrackers_pending_receipt_value = "1";

inline std::string retrackers_pending_receipt_key(const std::string& local_id) {
  return "wp:" + local_id;
}

} // namespace core

#endif
