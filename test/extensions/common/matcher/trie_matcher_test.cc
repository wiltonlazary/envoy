#include <memory>

#include "envoy/config/core/v3/extension.pb.h"
#include "envoy/matcher/matcher.h"
#include "envoy/registry/registry.h"

#include "source/common/matcher/matcher.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/common/matcher/trie_matcher.h"

#include "test/common/matcher/test_utility.h"
#include "test/mocks/matcher/mocks.h"
#include "test/mocks/server/factory_context.h"
#include "test/test_common/registry.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"
#include "xds/type/matcher/v3/matcher.pb.h"
#include "xds/type/matcher/v3/matcher.pb.validate.h"

namespace Envoy {
namespace Extensions {
namespace Common {
namespace Matcher {
namespace {

using ::Envoy::Matcher::ActionFactory;
using ::Envoy::Matcher::CustomMatcherFactory;
using ::Envoy::Matcher::DataInputGetResult;
using ::Envoy::Matcher::MatchTreeFactory;
using ::Envoy::Matcher::MockMatchTreeValidationVisitor;
using ::Envoy::Matcher::StringAction;
using ::Envoy::Matcher::StringActionFactory;
using ::Envoy::Matcher::TestData;
using ::Envoy::Matcher::TestDataInputFactory;

class TrieMatcherTest : public ::testing::Test {
public:
  TrieMatcherTest()
      : inject_action_(action_factory_), inject_matcher_(trie_matcher_factory_),
        factory_(context_, factory_context_, validation_visitor_) {
    EXPECT_CALL(validation_visitor_, performDataInputValidation(_, _)).Times(testing::AnyNumber());
  }

  void loadConfig(const std::string& config) {
    MessageUtil::loadFromYaml(config, matcher_, ProtobufMessage::getStrictValidationVisitor());
    TestUtility::validate(matcher_);
  }
  void validateMatch(const std::string& output) {
    auto match_tree = factory_.create(matcher_);
    const auto result = match_tree()->match(TestData());
    EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
    EXPECT_TRUE(result.on_match_.has_value());
    EXPECT_NE(result.on_match_->action_cb_, nullptr);
    auto action = result.on_match_->action_cb_();
    const auto value = action->getTyped<StringAction>();
    EXPECT_EQ(value.string_, output);
  }
  void validateNoMatch() {
    auto match_tree = factory_.create(matcher_);
    const auto result = match_tree()->match(TestData());
    EXPECT_EQ(result.match_state_, MatchState::MatchComplete);
    EXPECT_FALSE(result.on_match_.has_value());
  }
  void validateUnableToMatch() {
    auto match_tree = factory_.create(matcher_);
    const auto result = match_tree()->match(TestData());
    EXPECT_EQ(result.match_state_, MatchState::UnableToMatch);
  }

  StringActionFactory action_factory_;
  Registry::InjectFactory<ActionFactory<absl::string_view>> inject_action_;
  TrieMatcherFactoryBase<TestData> trie_matcher_factory_;
  Registry::InjectFactory<CustomMatcherFactory<TestData>> inject_matcher_;
  MockMatchTreeValidationVisitor<TestData> validation_visitor_;

  absl::string_view context_ = "";
  NiceMock<Server::Configuration::MockServerFactoryContext> factory_context_;
  MatchTreeFactory<TestData, absl::string_view> factory_;
  xds::type::matcher::v3::Matcher matcher_;
};

TEST_F(TrieMatcherTest, TestMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  custom_match:
    name: ip_matcher
    typed_config:
      "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
      range_matchers:
      - ranges:
        - address_prefix: 192.0.0.0
          prefix_len: 2
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: foo
      - ranges:
        - address_prefix: 192.101.0.0
          prefix_len: 10
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: bar
  )EOF";
  loadConfig(yaml);

  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    validateMatch("foo");
  }
  {
    auto input = TestDataInputFactory("input", "192.101.0.1");
    validateMatch("bar");
  }
  {
    auto input = TestDataInputFactory("input", "128.0.0.1");
    validateNoMatch();
  }
  {
    auto input = TestDataInputFactory("input", "xxx");
    validateNoMatch();
  }
}

TEST_F(TrieMatcherTest, OverlappingMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  custom_match:
    name: ip_matcher
    typed_config:
      "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
      range_matchers:
      - ranges:
        - address_prefix: 128.0.0.0
          prefix_len: 1
        - address_prefix: 192.0.0.0
          prefix_len: 2
        - address_prefix: 192.0.0.0
          prefix_len: 2
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: foo
      - ranges:
        - address_prefix: 255.0.0.0
          prefix_len: 8
        - address_prefix: 192.0.0.0
          prefix_len: 2
        - address_prefix: 192.0.0.1
          prefix_len: 32
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: bar
  )EOF";
  loadConfig(yaml);

  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    validateMatch("foo");
  }
  {
    auto input = TestDataInputFactory("input", "192.0.0.1");
    validateMatch("bar");
  }
  {
    auto input = TestDataInputFactory("input", "255.0.0.1");
    validateMatch("bar");
  }
}

TEST_F(TrieMatcherTest, NestedInclusiveMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  custom_match:
    name: ip_matcher
    typed_config:
      "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
      range_matchers:
      - ranges:
        - address_prefix: 0.0.0.0
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: foo
      - ranges:
        - address_prefix: 192.0.0.0
          prefix_len: 2
        on_match:
          matcher:
            matcher_tree:
              input:
                name: nested
                typed_config:
                  "@type": type.googleapis.com/google.protobuf.StringValue
              exact_match_map:
                map:
                  baz:
                    action:
                      name: test_action
                      typed_config:
                        "@type": type.googleapis.com/google.protobuf.StringValue
                        value: bar
  )EOF";
  loadConfig(yaml);

  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    auto nested = TestDataInputFactory("nested", "baz");
    validateMatch("bar");
  }
  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    auto nested = TestDataInputFactory("nested", "");
    validateMatch("foo");
  }
  {
    auto input = TestDataInputFactory("input", "128.0.0.1");
    auto nested = TestDataInputFactory("nested", "");
    validateMatch("foo");
  }
}

