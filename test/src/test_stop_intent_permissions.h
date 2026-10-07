#include "test/helpers/test_fixture.h"

#include <string>

class TestStopIntentPermissions : public test_fixture {
  CPPUNIT_TEST_SUITE(TestStopIntentPermissions);
  CPPUNIT_TEST(test_private_journal_accepts_owned_session_mode_0755);
  CPPUNIT_TEST(test_writable_session_refuses_first_claim);
  CPPUNIT_TEST(test_writable_intent_directory_refuses_absence_and_claim);
  CPPUNIT_TEST(test_writable_journal_cannot_supply_generation);
  CPPUNIT_TEST(test_readable_claim_cannot_reserve_download);
  CPPUNIT_TEST(test_trailing_slash_cannot_follow_session_symlink);
  CPPUNIT_TEST(test_writable_ancestor_cannot_replace_private_session);
  CPPUNIT_TEST(test_relative_session_under_sticky_tmp_still_works);
  CPPUNIT_TEST(test_dotdot_cannot_hide_writable_ancestor);
  CPPUNIT_TEST(test_setgid_session_accepts_private_journal);
  CPPUNIT_TEST(test_legacy_counter_compacts_after_increment);
  CPPUNIT_TEST(test_counter_stays_bounded);
  CPPUNIT_TEST(test_retired_counter_is_not_inherited);
  CPPUNIT_TEST(test_multiple_link_journal_is_refused);
  CPPUNIT_TEST(test_corrupt_journal_names_document_and_hash);
  CPPUNIT_TEST(test_malformed_claim_names_document_and_hash);
  CPPUNIT_TEST(test_claim_publication_never_creates_hardlink);
  CPPUNIT_TEST(test_claim_publication_collision_preserves_both_files);
  CPPUNIT_TEST(test_claim_publication_falls_back_for_unsupported_rename_flags);
  CPPUNIT_TEST(test_claim_fallback_refuses_existing_and_dangling_targets);
  CPPUNIT_TEST(test_claim_publication_does_not_fall_back_for_permission_failure);
  CPPUNIT_TEST(test_interrupted_claim_pair_recovers_exact_staging_link);
  CPPUNIT_TEST(test_unexplained_claim_link_is_refused);
  CPPUNIT_TEST(test_interrupted_claim_wrong_nonce_is_refused);
  CPPUNIT_TEST(test_fifo_counter_is_refused_without_blocking);
  CPPUNIT_TEST(test_fifo_claim_is_refused_without_blocking);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp();
  void tearDown();

  void test_private_journal_accepts_owned_session_mode_0755();
  void test_writable_session_refuses_first_claim();
  void test_writable_intent_directory_refuses_absence_and_claim();
  void test_writable_journal_cannot_supply_generation();
  void test_readable_claim_cannot_reserve_download();
  void test_trailing_slash_cannot_follow_session_symlink();
  void test_writable_ancestor_cannot_replace_private_session();
  void test_relative_session_under_sticky_tmp_still_works();
  void test_dotdot_cannot_hide_writable_ancestor();

  void test_setgid_session_accepts_private_journal();

  void test_legacy_counter_compacts_after_increment();

  void test_counter_stays_bounded();

  void test_retired_counter_is_not_inherited();

  void test_multiple_link_journal_is_refused();

  void test_corrupt_journal_names_document_and_hash();

  void test_malformed_claim_names_document_and_hash();

  void test_claim_publication_never_creates_hardlink();
  void test_claim_publication_collision_preserves_both_files();

  void test_claim_publication_falls_back_for_unsupported_rename_flags();

  void test_claim_fallback_refuses_existing_and_dangling_targets();

  void test_claim_publication_does_not_fall_back_for_permission_failure();

  void test_interrupted_claim_pair_recovers_exact_staging_link();
  void test_unexplained_claim_link_is_refused();
  void test_interrupted_claim_wrong_nonce_is_refused();

  void test_fifo_counter_is_refused_without_blocking();
  void test_fifo_claim_is_refused_without_blocking();

private:
  std::string m_session;
};
