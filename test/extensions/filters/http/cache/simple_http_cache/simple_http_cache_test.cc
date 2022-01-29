#include "envoy/http/header_map.h"
#include "envoy/registry/registry.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/extensions/filters/http/cache/cache_headers_utils.h"
#include "source/extensions/filters/http/cache/simple_http_cache/simple_http_cache.h"

#include "test/extensions/filters/http/cache/common.h"
#include "test/mocks/server/factory_context.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Cache {
namespace {

const std::string EpochDate = "Thu, 01 Jan 1970 00:00:00 GMT";

envoy::extensions::filters::http::cache::v3::CacheConfig getConfig() {
  // Allows 'accept' to be varied in the tests.
  envoy::extensions::filters::http::cache::v3::CacheConfig config;
  const auto& add_accept = config.mutable_allowed_vary_headers()->Add();
  add_accept->set_exact("accept");
  return config;
}

class SimpleHttpCacheTest : public testing::Test {
protected:
  SimpleHttpCacheTest() : vary_allow_list_(getConfig().allowed_vary_headers()) {
    request_headers_.setMethod("GET");
    request_headers_.setHost("example.com");
    request_headers_.setScheme("https");
    request_headers_.setCopy(Http::CustomHeaders::get().CacheControl, "max-age=3600");
  }

  // Updates the cache entry's header
  void updateHeaders(LookupContext& lookup, const Http::TestResponseHeaderMapImpl& response_headers,
                     const ResponseMetadata& metadata) {
    cache_.updateHeaders(lookup, response_headers, metadata);
  }

  void updateHeaders(absl::string_view request_path,
                     const Http::TestResponseHeaderMapImpl& response_headers,
                     const ResponseMetadata& metadata) {
    LookupRequest request = makeLookupRequest(request_path);
    LookupContextPtr context = cache_.makeLookupContext(std::move(request), decoder_callbacks_);
    updateHeaders(*context, response_headers, metadata);
  }

  // Performs a cache lookup.
  LookupContextPtr lookup(absl::string_view request_path) {
    LookupRequest request = makeLookupRequest(request_path);

    LookupContextPtr context = cache_.makeLookupContext(std::move(request), decoder_callbacks_);
    context->getHeaders([this](LookupResult&& result) { lookup_result_ = std::move(result); });
    return context;
  }

  // Inserts a value into the cache.
  void insert(LookupContextPtr lookup, const Http::TestResponseHeaderMapImpl& response_headers,
              const absl::string_view response_body,
              const Http::TestResponseTrailerMapImpl& response_trailers = {}) {
    InsertContextPtr inserter = cache_.makeInsertContext(move(lookup), encoder_callbacks_);
    const ResponseMetadata metadata = {time_source_.systemTime()};
    inserter->insertHeaders(response_headers, metadata, false);
    inserter->insertBody(
        Buffer::OwnedImpl(response_body),
        [&](bool) { inserter->insertTrailers(response_trailers); }, response_trailers.empty());
  }

  void insert(absl::string_view request_path,
              const Http::TestResponseHeaderMapImpl& response_headers,
              const absl::string_view response_body) {
    insert(lookup(request_path), response_headers, response_body);
  }

  std::string getBody(LookupContext& context, uint64_t start, uint64_t end) {
    AdjustedByteRange range(start, end);
    std::string body;
    context.getBody(range, [&body](Buffer::InstancePtr&& data) {
      EXPECT_NE(data, nullptr);
      if (data) {
        body = data->toString();
      }
    });
    return body;
  }

  Http::TestResponseTrailerMapImpl getTrailers(LookupContext& context) {
    Http::TestResponseTrailerMapImpl trailers;
    context.getTrailers([&trailers](Http::ResponseTrailerMapPtr&& data) {
      if (data) {
        trailers = *data;
      }
    });
    return trailers;
  }

  Http::ResponseHeaderMapPtr getHeaders(LookupContext& context) {
    Http::ResponseHeaderMapPtr response_headers_ptr;
    context.getHeaders([&response_headers_ptr](LookupResult&& lookup_result) {
      EXPECT_NE(lookup_result.cache_entry_status_, CacheEntryStatus::Unusable);
      EXPECT_NE(lookup_result.headers_, nullptr);
      response_headers_ptr = move(lookup_result.headers_);
    });
    return response_headers_ptr;
  }

