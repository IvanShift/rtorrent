#include "config.h"

#include "test/src/test_retrackers_queue_receipts.h"

#include <iterator>
#include <string>

#include "core/retrackers_queue_receipts.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestRetrackersQueueReceipts);

// These cases pin the native side of the r1 retry contract. The application
// side, plugins/retrackers/update.php RetrackersQueueRetry in ruTorrent, must
// accept exactly the same receipts.
namespace {
const std::string queue(32, 'a');

std::string receipt(const std::string& count, const std::string& due,
                    const std::string& reason = "capture-refused") {
  return "r1:" + queue + ":" + count + ":" + due + ":" + reason;
}

bool parses(const std::string& retry, core::RetrackersRetry* result = nullptr) {
  return core::retrackers_retry_parse(retry, queue, result);
}
}

void TestRetrackersQueueReceipts::test_retry_accepts_counts_one_through_six() {
  CPPUNIT_ASSERT_EQUAL(6u, core::retrackers_retry_max_attempts);
  for (unsigned count = 1; count != 6; ++count) {
    core::RetrackersRetry parsed;
    CPPUNIT_ASSERT_MESSAGE("count " + std::to_string(count) + " must be accepted",
                           parses(receipt(std::to_string(count), "1791400000"), &parsed));
    CPPUNIT_ASSERT_EQUAL(count, parsed.count);
    CPPUNIT_ASSERT_EQUAL(uint64_t(1791400000), parsed.due);
    CPPUNIT_ASSERT(!parsed.terminal());
  }
  core::RetrackersRetry terminal;
  CPPUNIT_ASSERT(parses(receipt("6", "0", "source-not-ready"), &terminal));
  CPPUNIT_ASSERT_EQUAL(6u, terminal.count);
  CPPUNIT_ASSERT_EQUAL(uint64_t(0), terminal.due);
  CPPUNIT_ASSERT_EQUAL(std::string("source-not-ready"), terminal.reason);
  CPPUNIT_ASSERT(terminal.terminal());
}

void TestRetrackersQueueReceipts::test_retry_rejects_counts_outside_the_limit() {
  for (const char* count : {"0", "7", "9", "10", "60", "", "a", "-1", "+1"})
    CPPUNIT_ASSERT_MESSAGE(std::string("count '") + count + "' must be refused",
                           !parses(receipt(count, "5")) && !parses(receipt(count, "0")));
}

void TestRetrackersQueueReceipts::test_retry_due_is_zero_only_when_terminal() {
  for (const char* count : {"1", "2", "3", "4", "5"})
    CPPUNIT_ASSERT_MESSAGE(std::string("count ") + count + " needs a positive due",
                           !parses(receipt(count, "0")));
  CPPUNIT_ASSERT(!parses(receipt("6", "1")));
  CPPUNIT_ASSERT(!parses(receipt("6", "1791400000")));
}

void TestRetrackersQueueReceipts::test_retry_due_is_canonical_decimal() {
  CPPUNIT_ASSERT(parses(receipt("1", "9999999999")));
  for (const char* due : {"", "00", "01", "10000000000", "12a", " 1", "1 ", "-5", "0x10"})
    CPPUNIT_ASSERT_MESSAGE(std::string("due '") + due + "' must be refused", !parses(receipt("1", due)));
}

void TestRetrackersQueueReceipts::test_retry_reasons_match_the_application_list() {
  const char* expected[] = {"capture-refused", "transport-unavailable", "source-not-ready", "preflight-refused"};
  CPPUNIT_ASSERT_EQUAL(std::size(expected), std::size(core::retrackers_retry_reasons));
  for (size_t index = 0; index != std::size(expected); ++index) {
    CPPUNIT_ASSERT_EQUAL(std::string(expected[index]), std::string(core::retrackers_retry_reasons[index]));
    core::RetrackersRetry parsed;
    CPPUNIT_ASSERT(parses(receipt("2", "77", expected[index]), &parsed));
    CPPUNIT_ASSERT_EQUAL(std::string(expected[index]), parsed.reason);
  }
  for (const char* reason : {"", "untrusted-text", "capture-refused ", "Capture-refused",
                             "capture-refused:extra", "native-capture-retry-pending"})
    CPPUNIT_ASSERT_MESSAGE(std::string("reason '") + reason + "' must be refused",
                           !parses(receipt("2", "77", reason)));
}

