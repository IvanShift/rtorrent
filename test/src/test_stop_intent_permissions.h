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

private:
  std::string m_session;
};