  LookupRequest makeLookupRequest(absl::string_view request_path) {
    request_headers_.setPath(request_path);
    return LookupRequest(request_headers_, time_source_.systemTime(), vary_allow_list_);
  }

  AssertionResult
  expectLookupSuccessWithBodyAndTrailers(LookupContext* lookup_context, absl::string_view body,
                                         Http::TestResponseTrailerMapImpl trailers = {}) {
    if (lookup_result_.cache_entry_status_ != CacheEntryStatus::Ok) {
      return AssertionFailure() << "Expected: lookup_result_.cache_entry_status == "
                                   "CacheEntryStatus::Ok\n  Actual: "
                                << lookup_result_.cache_entry_status_;
    }
    if (!lookup_result_.headers_) {
      return AssertionFailure() << "Expected nonnull lookup_result_.headers";
    }
    if (!lookup_context) {
      return AssertionFailure() << "Expected nonnull lookup_context";
    }
    const std::string actual_body = getBody(*lookup_context, 0, body.size());
    if (body != actual_body) {
      return AssertionFailure() << "Expected body == " << body << "\n  Actual:  " << actual_body;
    }
    if (lookup_result_.has_trailers_) {
      const Http::TestResponseTrailerMapImpl actual_trailers = getTrailers(*lookup_context);
      if (trailers != actual_trailers) {
        return AssertionFailure() << "Expected trailers == " << trailers
                                  << "\n  Actual:  " << actual_trailers;
      }
    }
    return AssertionSuccess();
  }

  AssertionResult expectLookupSuccessWithHeaders(LookupContext* lookup_context,
                                                 const Http::TestResponseHeaderMapImpl& headers) {
    if (lookup_result_.cache_entry_status_ != CacheEntryStatus::Ok) {
      return AssertionFailure() << "Expected: lookup_result_.cache_entry_status == "
                                   "CacheEntryStatus::Ok\n  Actual: "
                                << lookup_result_.cache_entry_status_;
    }
    if (!lookup_result_.headers_) {
      return AssertionFailure() << "Expected nonnull lookup_result_.headers";
    }
    if (!lookup_context) {
      return AssertionFailure() << "Expected nonnull lookup_context";
    }

    Http::ResponseHeaderMapPtr actual_headers_ptr = getHeaders(*lookup_context);
    if (!TestUtility::headerMapEqualIgnoreOrder(headers, *actual_headers_ptr)) {
      return AssertionFailure() << "Expected headers: " << headers
                                << "\nActual:  " << *actual_headers_ptr;
    }
    return AssertionSuccess();
  }

  SimpleHttpCache cache_;
  LookupResult lookup_result_;
  Http::TestRequestHeaderMapImpl request_headers_;
  Event::SimulatedTimeSystem time_source_;
  DateFormatter formatter_{"%a, %d %b %Y %H:%M:%S GMT"};
  VaryAllowList vary_allow_list_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
};

// Simple flow of putting in an item, getting it, deleting it.
TEST_F(SimpleHttpCacheTest, PutGet) {
  const std::string request_path1("/name");
  LookupContextPtr name_lookup_context = lookup(request_path1);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"cache-control", "public,max-age=3600"}};

  const std::string body1("Value");
  insert(move(name_lookup_context), response_headers, body1);
  name_lookup_context = lookup(request_path1);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(name_lookup_context.get(), body1));

  const std::string& request_path2("/another-name");
  LookupContextPtr another_name_lookup_context = lookup(request_path2);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  const std::string new_body1("NewValue");
  insert(move(name_lookup_context), response_headers, new_body1);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(lookup(request_path1).get(), new_body1));
}

TEST_F(SimpleHttpCacheTest, PrivateResponse) {
  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"age", "2"},
      {"cache-control", "private,max-age=3600"}};
  const std::string request_path("/name");

  LookupContextPtr name_lookup_context = lookup(request_path);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  const std::string Body("Value");
  // We must make sure at cache insertion time, private responses must not be
  // inserted. However, if the insertion did happen, it would be served at the
  // time of lookup.
  insert(move(name_lookup_context), response_headers, Body);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(lookup(request_path).get(), Body));
}