TEST_F(TrieMatcherTest, NestedExclusiveMatcher) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  custom_match:
    name: ip_matcher
    typed_config:
      "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
      range_matchers:
      - ranges:
        - address_prefix: 0.0.0.0
        exclusive: true
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: foo
      - ranges:
        - address_prefix: 192.0.0.0
          prefix_len: 2
        exclusive: true
        on_match:
          matcher:
            matcher_tree:
              input:
                name: nested
                typed_config:
                  "@type": type.googleapis.com/google.protobuf.StringValue
              exact_match_map:
                map:
                  baz:
                    action:
                      name: test_action
                      typed_config:
                        "@type": type.googleapis.com/google.protobuf.StringValue
                        value: bar
  )EOF";
  loadConfig(yaml);

  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    auto nested = TestDataInputFactory("nested", "baz");
    validateMatch("bar");
  }
  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    auto nested = TestDataInputFactory("nested", "");
    validateNoMatch();
  }
  {
    auto input = TestDataInputFactory("input", "128.0.0.1");
    auto nested = TestDataInputFactory("nested", "");
    validateMatch("foo");
  }
}

TEST_F(TrieMatcherTest, RecursiveMatcherTree) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  custom_match:
    name: ip_matcher
    typed_config:
      "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
      range_matchers:
      - ranges:
        - address_prefix: 0.0.0.0
        on_match:
          action:
            name: test_action
            typed_config:
              "@type": type.googleapis.com/google.protobuf.StringValue
              value: foo
      - ranges:
        - address_prefix: 192.0.0.0
          prefix_len: 2
        on_match:
          matcher:
            matcher_tree:
              input:
                name: nested
                typed_config:
                  "@type": type.googleapis.com/google.protobuf.StringValue
              exact_match_map:
                map:
                  bar:
                    action:
                      name: test_action
                      typed_config:
                        "@type": type.googleapis.com/google.protobuf.StringValue
                        value: bar
            on_no_match:
              matcher:
                matcher_tree:
                  input:
                    name: nested
                    typed_config:
                      "@type": type.googleapis.com/google.protobuf.StringValue
                  exact_match_map:
                    map:
                      baz:
                        action:
                          name: test_action
                          typed_config:
                            "@type": type.googleapis.com/google.protobuf.StringValue
                            value: baz
  )EOF";
  loadConfig(yaml);

  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    auto nested = TestDataInputFactory("nested", "baz");
    validateMatch("baz");
  }
  {
    auto input = TestDataInputFactory("input", "192.0.100.1");
    auto nested = TestDataInputFactory("nested", "bar");
    validateMatch("bar");
  }
  {
    auto input = TestDataInputFactory("input", "128.0.0.1");
    auto nested = TestDataInputFactory("nested", "");
    validateMatch("foo");
  }
}

TEST_F(TrieMatcherTest, NoData) {
  const std::string yaml = R"EOF(
matcher_tree:
  input:
    name: input
    typed_config:
      "@type": type.googleapis.com/google.protobuf.StringValue
  custom_match:
    name: ip_matcher
    typed_config:
      "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
      range_matchers:
      - ranges:
        - address_prefix: 0.0.0.0
        on_match:
          matcher:
            matcher_tree:
              input:
                name: nested
                typed_config:
                  "@type": type.googleapis.com/google.protobuf.StringValue
              custom_match:
                name: ip_matcher
                typed_config:
                  "@type": type.googleapis.com/xds.type.matcher.v3.IPMatcher
                  range_matchers:
                  - ranges:
                    - address_prefix: 192.0.0.0
                      prefix_len: 2
                    on_match:
                      action:
                        name: test_action
                        typed_config:
                          "@type": type.googleapis.com/google.protobuf.StringValue
                          value: foo
  )EOF";
  loadConfig(yaml);

  {
    auto input = TestDataInputFactory(
        "input", {DataInputGetResult::DataAvailability::AllDataAvailable, absl::nullopt});
    auto nested = TestDataInputFactory("nested", "");
    validateNoMatch();
  }
  {
    auto input = TestDataInputFactory("input", "127.0.0.1");
    auto nested = TestDataInputFactory(
        "nested", {DataInputGetResult::DataAvailability::AllDataAvailable, absl::nullopt});
    validateNoMatch();
  }
  {
    auto input = TestDataInputFactory(
        "input", {DataInputGetResult::DataAvailability::NotAvailable, absl::nullopt});
    auto nested = TestDataInputFactory("nested", "");
    validateUnableToMatch();
  }
  {
    auto input = TestDataInputFactory("input", "127.0.0.1");
    auto nested = TestDataInputFactory(
        "nested", {DataInputGetResult::DataAvailability::NotAvailable, absl::nullopt});
    validateUnableToMatch();
  }
}

} // namespace
} // namespace Matcher
} // namespace Common
} // namespace Extensions
} // namespace Envoy
