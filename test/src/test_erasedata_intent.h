#pragma once

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <string>

class TestErasedataIntent : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(TestErasedataIntent);
  CPPUNIT_TEST(test_canonical_record_round_trip);
  CPPUNIT_TEST(test_malformed_records_are_refused);
  CPPUNIT_TEST(test_identity_does_not_depend_on_phase);
  CPPUNIT_TEST(test_exact_record_publication_and_retirement);
  CPPUNIT_TEST(test_stable_lock_survives_record_retirement_and_fork);
  CPPUNIT_TEST(test_missing_lock_and_unsafe_lock_are_refused);
  CPPUNIT_TEST(test_session_absence_requires_every_document_and_staging_name);
  CPPUNIT_TEST_SUITE_END();

public:
  void setUp() override;
  void tearDown() override;
  void test_canonical_record_round_trip();
  void test_malformed_records_are_refused();
  void test_identity_does_not_depend_on_phase();
  void test_exact_record_publication_and_retirement();
  void test_stable_lock_survives_record_retirement_and_fork();
  void test_missing_lock_and_unsafe_lock_are_refused();
  void test_session_absence_requires_every_document_and_staging_name();

private:
  std::string m_session;
};
