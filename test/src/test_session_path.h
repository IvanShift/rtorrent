#include "test/helpers/test_fixture.h"

#include <string>

class TestSessionPath : public test_fixture {
  CPPUNIT_TEST_SUITE(TestSessionPath);
  CPPUNIT_TEST(test_pass_walks_once);
  CPPUNIT_TEST(test_failed_walk_is_not_cached);
  CPPUNIT_TEST(test_copies_have_own_offset);
  CPPUNIT_TEST(test_other_path_refused_each_time);
  CPPUNIT_TEST(test_other_thread_never_uses_pass);
  CPPUNIT_TEST(test_descriptors_closed);
  CPPUNIT_TEST(test_readers_see_new_names);
  CPPUNIT_TEST(test_created_directory_seen);
  CPPUNIT_TEST(test_check_follows_replaced_session);
  CPPUNIT_TEST(test_check_follows_replaced_sidecar);
  CPPUNIT_TEST(test_unsafe_sidecar_refused_each_time);
  CPPUNIT_TEST(test_check_attests_again);
  CPPUNIT_TEST(test_missing_needs_enoent);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp();
  void tearDown();

  void test_pass_walks_once();
  void test_failed_walk_is_not_cached();
  void test_copies_have_own_offset();
  void test_other_path_refused_each_time();
  void test_other_thread_never_uses_pass();
  void test_descriptors_closed();
  void test_readers_see_new_names();
  void test_created_directory_seen();
  void test_check_follows_replaced_session();
  void test_check_follows_replaced_sidecar();
  void test_unsafe_sidecar_refused_each_time();
  void test_check_attests_again();
  void test_missing_needs_enoent();

private:
  std::string m_root;
  std::string m_outer;
  std::string m_session;
};
