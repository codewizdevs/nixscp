// Linked into every test program on macOS.
//
// The configuration is under the home folder there (~/Library/Application
// Support/NixSCP), and XDG_CONFIG_HOME, which keeps a test away from it on Linux,
// means nothing. So a test program gets a home folder of its own before main():
// it does not touch the user's configuration, and it does not find what the test
// before it left there. Programs it starts inherit that home, also itself again.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace
{

std::string Home;
pid_t Owner = 0;

void RemoveHome()
{
  if (getpid() == Owner)
  {
    std::error_code Ignored;
    std::filesystem::remove_all(Home, Ignored);
  }
}

const bool Isolated = []()
{
  if (getenv("NIXSCP_TEST_OWN_HOME") != nullptr)
  {
    return true;
  }
  // short for socket paths, and /private/tmp as /tmp is only a link to it
  char Template[] = "/private/tmp/nxh-XXXXXX";
  if (mkdtemp(Template) == nullptr)
  {
    return false;
  }
  Home = Template;
  Owner = getpid();
  setenv("HOME", Template, 1);
  setenv("NIXSCP_TEST_OWN_HOME", "1", 1);
  atexit(RemoveHome);
  return true;
}();

}
