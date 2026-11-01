#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "grpc_flatcc.h"
#include "test_msg_builder.h"
#include "test_msg_verifier.h"
#include "test_noid_builder.h"
#include "test_noid_verifier.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static flatcc_builder_t builder;

static grpc_message *build_echo(const char *text, int32_t number) {
  test_Echo_start_as_root(&builder);
  test_Echo_text_create_str(&builder, text);
  test_Echo_number_add(&builder, number);
  test_Echo_tags_start(&builder);
  test_Echo_tags_push_create_str(&builder, "a");
  test_Echo_tags_push_create_str(&builder, "b");
  test_Echo_tags_end(&builder);
  test_Echo_end_as_root(&builder);
  return grpc_fb_pack(&builder);
}

void test_fb_pack_and_unpack(void) {
  grpc_message *msg = build_echo("Hello from grpc flatcc integration!", 42);
  TEST_ASSERT_NOT_NULL(msg);
  TEST_ASSERT_EQUAL_UINT8(0, msg->cflag);
  TEST_ASSERT_GREATER_THAN_UINT32(0, msg->len);
  /* read in place, so the payload must be aligned */
  TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t)msg->data % alignof(grpc_max_align_t));

  TEST_ASSERT_EQUAL_INT(flatcc_verify_ok, GRPC_FB_VERIFY(test_Echo, msg));
  test_Echo_table_t echo = GRPC_FB_UNPACK(test_Echo, msg);
  TEST_ASSERT_NOT_NULL(echo);
  TEST_ASSERT_EQUAL_STRING("Hello from grpc flatcc integration!", test_Echo_text(echo));
  TEST_ASSERT_EQUAL_INT32(42, test_Echo_number(echo));
  flatbuffers_string_vec_t tags = test_Echo_tags(echo);
  TEST_ASSERT_EQUAL_UINT64(2, (uint64_t)flatbuffers_string_vec_len(tags));
  TEST_ASSERT_EQUAL_STRING("b", flatbuffers_string_vec_at(tags, 1));

  grpc_message_free(msg);
}

void test_fb_builder_reuse(void) {
  /* grpc_fb_pack() resets the builder for the next buffer */
  for (int i = 0; i < 100; i++) {
    char text[32];
    snprintf(text, sizeof(text), "message %d", i);
    grpc_message *msg = build_echo(text, i);
    TEST_ASSERT_NOT_NULL(msg);
    test_Echo_table_t echo = GRPC_FB_UNPACK(test_Echo, msg);
    TEST_ASSERT_NOT_NULL(echo);
    TEST_ASSERT_EQUAL_STRING(text, test_Echo_text(echo));
    TEST_ASSERT_EQUAL_INT32(i, test_Echo_number(echo));
    grpc_message_free(msg);
  }
}

void test_fb_send_null_safety(void) {
  TEST_ASSERT_EQUAL_INT(GRPC_ERR_INVALID, grpc_call_send_fb(nullptr, &builder));
  TEST_ASSERT_EQUAL_INT(GRPC_ERR_INVALID, grpc_call_send_fb(nullptr, nullptr));
}

void test_fb_rejects_invalid(void) {
  grpc_message *msg = build_echo("payload", 7);
  TEST_ASSERT_NOT_NULL(msg);

  /* wrong file identifier */
  TEST_ASSERT_NOT_EQUAL(flatcc_verify_ok, grpc_fb_verify(msg, "XXXX", test_Echo_verify_table));
  TEST_ASSERT_NULL(grpc_fb_unpack(msg, "XXXX", test_Echo_verify_table));

  /* truncated */
  grpc_message *cut = grpc_message_new(msg->data, msg->len / 2);
  TEST_ASSERT_NULL(GRPC_FB_UNPACK(test_Echo, cut));
  grpc_message_free(cut);

  /* root offset pointing outside the buffer */
  grpc_message *bad = grpc_message_new(msg->data, msg->len);
  bad->data[0] = 0xff;
  bad->data[1] = 0xff;
  TEST_ASSERT_NULL(GRPC_FB_UNPACK(test_Echo, bad));
  grpc_message_free(bad);

  /* not a FlatBuffer at all */
  grpc_message *junk = grpc_message_new("junk", 4);
  TEST_ASSERT_NULL(GRPC_FB_UNPACK(test_Echo, junk));
  grpc_message_free(junk);

  /* empty */
  grpc_message *empty = grpc_message_new(nullptr, 0);
  TEST_ASSERT_NULL(GRPC_FB_UNPACK(test_Echo, empty));
  grpc_message_free(empty);

  grpc_message_free(msg);
}

void test_fb_without_identifier(void) {
  test_Ping_start_as_root(&builder);
  test_Ping_value_add(&builder, INT64_C(1) << 40);
  test_Ping_end_as_root(&builder);
  grpc_message *msg = grpc_fb_pack(&builder);
  TEST_ASSERT_NOT_NULL(msg);

  test_Ping_table_t ping = GRPC_FB_UNPACK(test_Ping, msg);
  TEST_ASSERT_NOT_NULL(ping);
  TEST_ASSERT_EQUAL_INT64(INT64_C(1) << 40, test_Ping_value(ping));
  grpc_message_free(msg);
}

void test_fb_null_safety(void) {
  TEST_ASSERT_EQUAL_INT(GRPC_ERR_INVALID, grpc_fb_builder_init(nullptr));
  TEST_ASSERT_NULL(grpc_fb_pack(nullptr));
  TEST_ASSERT_NULL(grpc_fb_unpack(nullptr, nullptr, test_Echo_verify_table));
  TEST_ASSERT_NOT_EQUAL(flatcc_verify_ok, grpc_fb_verify(nullptr, nullptr, nullptr));
  /* nothing was built */
  TEST_ASSERT_NULL(grpc_fb_pack(&builder));
}

int main(void) {
  TEST_ASSERT_EQUAL_INT(GRPC_OK, grpc_fb_builder_init(&builder));
  UNITY_BEGIN();
  RUN_TEST(test_fb_pack_and_unpack);
  RUN_TEST(test_fb_builder_reuse);
  RUN_TEST(test_fb_send_null_safety);
  RUN_TEST(test_fb_rejects_invalid);
  RUN_TEST(test_fb_without_identifier);
  RUN_TEST(test_fb_null_safety);
  int res = UNITY_END();
  flatcc_builder_clear(&builder);
  return res;
}