TEST_F(SimpleHttpCacheTest, Miss) {
  LookupContextPtr name_lookup_context = lookup("/name");
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
}

TEST_F(SimpleHttpCacheTest, Fresh) {
  const std::string time_value_1 = formatter_.fromTime(time_source_.systemTime());
  const Http::TestResponseHeaderMapImpl response_headers = {
      {"date", time_value_1}, {"cache-control", "public, max-age=3600"}};
  // TODO(toddmgreer): Test with various date headers.
  insert("/", response_headers, "");
  time_source_.advanceTimeWait(Seconds(3600));
  lookup("/");
  EXPECT_EQ(CacheEntryStatus::Ok, lookup_result_.cache_entry_status_);
}

TEST_F(SimpleHttpCacheTest, Stale) {
  const std::string time_value_1 = formatter_.fromTime(time_source_.systemTime());
  const Http::TestResponseHeaderMapImpl response_headers = {
      {"date", time_value_1}, {"cache-control", "public, max-age=3600"}};
  // TODO(toddmgreer): Test with various date headers.
  insert("/", response_headers, "");
  time_source_.advanceTimeWait(Seconds(3601));
  lookup("/");

  EXPECT_EQ(CacheEntryStatus::RequiresValidation, lookup_result_.cache_entry_status_);
}

TEST_F(SimpleHttpCacheTest, RequestSmallMinFresh) {
  request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl, "min-fresh=1000");
  const std::string request_path("/name");
  LookupContextPtr name_lookup_context = lookup(request_path);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"age", "6000"},
      {"cache-control", "public, max-age=9000"}};
  const std::string Body("Value");
  insert(move(name_lookup_context), response_headers, Body);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(lookup(request_path).get(), Body));
}

TEST_F(SimpleHttpCacheTest, ResponseStaleWithRequestLargeMaxStale) {
  request_headers_.setReferenceKey(Http::CustomHeaders::get().CacheControl, "max-stale=9000");

  const std::string request_path("/name");
  LookupContextPtr name_lookup_context = lookup(request_path);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"age", "7200"},
      {"cache-control", "public, max-age=3600"}};

  const std::string Body("Value");
  insert(move(name_lookup_context), response_headers, Body);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(lookup(request_path).get(), Body));
}

TEST_F(SimpleHttpCacheTest, StreamingPut) {
  const std::string RequestPath("request_path");
  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"age", "2"},
      {"cache-control", "public, max-age=3600"}};
  InsertContextPtr inserter = cache_.makeInsertContext(lookup(RequestPath), encoder_callbacks_);
  const ResponseMetadata metadata = {time_source_.systemTime()};

  inserter->insertHeaders(response_headers, metadata, false);
  inserter->insertBody(
      Buffer::OwnedImpl("Hello, "), [](bool ready) { EXPECT_TRUE(ready); }, false);
  inserter->insertBody(Buffer::OwnedImpl("World!"), nullptr, true);
  LookupContextPtr name_lookup_context = lookup(RequestPath);
  EXPECT_EQ(CacheEntryStatus::Ok, lookup_result_.cache_entry_status_);
  EXPECT_NE(nullptr, lookup_result_.headers_);
  ASSERT_EQ(13, lookup_result_.content_length_);
  EXPECT_EQ("Hello, World!", getBody(*name_lookup_context, 0, 13));
}

TEST(Registration, GetFactory) {
  HttpCacheFactory* factory = Registry::FactoryRegistry<HttpCacheFactory>::getFactoryByType(
      "envoy.extensions.cache.simple_http_cache.v3.SimpleHttpCacheConfig");
  ASSERT_NE(factory, nullptr);
  envoy::extensions::filters::http::cache::v3::CacheConfig config;
  testing::NiceMock<Server::Configuration::MockFactoryContext> factory_context;
  config.mutable_typed_config()->PackFrom(*factory->createEmptyConfigProto());
  EXPECT_EQ(factory->getCache(config, factory_context).cacheInfo().name_,
            "envoy.extensions.http.cache.simple");
}

