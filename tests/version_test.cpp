#include "core/version.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace wsld {
namespace {

TEST(Version, IsWiredToTheProjectVersion) {
  // The version used to be written out by hand in several places and drifted:
  // the handshake said 0.1, the installer defaulted to 0.1.0, and the project
  // was 0.9.0. Everything reads the generated header now, so this only has to
  // check that the header carries a real version and that the agent strings are
  // built from it rather than repeating a literal.
  EXPECT_FALSE(kVersionString.empty());
  EXPECT_NE(kVersionString, "0.0.0");
  EXPECT_EQ(kVersionString.find_first_not_of("0123456789."), std::string_view::npos)
      << "expected a dotted numeric version, got: " << kVersionString;
  EXPECT_EQ(kVersionString, std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
                                std::to_string(kVersionPatch));

  EXPECT_EQ(agent_string("wsldrive"), "wsldrive/" + std::string(kVersionString));
  EXPECT_EQ(agent_string("wsldrived"), "wsldrived/" + std::string(kVersionString));
  // The handshake stores a view into this, so it has to outlive the call.
  EXPECT_EQ(&agent_string("wsldrive"), &agent_string("wsldrive"));
}

}  // namespace
}  // namespace wsld
