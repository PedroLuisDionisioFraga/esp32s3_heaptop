#include <stdint.h>
#include <string.h>

#include "heaptop_json.h"
#include "unity.h"

#define OUT_BYTES 16384
#define CHUNK_MAX 512 /* the writer's buffer */

typedef struct sink_state
{
  char out[OUT_BYTES];
  size_t len;
  size_t max_chunk;
  int calls;     /* sink calls, including one that was refused */
  int chunks;    /* chunks accepted */
  int refuse_at; /* refuse the call with this number (1-based); 0 = never */
} sink_state_t;

static sink_state_t s_sink;
static heaptop_storage_t s_storage;

void setUp(void)
{
  memset(&s_sink, 0, sizeof(s_sink));
  memset(&s_storage, 0, sizeof(s_storage));
}

void tearDown(void)
{
}

static bool _sink(const char *data, size_t len, void *ctx)
{
  sink_state_t *st = ctx;
  st->calls++;
  if (st->refuse_at != 0 && st->calls == st->refuse_at)
    return false;
  TEST_ASSERT_TRUE_MESSAGE(len > 0, "empty chunk");
  TEST_ASSERT_TRUE_MESSAGE(len <= CHUNK_MAX, "chunk larger than the writer's buffer");
  TEST_ASSERT_TRUE_MESSAGE(st->len + len < OUT_BYTES, "test buffer too small");
  memcpy(st->out + st->len, data, len);
  st->len += len;
  st->out[st->len] = '\0';
  st->chunks++;
  if (len > st->max_chunk)
    st->max_chunk = len;
  return true;
}

static bool _write(void)
{
  return heaptop_json_storage(&s_storage, _sink, &s_sink);
}

static int _count(const char *text, const char *needle)
{
  int n = 0;
  for (const char *p = strstr(text, needle); p != NULL; p = strstr(p + strlen(needle), needle)) n++;
  return n;
}

/* Structural check: one object on one line, brackets balanced outside strings, nothing after the last brace. */
static void _assert_json_doc(const char *text, size_t len)
{
  TEST_ASSERT_TRUE(len > 2);
  TEST_ASSERT_EQUAL_INT(0, strncmp(text, "{\"flash_size\":", 14));
  TEST_ASSERT_EQUAL_CHAR('}', text[len - 1]);
  TEST_ASSERT_NULL_MESSAGE(memchr(text, '\n', len), "a document is one line without a newline");
  int depth = 0;
  bool in_str = false;
  for (size_t i = 0; i < len; i++)
  {
    const char c = text[i];
    if (in_str)
    {
      if (c == '\\')
        i++;
      else if (c == '"')
        in_str = false;
      continue;
    }
    if (c == '"')
    {
      in_str = true;
    }
    else if (c == '{' || c == '[')
    {
      depth++;
    }
    else if (c == '}' || c == ']')
    {
      depth--;
      TEST_ASSERT_TRUE_MESSAGE(depth >= 0, "unbalanced brackets");
      TEST_ASSERT_TRUE_MESSAGE(depth > 0 || i == len - 1, "document ends before its last byte");
    }
  }
  TEST_ASSERT_FALSE_MESSAGE(in_str, "unterminated string");
  TEST_ASSERT_EQUAL_INT(0, depth);
}

static void _partition(int i, const char *label, uint8_t type, uint8_t subtype, uint32_t address, uint32_t size,
                       bool running)
{
  heaptop_partition_t *p = &s_storage.partitions[i];
  strncpy(p->label, label, sizeof(p->label) - 1);
  p->type = type;
  p->subtype = subtype;
  p->address = address;
  p->size = size;
  p->running = running;
  if (i >= s_storage.partition_count)
    s_storage.partition_count = (uint8_t)(i + 1);
}

static void _namespace(int i, const char *name, uint32_t entries)
{
  heaptop_nvs_namespace_t *ns = &s_storage.namespaces[i];
  strncpy(ns->name, name, sizeof(ns->name) - 1);
  ns->entries = entries;
  if (i >= s_storage.namespace_count)
    s_storage.namespace_count = (uint8_t)(i + 1);
}

static void _fill(void)
{
  s_storage.flash_size = 4194304;
  _partition(0, "nvs", 1, 0x02, 0x9000, 0x10000, false);
  _partition(1, "ota_0", 0, 0x10, 0x20000, 0x180000, true);
  _partition(2, "www", 1, 0x83, 0x320000, 0xE0000, false);
  s_storage.nvs_present = true;
  strcpy(s_storage.nvs_label, "nvs");
  s_storage.nvs_entry_bytes = 32;
  s_storage.nvs_total = 1890;
  s_storage.nvs_used = 700;
  s_storage.nvs_available = 1100;
  _namespace(0, "evt_log", 540);
  _namespace(1, "storage", 12);
}

