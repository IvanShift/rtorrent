// Stands in for php85 in dispatcher gate tests. It records every launch in
// /data/actor.log, ending with its pid, and keeps update.php workers alive
// for a while, as a running worker would; dispatch.php passes exit at once.
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
  {
    std::ofstream log("/data/actor.log", std::ios::app);
    log << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (int index = 1; index < argc; ++index) log << ' ' << argv[index];
    log << " pid=" << ::getpid() << '\n';
  }
  const std::string script = argc > 1 ? argv[1] : "";
  if (script.size() >= 10 && script.compare(script.size() - 10, 10, "update.php") == 0)
    std::this_thread::sleep_for(std::chrono::seconds(40));
  return 0;
}