void TestRetrackersQueueReceipts::test_retry_is_bound_to_queue_and_size() {
  CPPUNIT_ASSERT(!core::retrackers_retry_parse(receipt("1", "5"), std::string(32, 'b'), nullptr));
  CPPUNIT_ASSERT(!parses("r2:" + queue + ":1:5:capture-refused"));
  CPPUNIT_ASSERT(!parses("r1:" + queue + ":1:5"));
  CPPUNIT_ASSERT(!parses("r1:" + queue + ":1"));
  CPPUNIT_ASSERT(!parses(""));
  // The longest valid receipt is 3 + 32 + 1 + 2 + 10 + 1 + 21 bytes; the
  // native 100-byte limit stays above it.
  const std::string longest = receipt("5", "9999999999", "transport-unavailable");
  CPPUNIT_ASSERT_EQUAL(size_t(70), longest.size());
  CPPUNIT_ASSERT(parses(longest));
}

void TestRetrackersQueueReceipts::test_pending_receipt_key_and_value() {
  const std::string local_id(40, 'C');
  CPPUNIT_ASSERT_EQUAL("wp:" + local_id, core::retrackers_pending_receipt_key(local_id));
  CPPUNIT_ASSERT_EQUAL(std::string("1"), std::string(core::retrackers_pending_receipt_value));
}

namespace {
core::RetrackersRetry parsed_receipt(unsigned count, uint64_t due, const char* reason) {
  core::RetrackersRetry result;
  const std::string wire = receipt(std::to_string(count), std::to_string(due), reason);
  CPPUNIT_ASSERT_MESSAGE("fixture must parse: " + wire, parses(wire, &result));
  return result;
}
}

// The application behaviour before waits: every deferral spends one attempt.
void TestRetrackersQueueReceipts::test_retry_spends_exactly_one_attempt() {
  for (const char* reason : core::retrackers_retry_reasons) {
    CPPUNIT_ASSERT(core::retrackers_retry_transition_valid(nullptr, parsed_receipt(1, 77, reason)));
    for (unsigned count = 1; count != 6; ++count) {
      const auto prior = parsed_receipt(count, 77, reason);
      const auto next = count + 1 == 6 ? parsed_receipt(6, 0, reason) : parsed_receipt(count + 1, 99, reason);
      CPPUNIT_ASSERT_MESSAGE(std::string("+1 from ") + std::to_string(count) + " " + reason,
                             core::retrackers_retry_transition_valid(&prior, next));
      if (count + 2 <= 5)
        CPPUNIT_ASSERT(!core::retrackers_retry_transition_valid(&prior, parsed_receipt(count + 2, 99, reason)));
    }
  }
  CPPUNIT_ASSERT(!core::retrackers_retry_transition_valid(nullptr, parsed_receipt(2, 77, "capture-refused")));
}

