#pragma once

#include <algorithm>
#include <string>
#include <torrent/object.h>
#include <torrent/utils/string_manip.h>

namespace core {
// libtorrent uses metadata `pieces` as the identity, and its tracker manager
// treats zero as an internal invariant failure. Check input before download_add.
inline bool download_input_zero_infohash(const torrent::Object& object) {
  if (!object.is_map()) return false;
  if(object.has_key_map("info")) {
    const auto& info=object.get_key("info");
    return info.has_key_value("meta_download") && info.get_key_value("meta_download")!=0 &&
        info.has_key_string("pieces") && info.get_key_string("pieces")==std::string(20,'\0');
  }
  if(!object.has_key_string("magnet-uri")) return false;
  const auto& uri=object.get_key_string("magnet-uri");
  if(uri.rfind("magnet:?",0)!=0) return false;
  bool selected_zero=false;
  size_t begin=8;
  while(begin<uri.size()) {
    // Tokenize like libtorrent's parse_magnet_uri(): the tag runs to '=' across
    // any '&', the value to the next '&'. A piece without '=' (also the empty
    // piece of "&&") joins the next tag, so "x&xt" is no exact topic there.
    const auto equal=uri.find('=',begin);
    if(equal==std::string::npos) return false; // libtorrent refuses the URI.
    auto end=uri.find('&',equal+1);
    if(end==std::string::npos) end=uri.size();
    if(uri.compare(begin,equal-begin,"xt")==0 && uri.compare(equal+1,9,"urn:btih:")==0) {
      const auto raw=uri.substr(equal+10,end-equal-10);
      // Match the current libtorrent parser's last btih and accepted encodings.
      // Unrelated topics, including a hybrid btmh, do not select an identity.
      selected_zero=raw.size()==32 && std::all_of(raw.begin(),raw.end(),[](char c){return c=='A'||c=='a';});
      if(!selected_zero) {
        std::string decoded;
        for(size_t i=0;i<raw.size();++i) {
          if(raw[i]!='%') { decoded+=raw[i]; continue; }
          if(raw.size()-i<3) return false; // libtorrent refuses malformed encoding.
          const auto high=torrent::utils::hex_to_value_or_error(raw[i+1]);
          const auto low=torrent::utils::hex_to_value_or_error(raw[i+2]);
          if(high==static_cast<char>(-1)||low==static_cast<char>(-1)) return false;
          decoded+=static_cast<char>((high<<4)|low); i+=2;
        }
        selected_zero=decoded==std::string(20,'\0') || decoded==std::string(40,'0');
      }
    }
    begin=end+1;
  }
  return selected_zero;
}
}
