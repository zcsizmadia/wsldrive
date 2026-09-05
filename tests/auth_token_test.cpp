#include "core/auth_token.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wsld {
namespace {

TEST(AuthProof, BindsToBothNoncesAndToTheDirection) {
  const std::string token = "s3cret";
  const std::string cn = "client-nonce";
  const std::string sn = "server-nonce";

  const std::string client = auth_proof(token, cn, sn, /*from_server=*/false);
  const std::string server = auth_proof(token, cn, sn, /*from_server=*/true);

  EXPECT_FALSE(client.empty());
  // The two directions must differ, or the agent's proof could be replayed
  // straight back at it as the client's.
  EXPECT_NE(client, server);

  // Changing either nonce changes the proof, so a captured one is worthless in
  // another session.
  EXPECT_NE(client, auth_proof(token, "other-client-nonce", sn, false));
  EXPECT_NE(client, auth_proof(token, cn, "other-server-nonce", false));
  // And it depends on the secret, which is the whole point.
  EXPECT_NE(client, auth_proof("different-secret", cn, sn, false));

  EXPECT_EQ(client, auth_proof(token, cn, sn, false));  // deterministic
}

TEST(AuthProof, DoesNotContainTheSecret) {
  const std::string token = "an-unmistakable-secret-value";
  const std::string proof = auth_proof(token, "cn", "sn", false);
  EXPECT_EQ(proof.find(token), std::string::npos);
}

TEST(AuthProof, FieldsAreUnambiguouslySeparated) {
  // Without length-prefixing, ("ab","c") and ("a","bc") would hash the same and
  // a peer could shift bytes between the fields.
  EXPECT_NE(auth_proof("ab", "c", "sn", false), auth_proof("a", "bc", "sn", false));
  EXPECT_NE(auth_proof("t", "ab", "c", false), auth_proof("t", "a", "bc", false));
}

TEST(AuthProof, NoncesAreDistinct) {
  const std::string a = generate_auth_nonce();
  const std::string b = generate_auth_nonce();
  ASSERT_FALSE(a.empty());
  EXPECT_NE(a, b);
  EXPECT_GE(a.size(), 32u);  // 128 bits as hex
}

TEST(ConstantTimeEqual, MatchesOnlyIdenticalValues) {
  EXPECT_TRUE(constant_time_equal("abc", "abc"));
  EXPECT_TRUE(constant_time_equal("", ""));
  EXPECT_FALSE(constant_time_equal("abc", "abd"));
  EXPECT_FALSE(constant_time_equal("abc", "ab"));
  EXPECT_FALSE(constant_time_equal("", "a"));
}

}  // namespace
}  // namespace wsld