TEST_F(SimpleHttpCacheTest, VaryResponses) {
  // Responses will vary on accept.
  const std::string RequestPath("some-resource");
  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"cache-control", "public,max-age=3600"},
      {"vary", "accept"}};

  // First request.
  request_headers_.setCopy(Http::LowerCaseString("accept"), "image/*");
  LookupContextPtr first_value_vary = lookup(RequestPath);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
  const std::string Body1("accept is image/*");
  insert(move(first_value_vary), response_headers, Body1);
  first_value_vary = lookup(RequestPath);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(first_value_vary.get(), Body1));

  // Second request with a different value for the varied header.
  request_headers_.setCopy(Http::LowerCaseString("accept"), "text/html");
  LookupContextPtr second_value_vary = lookup(RequestPath);
  // Should miss because we don't have this version of the response saved yet.
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
  // Add second version and make sure we receive the correct one..
  const std::string Body2("accept is text/html");
  insert(move(second_value_vary), response_headers, Body2);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(lookup(RequestPath).get(), Body2));

  request_headers_.setCopy(Http::LowerCaseString("accept"), "image/*");
  LookupContextPtr first_value_lookup2 = lookup(RequestPath);
  // Looks up first version again to be sure it wasn't replaced with the second one.
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(first_value_lookup2.get(), Body1));

  // Create a new allow list to make sure a now disallowed cached vary entry is not served.
  Protobuf::RepeatedPtrField<::envoy::type::matcher::v3::StringMatcher> proto_allow_list;
  ::envoy::type::matcher::v3::StringMatcher* matcher = proto_allow_list.Add();
  matcher->set_exact("width");
  vary_allow_list_ = VaryAllowList(proto_allow_list);
  lookup(RequestPath);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
}

TEST_F(SimpleHttpCacheTest, VaryOnDisallowedKey) {
  // Responses will vary on accept.
  const std::string RequestPath("some-resource");
  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"cache-control", "public,max-age=3600"},
      {"vary", "user-agent"}};

  // First request.
  request_headers_.setCopy(Http::LowerCaseString("user-agent"), "user_agent_one");
  LookupContextPtr first_value_vary = lookup(RequestPath);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
  const std::string Body1("one");
  insert(move(first_value_vary), response_headers, Body1);
  first_value_vary = lookup(RequestPath);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
}

TEST_F(SimpleHttpCacheTest, UpdateHeadersAndMetadata) {
  const std::string request_path_1("/name");
  const std::string time_value_1 = formatter_.fromTime(time_source_.systemTime());
  Http::TestResponseHeaderMapImpl response_headers{{"date", time_value_1},
                                                   {"cache-control", "public,max-age=3600"}};
  Http::TestResponseHeaderMapImpl response_headers_with_age(response_headers);
  response_headers_with_age.setReferenceKey(Http::LowerCaseString("age"), "0");

  insert(request_path_1, response_headers, "body");
  EXPECT_TRUE(
      expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_with_age));

  // Update the date field in the headers
  time_source_.advanceTimeWait(Seconds(3601));
  const SystemTime time_2 = time_source_.systemTime();
  const std::string time_value_2 = formatter_.fromTime(time_2);
  Http::TestResponseHeaderMapImpl response_headers_2 = Http::TestResponseHeaderMapImpl{
      {"date", time_value_2}, {"cache-control", "public,max-age=3600"}};
  Http::TestResponseHeaderMapImpl response_headers_with_age_2(response_headers_2);
  response_headers_with_age_2.setReferenceKey(Http::LowerCaseString("age"), "0");

  updateHeaders(request_path_1, response_headers_2, {time_2});
  EXPECT_TRUE(
      expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_with_age_2));
}

TEST_F(SimpleHttpCacheTest, UpdateHeadersForMissingKey) {
  const std::string request_path_1("/name");
  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"cache-control", "public,max-age=3600"}};
  updateHeaders(request_path_1, response_headers, {time_source_.systemTime()});
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);
}

