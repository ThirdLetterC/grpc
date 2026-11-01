#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <uv.h>
#include "grpc.h"
#include "grpc_timeout.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_timeout_parsing(void) {
  // Valid formats
  TEST_ASSERT_EQUAL_INT64(10, grpc_timeout_parse_ms("10m"));
  TEST_ASSERT_EQUAL_INT64(1000, grpc_timeout_parse_ms("1S"));
  TEST_ASSERT_EQUAL_INT64(60000, grpc_timeout_parse_ms("1M"));
  TEST_ASSERT_EQUAL_INT64(3600000, grpc_timeout_parse_ms("1H"));

  // Microseconds / Nanoseconds rounding up
  TEST_ASSERT_EQUAL_INT64(1, grpc_timeout_parse_ms("1u"));
  TEST_ASSERT_EQUAL_INT64(1, grpc_timeout_parse_ms("1000u"));
  TEST_ASSERT_EQUAL_INT64(2, grpc_timeout_parse_ms("1001u"));
  TEST_ASSERT_EQUAL_INT64(1, grpc_timeout_parse_ms("1n"));
  TEST_ASSERT_EQUAL_INT64(1, grpc_timeout_parse_ms("1000000n"));
  TEST_ASSERT_EQUAL_INT64(2, grpc_timeout_parse_ms("1000001n"));

  // Multiple digits
  TEST_ASSERT_EQUAL_INT64(12345, grpc_timeout_parse_ms("12345m"));

  // Invalid formats
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms(nullptr));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms(""));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms("abc"));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms("10"));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms("10X"));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms("0S"));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms("-5S"));
  TEST_ASSERT_EQUAL_INT64(-1, grpc_timeout_parse_ms("10Sextra"));
}

void test_time_now_ms(void) {
  uint64_t t1 = grpc_time_now_ms();
  uv_sleep(10);
  uint64_t t2 = grpc_time_now_ms();
  TEST_ASSERT_TRUE(t2 >= t1 + 9); // Allow small jitter
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_timeout_parsing);
  RUN_TEST(test_time_now_ms);
  return UNITY_END();
}
