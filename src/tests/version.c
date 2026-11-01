#include <stdio.h>
#include "grpc_version.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_version(void) {
  char expected[64];
  snprintf(expected, sizeof(expected), "%u.%u.%u", (unsigned)GRPC_VERSION_MAJOR,
           (unsigned)GRPC_VERSION_MINOR, (unsigned)GRPC_VERSION_PATCH);
  TEST_ASSERT_EQUAL_STRING(expected, GRPC_VERSION_STRING);
  TEST_ASSERT_NOT_NULL(grpc_version());
  TEST_ASSERT_EQUAL_STRING(GRPC_VERSION_STRING, grpc_version());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_version);
  return UNITY_END();
}