static void test_document_fields(void)
{
  _fill();
  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_EQUAL_STRING(
    "{\"flash_size\":4194304,\"partitions\":["
    "{\"label\":\"nvs\",\"type\":\"data\",\"subtype\":\"nvs\",\"address\":36864,\"size\":65536,"
    "\"encrypted\":false,\"running\":false},"
    "{\"label\":\"ota_0\",\"type\":\"app\",\"subtype\":\"ota_0\",\"address\":131072,\"size\":1572864,"
    "\"encrypted\":false,\"running\":true},"
    "{\"label\":\"www\",\"type\":\"data\",\"subtype\":\"littlefs\",\"address\":3276800,\"size\":917504,"
    "\"encrypted\":false,\"running\":false}],\"partitions_truncated\":false,"
    "\"nvs\":{\"label\":\"nvs\",\"entry_bytes\":32,\"total\":1890,\"used\":700,\"available\":1100,"
    "\"namespaces\":[{\"name\":\"evt_log\",\"entries\":540},{\"name\":\"storage\",\"entries\":12}],"
    "\"namespaces_truncated\":false}}",
    s_sink.out);
}

static void test_an_unread_chip_and_a_missing_nvs_are_null(void)
{
  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_EQUAL_STRING("{\"flash_size\":null,\"partitions\":[],\"partitions_truncated\":false,\"nvs\":null}",
                           s_sink.out);
}

static void test_encrypted_and_truncation_flags(void)
{
  _fill();
  s_storage.partitions[0].encrypted = true;
  s_storage.partitions_truncated = true;
  s_storage.namespaces_truncated = true;
  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"size\":65536,\"encrypted\":true,\"running\":false}"));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"partitions_truncated\":true,"));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"namespaces_truncated\":true}}"));
}

static void test_known_types_and_subtypes_have_names_the_rest_are_hex(void)
{
  _partition(0, "boot", 2, 0x00, 0x0, 0x8000, false);
  _partition(1, "table", 3, 0x01, 0x8000, 0x1000, false);
  _partition(2, "a", 0, 0x00, 0x20000, 0x1000, false);
  _partition(3, "b", 0, 0x1f, 0x30000, 0x1000, false);
  _partition(4, "c", 0, 0x20, 0x40000, 0x1000, false);
  _partition(5, "d", 1, 0x00, 0x50000, 0x1000, false);
  _partition(6, "e", 1, 0x04, 0x60000, 0x1000, false);
  _partition(7, "f", 1, 0x82, 0x70000, 0x1000, false);
  _partition(8, "g", 1, 0x99, 0x80000, 0x1000, false);
  _partition(9, "h", 0x7f, 0x05, 0x90000, 0x1000, false);
  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"bootloader\",\"subtype\":\"primary\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"partition_table\",\"subtype\":\"ota\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"app\",\"subtype\":\"factory\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"app\",\"subtype\":\"ota_15\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"app\",\"subtype\":\"test\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"data\",\"subtype\":\"ota\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"data\",\"subtype\":\"nvs_keys\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"type\":\"data\",\"subtype\":\"spiffs\""));
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_sink.out, "\"type\":\"data\",\"subtype\":\"0x99\""),
                               "an unknown subtype is its hex value");
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_sink.out, "\"type\":\"0x7f\",\"subtype\":\"0x05\""),
                               "an unknown type is its hex value, and so is its subtype");
}

static void test_labels_and_namespaces_are_escaped(void)
{
  _fill();
  strcpy(s_storage.partitions[0].label, "we\"ird\\x\t");
  strcpy(s_storage.namespaces[0].name, "a\"b");
  strcpy(s_storage.nvs_label, "n\\v");
  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"label\":\"we\\\"ird\\\\x\\u0009\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"name\":\"a\\\"b\""));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"nvs\":{\"label\":\"n\\\\v\""));
}

static void test_counts_past_the_arrays_are_clamped(void)
{
  _fill();
  s_storage.partition_count = 255;
  s_storage.namespace_count = 255;
  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_EQUAL_INT(HEAPTOP_STORAGE_MAX_PARTITIONS, _count(s_sink.out, "\"type\":"));
  TEST_ASSERT_EQUAL_INT(HEAPTOP_STORAGE_MAX_NAMESPACES, _count(s_sink.out, "{\"name\":"));
}

