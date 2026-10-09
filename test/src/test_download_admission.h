#include <cppunit/extensions/HelperMacros.h>

class TestDownloadAdmission : public CppUnit::TestFixture {
  CPPUNIT_TEST_SUITE(TestDownloadAdmission);
  CPPUNIT_TEST(test_zero_metadata_identity);
  CPPUNIT_TEST(test_zero_magnet_encodings);
  CPPUNIT_TEST(test_selected_topic_and_nonzero_identity);
  CPPUNIT_TEST_SUITE_END();
public:
  void test_zero_metadata_identity();
  void test_zero_magnet_encodings();
  void test_selected_topic_and_nonzero_identity();
};