TEST_F(SimpleHttpCacheTest, UpdateHeadersDisabledForVaryHeaders) {
  const std::string request_path_1("/name");
  const std::string time_value_1 = formatter_.fromTime(time_source_.systemTime());
  Http::TestResponseHeaderMapImpl response_headers_1{{"date", time_value_1},
                                                     {"cache-control", "public,max-age=3600"},
                                                     {"accept", "image/*"},
                                                     {"vary", "accept"}};
  insert(request_path_1, response_headers_1, "body");
  // An age header is inserted by `makeLookUpResult`
  response_headers_1.setReferenceKey(Http::LowerCaseString("age"), "0");
  EXPECT_TRUE(expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_1));

  // Update the date field in the headers
  time_source_.advanceTimeWait(Seconds(3600));
  const SystemTime time_2 = time_source_.systemTime();
  const std::string time_value_2 = formatter_.fromTime(time_2);
  Http::TestResponseHeaderMapImpl response_headers_2{{"date", time_value_2},
                                                     {"cache-control", "public,max-age=3600"},
                                                     {"accept", "image/*"},
                                                     {"vary", "accept"}};
  updateHeaders(request_path_1, response_headers_2, {time_2});
  response_headers_1.setReferenceKey(Http::LowerCaseString("age"), "3600");
  // the age is still 0 because an entry is considered fresh after validation
  EXPECT_TRUE(expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_1));
}

TEST_F(SimpleHttpCacheTest, UpdateHeadersSkipEtagHeader) {
  const std::string request_path_1("/name");
  const std::string time_value_1 = formatter_.fromTime(time_source_.systemTime());
  Http::TestResponseHeaderMapImpl response_headers_1{
      {"date", time_value_1}, {"cache-control", "public,max-age=3600"}, {"etag", "0000-0000"}};
  insert(request_path_1, response_headers_1, "body");
  // An age header is inserted by `makeLookUpResult`
  response_headers_1.setReferenceKey(Http::LowerCaseString("age"), "0");
  EXPECT_TRUE(expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_1));

  // Update the date field in the headers
  time_source_.advanceTimeWait(Seconds(3601));
  const SystemTime time_2 = time_source_.systemTime();
  const std::string time_value_2 = formatter_.fromTime(time_2);
  Http::TestResponseHeaderMapImpl response_headers_2{
      {"date", time_value_2}, {"cache-control", "public,max-age=3600"}, {"etag", "1111-1111"}};
  // The etag header should not be updated
  Http::TestResponseHeaderMapImpl response_headers_3{
      {"date", time_value_2}, {"cache-control", "public,max-age=3600"}, {"etag", "0000-0000"}};

  updateHeaders(request_path_1, response_headers_2, {time_2});
  response_headers_3.setReferenceKey(Http::LowerCaseString("age"), "0");
  EXPECT_TRUE(expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_3));
}

TEST_F(SimpleHttpCacheTest, UpdateHeadersSkipSpecificHeaders) {
  const std::string request_path_1("/name");
  const std::string time_value_1 = formatter_.fromTime(time_source_.systemTime());

  // Vary not tested because we have separate tests that cover it
  Http::TestResponseHeaderMapImpl origin_response_headers{
      {"date", time_value_1},
      {"cache-control", "public,max-age=3600"},
      {"content-range", "bytes 200-1000/67589"},
      {"content-length", "800"},
      {"etag", "0000-0000"},
      {"etag", "1111-1111"},
      {"link", "<https://example.com>; rel=\"preconnect\""}};
  insert(request_path_1, origin_response_headers, "body");

  // An age header is inserted by `makeLookUpResult`
  origin_response_headers.setReferenceKey(Http::LowerCaseString("age"), "0");
  EXPECT_TRUE(
      expectLookupSuccessWithHeaders(lookup(request_path_1).get(), origin_response_headers));
  time_source_.advanceTimeWait(Seconds(100));

  const SystemTime time_2 = time_source_.systemTime();
  const std::string time_value_2 = formatter_.fromTime(time_2);
  Http::TestResponseHeaderMapImpl incoming_response_headers{
      {"date", time_value_2},
      {"cache-control", "public,max-age=3600"},
      {"content-range", "bytes 5-1000/67589"},
      {"content-length", "995"},
      {"content-length", "996"},
      {"age", "20"},
      {"etag", "2222-2222"},
      {"link", "<https://changed.com>; rel=\"preconnect\""}};

  // The skipped headers should not be updated
  // "age" and "link" should be updated
  Http::TestResponseHeaderMapImpl expected_response_headers{
      {"date", time_value_2},
      {"cache-control", "public,max-age=3600"},
      {"content-range", "bytes 200-1000/67589"},
      {"content-length", "800"},
      {"age", "20"},
      {"etag", "0000-0000"},
      {"etag", "1111-1111"},
      {"link", "<https://changed.com>; rel=\"preconnect\""}};

  updateHeaders(request_path_1, incoming_response_headers, {time_2});
  EXPECT_TRUE(
      expectLookupSuccessWithHeaders(lookup(request_path_1).get(), expected_response_headers));
}

