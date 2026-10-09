#include "config.h"
#include "test/src/test_download_admission.h"
#include "core/download_admission.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestDownloadAdmission);

namespace {
torrent::Object magnet(const std::string& value) {
  torrent::Object object=torrent::Object::create_map();
  object.insert_key("magnet-uri",value);
  return object;
}
std::string repeated(const std::string& value,size_t count) {
  std::string result;
  while(count--) result+=value;
  return result;
}
}
void TestDownloadAdmission::test_zero_metadata_identity() {
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(torrent::Object(int64_t(7))));
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(torrent::Object::create_list()));
  torrent::Object object=torrent::Object::create_map();
  auto& info=object.insert_key("info",torrent::Object::create_map());
  info.insert_key("meta_download",int64_t(1));
  info.insert_key("pieces",std::string(20,'\0'));
  CPPUNIT_ASSERT(core::download_input_zero_infohash(object));
  info.insert_key("pieces",std::string(20,'x'));
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(object));
  info.insert_key("pieces",std::string(20,'\0'));
  info.insert_key("meta_download",int64_t(0));
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(object));
}
void TestDownloadAdmission::test_zero_magnet_encodings() {
  for(const auto& hash:{std::string(32,'A'),std::string(32,'a'),std::string(40,'0'),
                       repeated("%00",20),repeated("%30",40)})
    CPPUNIT_ASSERT(core::download_input_zero_infohash(magnet("magnet:?xt=urn:btih:"+hash+"&xt=urn:btmh:1220"+std::string(64,'b'))));
}
void TestDownloadAdmission::test_selected_topic_and_nonzero_identity() {
  const auto zero="xt=urn:btih:"+std::string(40,'0');
  const auto valid="xt=urn:btih:"+std::string(40,'1');
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(magnet("magnet:?"+zero+"&"+valid)));
  CPPUNIT_ASSERT(core::download_input_zero_infohash(magnet("magnet:?"+valid+"&"+zero)));
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(magnet("magnet:?xt=urn:btih:"+std::string(32,'B'))));
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(magnet("magnet:?dn="+zero)));
  CPPUNIT_ASSERT(!core::download_input_zero_infohash(magnet("magnet:?xt=urn:btmh:1220"+std::string(64,'0'))));
}
