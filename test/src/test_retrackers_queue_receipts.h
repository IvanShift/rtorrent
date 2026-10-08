#include "test/helpers/test_fixture.h"

class TestRetrackersQueueReceipts : public test_fixture {
  CPPUNIT_TEST_SUITE(TestRetrackersQueueReceipts);

  CPPUNIT_TEST(test_retry_accepts_counts_one_through_six);
  CPPUNIT_TEST(test_retry_rejects_counts_outside_the_limit);
  CPPUNIT_TEST(test_retry_due_is_zero_only_when_terminal);
  CPPUNIT_TEST(test_retry_due_is_canonical_decimal);
  CPPUNIT_TEST(test_retry_reasons_match_the_application_list);
  CPPUNIT_TEST(test_retry_is_bound_to_queue_and_size);
  CPPUNIT_TEST(test_pending_receipt_key_and_value);
  CPPUNIT_TEST(test_retry_spends_exactly_one_attempt);
  CPPUNIT_TEST(test_retry_wait_keeps_count_only_for_wait_reasons);
  CPPUNIT_TEST(test_retry_terminal_and_bounds_stay_closed);
  CPPUNIT_TEST(test_retry_counted_next_matches_the_application);

  CPPUNIT_TEST_SUITE_END();

public:
  void test_retry_accepts_counts_one_through_six();
  void test_retry_rejects_counts_outside_the_limit();
  void test_retry_due_is_zero_only_when_terminal();
  void test_retry_due_is_canonical_decimal();
  void test_retry_reasons_match_the_application_list();
  void test_retry_is_bound_to_queue_and_size();
  void test_pending_receipt_key_and_value();
  void test_retry_spends_exactly_one_attempt();
  void test_retry_wait_keeps_count_only_for_wait_reasons();
  void test_retry_terminal_and_bounds_stay_closed();
  void test_retry_counted_next_matches_the_application();
};