TEST_F(SimpleHttpCacheTest, UpdateHeadersWithMultivalue) {
  const std::string request_path_1("/name");

  const SystemTime time_1 = time_source_.systemTime();
  const std::string time_value_1(formatter_.fromTime(time_1));
  // Vary not tested because we have separate tests that cover it
  Http::TestResponseHeaderMapImpl response_headers_1{
      {"date", time_value_1},
      {"cache-control", "public,max-age=3600"},
      {"link", "<https://www.example.com>; rel=\"preconnect\""},
      {"link", "<https://example.com>; rel=\"preconnect\""}};
  insert(request_path_1, response_headers_1, "body");

  // An age header is inserted by `makeLookUpResult`
  response_headers_1.setReferenceKey(Http::LowerCaseString("age"), "0");
  EXPECT_TRUE(expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_1));

  Http::TestResponseHeaderMapImpl response_headers_2{
      {"date", time_value_1},
      {"cache-control", "public,max-age=3600"},
      {"link", "<https://www.another-example.com>; rel=\"preconnect\""},
      {"link", "<https://another-example.com>; rel=\"preconnect\""}};

  updateHeaders(request_path_1, response_headers_2, {time_1});

  response_headers_2.setReferenceKey(Http::LowerCaseString("age"), "0");
  EXPECT_TRUE(expectLookupSuccessWithHeaders(lookup(request_path_1).get(), response_headers_2));
}
TEST_F(SimpleHttpCacheTest, PutGetWithTrailers) {
  const std::string request_path1("/name");
  LookupContextPtr name_lookup_context = lookup(request_path1);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"cache-control", "public,max-age=3600"}};
  const std::string body1("Value");
  Http::TestResponseTrailerMapImpl response_trailers{{"why", "is"}, {"sky", "blue"}};
  insert(move(name_lookup_context), response_headers, body1, response_trailers);
  name_lookup_context = lookup(request_path1);
  EXPECT_TRUE(
      expectLookupSuccessWithBodyAndTrailers(name_lookup_context.get(), body1, response_trailers));

  const std::string& request_path2("/another-name");
  LookupContextPtr another_name_lookup_context = lookup(request_path2);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  const std::string new_body1("NewValue");
  insert(move(name_lookup_context), response_headers, new_body1, response_trailers);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(lookup(request_path1).get(), new_body1,
                                                     response_trailers));
  EXPECT_TRUE(lookup_result_.has_trailers_);
}

TEST_F(SimpleHttpCacheTest, EmptyTrailers) {
  const std::string request_path1("/name");
  LookupContextPtr name_lookup_context = lookup(request_path1);
  EXPECT_EQ(CacheEntryStatus::Unusable, lookup_result_.cache_entry_status_);

  Http::TestResponseHeaderMapImpl response_headers{
      {"date", formatter_.fromTime(time_source_.systemTime())},
      {"cache-control", "public,max-age=3600"}};
  const std::string body1("Value");
  insert(move(name_lookup_context), response_headers, body1);
  name_lookup_context = lookup(request_path1);
  EXPECT_TRUE(expectLookupSuccessWithBodyAndTrailers(name_lookup_context.get(), body1));
  EXPECT_FALSE(lookup_result_.has_trailers_);
}

} // namespace
} // namespace Cache
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