void TestRetrackersQueueReceipts::test_retry_wait_keeps_count_only_for_wait_reasons() {
  for (unsigned count = 1; count != 6; ++count) {
    const auto prior = parsed_receipt(count, 77, "capture-refused");
    for (const char* reason : {"source-not-ready", "transport-unavailable"})
      CPPUNIT_ASSERT_MESSAGE(std::string("wait at ") + std::to_string(count) + " " + reason,
                             core::retrackers_retry_transition_valid(&prior, parsed_receipt(count, 123, reason)));
    for (const char* reason : {"capture-refused", "preflight-refused"})
      CPPUNIT_ASSERT_MESSAGE(std::string("counted reason cannot wait: ") + reason,
                             !core::retrackers_retry_transition_valid(&prior, parsed_receipt(count, 123, reason)));
    // A wait with due 0 is refused even if a caller bypassed the parser.
    core::RetrackersRetry zero = parsed_receipt(count, 123, "source-not-ready");
    zero.due = 0;
    CPPUNIT_ASSERT(!core::retrackers_retry_transition_valid(&prior, zero));
    if (count > 1)
      CPPUNIT_ASSERT(!core::retrackers_retry_transition_valid(&prior, parsed_receipt(count - 1, 123, "source-not-ready")));
  }
  CPPUNIT_ASSERT(core::retrackers_retry_wait_reason("source-not-ready"));
  CPPUNIT_ASSERT(core::retrackers_retry_wait_reason("transport-unavailable"));
  CPPUNIT_ASSERT(!core::retrackers_retry_wait_reason("capture-refused"));
  CPPUNIT_ASSERT(!core::retrackers_retry_wait_reason("preflight-refused"));
}

void TestRetrackersQueueReceipts::test_retry_terminal_and_bounds_stay_closed() {
  const auto terminal = parsed_receipt(6, 0, "source-not-ready");
  for (const char* reason : core::retrackers_retry_reasons) {
    CPPUNIT_ASSERT(!core::retrackers_retry_transition_valid(&terminal, parsed_receipt(6, 0, reason)));
    CPPUNIT_ASSERT(!core::retrackers_retry_transition_valid(&terminal, parsed_receipt(5, 9, reason)));
  }
  // Only a +1 step reaches the terminal attempt, for any reason, as before
  // waits; a wait keeps its count and never does.
  const auto fifth = parsed_receipt(5, 77, "source-not-ready");
  CPPUNIT_ASSERT(core::retrackers_retry_transition_valid(&fifth, parsed_receipt(6, 0, "source-not-ready")));
  CPPUNIT_ASSERT(core::retrackers_retry_transition_valid(&fifth, parsed_receipt(5, 88, "source-not-ready")));
  CPPUNIT_ASSERT(!parses(receipt("5", "0", "source-not-ready")));
}

// RetrackersQueueRetry::next for a counted reason: count + 1, due now + 2^count,
// the sixth terminal with due 0, nothing after it. The formatted receipt parses
// back and is a valid defer transition.
void TestRetrackersQueueReceipts::test_retry_counted_next_matches_the_application() {
  const uint64_t now = 1791400000;
  core::RetrackersRetry next;
  CPPUNIT_ASSERT(core::retrackers_retry_counted_next(nullptr, now, "capture-refused", &next));
  CPPUNIT_ASSERT_EQUAL(receipt("1", std::to_string(now + 2)), core::retrackers_retry_format(queue, next));
  CPPUNIT_ASSERT(core::retrackers_retry_transition_valid(nullptr, next));
  for (unsigned count = 1; count != 6; ++count) {
    core::RetrackersRetry prior;
    CPPUNIT_ASSERT(parses(receipt(std::to_string(count), "1791399999", "source-not-ready"), &prior));
    CPPUNIT_ASSERT(core::retrackers_retry_counted_next(&prior, now, "capture-refused", &next));
    const std::string wire = core::retrackers_retry_format(queue, next);
    const std::string due = count + 1 == 6 ? "0" : std::to_string(now + (uint64_t(1) << (count + 1)));
    CPPUNIT_ASSERT_EQUAL(receipt(std::to_string(count + 1), due), wire);
    core::RetrackersRetry reparsed;
    CPPUNIT_ASSERT(parses(wire, &reparsed));
    CPPUNIT_ASSERT(core::retrackers_retry_transition_valid(&prior, reparsed));
  }
  core::RetrackersRetry terminal;
  CPPUNIT_ASSERT(parses(receipt("6", "0"), &terminal));
  CPPUNIT_ASSERT(!core::retrackers_retry_counted_next(&terminal, now, "capture-refused", &next));
}