/* Every number at its widest, every row used, and labels made only of escaped characters: the largest document, and a
 * check that no piece outgrows the reserve it was given. */
static void test_the_widest_document_fits_the_reserves(void)
{
  s_storage.flash_size = UINT32_MAX;
  s_storage.partitions_truncated = true;
  s_storage.namespaces_truncated = true;
  s_storage.nvs_present = true;
  s_storage.nvs_entry_bytes = UINT32_MAX;
  s_storage.nvs_total = UINT32_MAX;
  s_storage.nvs_used = UINT32_MAX;
  s_storage.nvs_available = UINT32_MAX;
  memset(s_storage.nvs_label, 0x01, sizeof(s_storage.nvs_label) - 1); /* each one escapes to six bytes */
  s_storage.partition_count = HEAPTOP_STORAGE_MAX_PARTITIONS;
  for (int i = 0; i < HEAPTOP_STORAGE_MAX_PARTITIONS; i++)
  {
    heaptop_partition_t *p = &s_storage.partitions[i];
    memset(p->label, 0x01, sizeof(p->label) - 1);
    p->type = 3; /* the longest type name */
    p->subtype = 0x7e;
    p->encrypted = true;
    p->running = false; /* "false" is longer than "true" */
    p->address = UINT32_MAX;
    p->size = UINT32_MAX;
  }
  s_storage.namespace_count = HEAPTOP_STORAGE_MAX_NAMESPACES;
  for (int i = 0; i < HEAPTOP_STORAGE_MAX_NAMESPACES; i++)
  {
    memset(s_storage.namespaces[i].name, 0x01, sizeof(s_storage.namespaces[i].name) - 1);
    s_storage.namespaces[i].entries = UINT32_MAX;
  }
  /* The longest subtype name is "undefined" (data type 1, subtype 6). */
  s_storage.partitions[0].type = 1;
  s_storage.partitions[0].subtype = 0x06;

  TEST_ASSERT_TRUE(_write());
  _assert_json_doc(s_sink.out, s_sink.len);
  TEST_ASSERT_TRUE_MESSAGE(s_sink.chunks > 1, "a document this size comes in several chunks");
  TEST_ASSERT_EQUAL_INT(HEAPTOP_STORAGE_MAX_PARTITIONS, _count(s_sink.out, "\"type\":"));
  TEST_ASSERT_EQUAL_INT(HEAPTOP_STORAGE_MAX_NAMESPACES, _count(s_sink.out, "{\"name\":"));
  TEST_ASSERT_NOT_NULL(strstr(s_sink.out, "\"namespaces_truncated\":true}}"));
  TEST_ASSERT_TRUE(s_sink.max_chunk <= CHUNK_MAX);
}

static void test_a_sink_that_stops_ends_the_document(void)
{
  _fill();
  s_storage.partition_count = HEAPTOP_STORAGE_MAX_PARTITIONS;
  for (int i = 3; i < HEAPTOP_STORAGE_MAX_PARTITIONS; i++) strcpy(s_storage.partitions[i].label, "a_partition");
  s_sink.refuse_at = 2;
  TEST_ASSERT_FALSE(_write());
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_sink.calls, "no call after the sink said stop");
  TEST_ASSERT_EQUAL_INT(1, s_sink.chunks);
}

static void test_null_arguments_fail_without_calling_the_sink(void)
{
  _fill();
  TEST_ASSERT_FALSE(heaptop_json_storage(NULL, _sink, &s_sink));
  TEST_ASSERT_FALSE(heaptop_json_storage(&s_storage, NULL, &s_sink));
  TEST_ASSERT_EQUAL_INT(0, s_sink.calls);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_document_fields);
  RUN_TEST(test_an_unread_chip_and_a_missing_nvs_are_null);
  RUN_TEST(test_encrypted_and_truncation_flags);
  RUN_TEST(test_known_types_and_subtypes_have_names_the_rest_are_hex);
  RUN_TEST(test_labels_and_namespaces_are_escaped);
  RUN_TEST(test_counts_past_the_arrays_are_clamped);
  RUN_TEST(test_the_widest_document_fits_the_reserves);
  RUN_TEST(test_a_sink_that_stops_ends_the_document);
  RUN_TEST(test_null_arguments_fail_without_calling_the_sink);
  return UNITY_END();
}
