#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cleveldb/options.h"
#include "harness.h"
#include "helpers/memenv/memenv.h"
#include "port/port_compression.h"
#include "status_test_util.h"
#include "table/block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

/* 可配置随机读文件：可把自有存储直接返回，或复制到调用者 scratch；
 * 还可模拟截断和 I/O 错误，避免测试依赖真实文件系统。 */
typedef struct fake_random_file {
  cldb_random_access_file_t base; /* 必须为首成员 */
  const char *storage;            /* 仅引用；文件内容 */
  size_t storage_size;
  bool return_storage;            /* true 时结果指向 storage，否则复制到 scratch */
  bool truncate;                  /* true 时少返回 1 字节 */
  bool fail;                      /* true 时直接返回 I/O 错误 */
} fake_random_file_t;

static cldb_status_t fake_read(cldb_random_access_file_t *file,
                               uint64_t offset, size_t n,
                               cldb_slice_t *result, char *scratch) {
  fake_random_file_t *self = (fake_random_file_t *)file;
  size_t start;
  size_t available;
  size_t read_size;

  if (self->fail) {
    return cldb_status_io("fake.Read", "injected failure");
  }
  if (offset > (uint64_t)self->storage_size) {
    return cldb_status_io("fake.Read", "offset beyond file size");
  }
  start = (size_t)offset;
  available = self->storage_size - start;
  read_size = n < available ? n : available;
  if (self->truncate && read_size > 0) {
    --read_size;
  }
  if (read_size == 0) {
    *result = cldb_slice_empty();
    return cldb_status_ok();
  }
  if (self->return_storage) {
    *result = cldb_slice_make(self->storage + start, read_size);
  } else {
    if (scratch == NULL) {
      *result = cldb_slice_empty();
      return cldb_status_invalid_argument("fake.Read", "scratch is NULL");
    }
    memcpy(scratch, self->storage + start, read_size);
    *result = cldb_slice_make(scratch, read_size);
  }
  return cldb_status_ok();
}

static void fake_destroy(cldb_random_access_file_t *file) {
  (void)file;
}

static const cldb_random_access_file_vtbl_t kFakeVtbl = {
    .read = fake_read,
    .destroy = fake_destroy,
};

static void init_fake(fake_random_file_t *fake, const char *storage,
                      size_t storage_size, bool return_storage) {
  memset(fake, 0, sizeof(*fake));
  fake->base.vtbl = &kFakeVtbl;
  fake->storage = storage;
  fake->storage_size = storage_size;
  fake->return_storage = return_storage;
}

/* 在 offset 处写 raw + type + masked CRC，返回块末尾位置。 */
static size_t write_block(char *dst, size_t offset, const char *raw,
                          size_t raw_size, uint8_t type) {
  uint32_t crc;

  memcpy(dst + offset, raw, raw_size);
  dst[offset + raw_size] = (char)type;
  crc = cldb_crc32c_value(
      dst + offset, raw_size + (size_t)CLDB_BLOCK_TRAILER_TYPE_SIZE);
  cldb_encode_fixed32(
      dst + offset + raw_size + (size_t)CLDB_BLOCK_TRAILER_CRC_OFFSET,
      cldb_crc32c_mask(crc));
  return offset + raw_size + (size_t)CLDB_BLOCK_TRAILER_SIZE;
}

CLDB_TEST(format_block_handle_roundtrip_and_limits) {
  static const cldb_block_handle_t kHandles[] = {
      {UINT64_C(0), UINT64_C(0)},
      {UINT64_C(127), UINT64_C(128)},
      {UINT64_C(12345), UINT64_C(67890)},
      {UINT64_MAX, UINT64_MAX},
  };
  cldb_buf_t encoded;
  size_t i;

  cldb_buf_init(&encoded);
  for (i = 0; i < sizeof(kHandles) / sizeof(kHandles[0]); ++i) {
    cldb_block_handle_t decoded;
    cldb_slice_t input;
    size_t expected_size =
        cldb_varint_length(kHandles[i].offset) +
        cldb_varint_length(kHandles[i].size);

    cldb_buf_clear(&encoded);
    CLDB_ASSERT_TRUE(cldb_block_handle_encode(&kHandles[i], &encoded));
    CLDB_ASSERT_EQ_UINT(expected_size, encoded.size);
    CLDB_ASSERT_TRUE(
        encoded.size <= (size_t)CLDB_BLOCK_HANDLE_MAX_ENCODED_LENGTH);
    input = cldb_buf_slice(&encoded);
    CLDB_ASSERT_OK(cldb_block_handle_decode(&input, &decoded));
    CLDB_EXPECT_EQ_UINT(kHandles[i].offset, decoded.offset);
    CLDB_EXPECT_EQ_UINT(kHandles[i].size, decoded.size);
    CLDB_EXPECT_EQ_UINT(0, input.size);
  }

  /* 最大 varint64 占 10 字节：前 9 字节 0xff，末字节只承载最高位 1。 */
  cldb_buf_clear(&encoded);
  CLDB_ASSERT_TRUE(
      cldb_block_handle_encode(&kHandles[3], &encoded));
  CLDB_ASSERT_EQ_UINT(20, encoded.size);
  for (i = 0; i < encoded.size; ++i) {
    uint8_t expected = i % 10U == 9U ? 0x01 : 0xff;

    CLDB_EXPECT_EQ_UINT(expected, (uint8_t)encoded.data[i]);
  }

  cldb_buf_free(&encoded);
}

CLDB_TEST(format_block_handle_errors_and_prefix) {
  cldb_buf_t buf;
  cldb_block_handle_t handle = {UINT64_C(7), UINT64_C(9)};
  cldb_block_handle_t unchanged;
  cldb_slice_t input;
  cldb_slice_t original_input;
  cldb_status_t status;
  char bad[20];

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_buf_append(&buf, "pre", 3));
  {
    cldb_block_handle_t value = {UINT64_C(1), UINT64_C(2)};

    CLDB_ASSERT_TRUE(cldb_block_handle_encode(&value, &buf));
  }
  CLDB_ASSERT_EQ_UINT(5, buf.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(buf.data, "pre\x01\x02", 5));
  input = cldb_slice_make(buf.data + 3, buf.size - 3);
  CLDB_ASSERT_OK(cldb_block_handle_decode(&input, &handle));
  CLDB_EXPECT_EQ_UINT(1, handle.offset);
  CLDB_EXPECT_EQ_UINT(2, handle.size);
  CLDB_EXPECT_EQ_UINT(0, input.size);
  cldb_buf_free(&buf);

  memset(bad, '\x80', sizeof(bad));
  original_input = cldb_slice_make(bad, sizeof(bad));
  input = original_input;
  handle.offset = 11;
  handle.size = 12;
  unchanged = handle;
  status = cldb_block_handle_decode(&input, &handle);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "bad block handle") !=
                   NULL);
  CLDB_EXPECT_TRUE(cldb_slice_equal(input, original_input));
  CLDB_EXPECT_EQ_UINT(unchanged.offset, handle.offset);
  CLDB_EXPECT_EQ_UINT(unchanged.size, handle.size);

  CLDB_EXPECT_FALSE(cldb_block_handle_encode(NULL, &buf));
  cldb_buf_clear(&buf);
  CLDB_EXPECT_FALSE(cldb_block_handle_encode(&handle, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_handle_decode(NULL, &handle));
  input = original_input;
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_handle_decode(&input, NULL));
}

CLDB_TEST(format_footer_layout_and_roundtrip) {
  static const uint8_t kMagic[CLDB_TABLE_MAGIC_SIZE] = {
      0x57, 0xfb, 0x80, 0x8b, 0x24, 0x75, 0x47, 0xdb};
  cldb_footer_t footer;
  cldb_footer_t decoded;
  cldb_buf_t buf;
  cldb_slice_t input;
  uint8_t expected[CLDB_FOOTER_ENCODED_LENGTH] = {0};
  uint8_t prefix[3] = {'a', 'b', 'c'};

  footer.metaindex_handle.offset = 1;
  footer.metaindex_handle.size = 2;
  footer.index_handle.offset = 3;
  footer.index_handle.size = 4;
  expected[0] = 1;
  expected[1] = 2;
  expected[2] = 3;
  expected[3] = 4;
  memcpy(expected + CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE,
         kMagic, sizeof(kMagic));

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_buf_append(&buf, prefix, sizeof(prefix)));
  CLDB_ASSERT_TRUE(cldb_footer_encode(&footer, &buf));
  CLDB_ASSERT_EQ_UINT(sizeof(prefix) + sizeof(expected), buf.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(buf.data, prefix, sizeof(prefix)));
  CLDB_EXPECT_EQ_INT(
      0, memcmp(buf.data + sizeof(prefix), expected, sizeof(expected)));

  input = cldb_slice_make(buf.data + sizeof(prefix), sizeof(expected));
  CLDB_ASSERT_OK(cldb_footer_decode(&input, &decoded));
  CLDB_EXPECT_EQ_UINT(footer.metaindex_handle.offset,
                      decoded.metaindex_handle.offset);
  CLDB_EXPECT_EQ_UINT(footer.metaindex_handle.size,
                      decoded.metaindex_handle.size);
  CLDB_EXPECT_EQ_UINT(footer.index_handle.offset,
                      decoded.index_handle.offset);
  CLDB_EXPECT_EQ_UINT(footer.index_handle.size, decoded.index_handle.size);
  CLDB_EXPECT_EQ_UINT(0, input.size);
  cldb_buf_free(&buf);
}

CLDB_TEST(format_footer_max_handles_fill_padding) {
  cldb_footer_t footer;
  cldb_footer_t decoded;
  cldb_buf_t buf;
  cldb_slice_t input;
  size_t i;

  footer.metaindex_handle.offset = UINT64_MAX;
  footer.metaindex_handle.size = UINT64_MAX;
  footer.index_handle.offset = UINT64_MAX;
  footer.index_handle.size = UINT64_MAX;
  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_footer_encode(&footer, &buf));
  CLDB_ASSERT_EQ_UINT(CLDB_FOOTER_ENCODED_LENGTH, buf.size);
  for (i = 0; i < 2 * (size_t)CLDB_BLOCK_HANDLE_MAX_ENCODED_LENGTH; ++i) {
    uint8_t expected = i % 10U == 9U ? 0x01 : 0xff;

    CLDB_EXPECT_EQ_UINT(expected, (uint8_t)buf.data[i]);
  }
  CLDB_EXPECT_EQ_UINT(
      0x57, (uint8_t)buf.data[CLDB_FOOTER_ENCODED_LENGTH -
                              CLDB_TABLE_MAGIC_SIZE]);

  input = cldb_buf_slice(&buf);
  CLDB_ASSERT_OK(cldb_footer_decode(&input, &decoded));
  CLDB_EXPECT_EQ_UINT(UINT64_MAX, decoded.metaindex_handle.offset);
  CLDB_EXPECT_EQ_UINT(UINT64_MAX, decoded.metaindex_handle.size);
  CLDB_EXPECT_EQ_UINT(UINT64_MAX, decoded.index_handle.offset);
  CLDB_EXPECT_EQ_UINT(UINT64_MAX, decoded.index_handle.size);
  CLDB_EXPECT_EQ_UINT(0, input.size);
  cldb_buf_free(&buf);
}

CLDB_TEST(format_footer_decode_errors_and_trailing_data) {
  cldb_footer_t footer;
  cldb_footer_t unchanged;
  cldb_footer_t decoded;
  cldb_buf_t encoded;
  cldb_slice_t input;
  cldb_slice_t original;
  cldb_status_t status;
  uint8_t trailing[CLDB_FOOTER_ENCODED_LENGTH + 3];
  uint8_t bad_handle[CLDB_FOOTER_ENCODED_LENGTH];

  footer.metaindex_handle.offset = 5;
  footer.metaindex_handle.size = 6;
  footer.index_handle.offset = 7;
  footer.index_handle.size = 8;
  cldb_buf_init(&encoded);
  CLDB_ASSERT_TRUE(cldb_footer_encode(&footer, &encoded));

  unchanged.metaindex_handle.offset = 11;
  unchanged.metaindex_handle.size = 12;
  unchanged.index_handle.offset = 13;
  unchanged.index_handle.size = 14;

  input = cldb_slice_make(encoded.data, encoded.size - 1);
  original = input;
  status = cldb_footer_decode(&input, &unchanged);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "footer too short") !=
                   NULL);
  CLDB_EXPECT_TRUE(cldb_slice_equal(input, original));

  input = cldb_buf_slice(&encoded);
  original = input;
  encoded.data[CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE] =
      (char)(encoded.data[CLDB_FOOTER_ENCODED_LENGTH -
                          CLDB_TABLE_MAGIC_SIZE] ^ 1);
  status = cldb_footer_decode(&input, &unchanged);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "bad magic number") !=
                   NULL);
  CLDB_EXPECT_TRUE(cldb_slice_equal(input, original));

  encoded.data[CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE] =
      (char)(encoded.data[CLDB_FOOTER_ENCODED_LENGTH -
                          CLDB_TABLE_MAGIC_SIZE] ^ 1);
  memcpy(bad_handle, encoded.data, sizeof(bad_handle));
  memset(bad_handle, 0x80,
         (size_t)(CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE));
  input = cldb_slice_make((const char *)bad_handle, sizeof(bad_handle));
  original = input;
  status = cldb_footer_decode(&input, &unchanged);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "bad block handle") !=
                   NULL);
  CLDB_EXPECT_TRUE(cldb_slice_equal(input, original));

  CLDB_EXPECT_EQ_UINT(11, unchanged.metaindex_handle.offset);
  CLDB_EXPECT_EQ_UINT(12, unchanged.metaindex_handle.size);
  CLDB_EXPECT_EQ_UINT(13, unchanged.index_handle.offset);
  CLDB_EXPECT_EQ_UINT(14, unchanged.index_handle.size);

  memcpy(trailing, encoded.data, CLDB_FOOTER_ENCODED_LENGTH);
  memcpy(trailing + CLDB_FOOTER_ENCODED_LENGTH, "xyz", 3);
  input = cldb_slice_make((const char *)trailing, sizeof(trailing));
  CLDB_ASSERT_OK(cldb_footer_decode(&input, &decoded));
  CLDB_EXPECT_EQ_UINT(footer.metaindex_handle.offset,
                      decoded.metaindex_handle.offset);
  CLDB_EXPECT_EQ_PTR(trailing + CLDB_FOOTER_ENCODED_LENGTH, input.data);
  CLDB_EXPECT_EQ_UINT(3, input.size);

  cldb_buf_free(&encoded);
  CLDB_EXPECT_FALSE(cldb_footer_encode(NULL, &encoded));
  input = cldb_slice_empty();
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_footer_decode(&input, NULL));
}

CLDB_TEST(format_read_block_uncompressed_scratch_ownership) {
  static const char kRaw[] = "plain-block";
  char file[32];
  cldb_read_options_t options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents;
  cldb_block_t block;
  fake_random_file_t fake;
  size_t end;

  end = write_block(file, 0, kRaw, strlen(kRaw), CLDB_COMPRESSION_NONE);
  init_fake(&fake, file, end, false);
  cldb_read_options_init(&options);
  handle.offset = 0;
  handle.size = strlen(kRaw);

  CLDB_ASSERT_OK(cldb_read_block(&fake.base, &options, handle, &contents));
  CLDB_EXPECT_EQ_UINT(strlen(kRaw), contents.data.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(contents.data.data, kRaw, strlen(kRaw)));
  CLDB_EXPECT_TRUE(contents.cachable);
  CLDB_EXPECT_TRUE(contents.heap_allocated);
  CLDB_EXPECT_TRUE(contents.data.data != file);

  /* 交给 Block 后由 Block 负责 free。 */
  CLDB_ASSERT_OK(cldb_block_init(&block, contents));
  cldb_block_destroy(&block);
}

CLDB_TEST(format_read_block_mem_env) {
  static const char kRaw[] = "env-block";
  cldb_env_t *env;
  cldb_random_access_file_t *random;
  cldb_read_options_t options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents;
  cldb_block_t block;
  char file[64];
  size_t end;
  size_t prefix = 4;

  env = cldb_memenv_create(NULL);
  CLDB_ASSERT_TRUE(env != NULL);
  memset(file, '.', prefix);
  end = write_block(file, prefix, kRaw, strlen(kRaw), CLDB_COMPRESSION_NONE);
  CLDB_ASSERT_OK(cldb_write_string_to_file(
      env, cldb_slice_make(file, end), "/table"));
  CLDB_ASSERT_OK(cldb_env_new_random_access_file(env, "/table", &random));
  cldb_read_options_init(&options);
  handle.offset = prefix;
  handle.size = strlen(kRaw);

  CLDB_ASSERT_OK(cldb_read_block(random, &options, handle, &contents));
  CLDB_EXPECT_EQ_UINT(strlen(kRaw), contents.data.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(contents.data.data, kRaw, strlen(kRaw)));
  CLDB_EXPECT_TRUE(contents.cachable);
  CLDB_EXPECT_TRUE(contents.heap_allocated);
  CLDB_ASSERT_OK(cldb_block_init(&block, contents));
  cldb_block_destroy(&block);

  cldb_random_access_file_destroy(random);
  cldb_env_destroy(env);
}

CLDB_TEST(format_read_block_borrowed_storage_at_offset) {
  static const char kRaw[] = "borrowed-block";
  char file[64];
  cldb_read_options_t options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents;
  fake_random_file_t fake;
  size_t end;
  size_t prefix = 7;

  memset(file, 'x', prefix);
  end = write_block(file, prefix, kRaw, strlen(kRaw), CLDB_COMPRESSION_NONE);
  init_fake(&fake, file, end, true);
  cldb_read_options_init(&options);
  handle.offset = prefix;
  handle.size = strlen(kRaw);

  CLDB_ASSERT_OK(cldb_read_block(&fake.base, &options, handle, &contents));
  CLDB_EXPECT_EQ_PTR(file + prefix, contents.data.data);
  CLDB_EXPECT_EQ_UINT(strlen(kRaw), contents.data.size);
  CLDB_EXPECT_FALSE(contents.cachable);
  CLDB_EXPECT_FALSE(contents.heap_allocated);
}

CLDB_TEST(format_read_block_truncated_and_io_error) {
  static const char kRaw[] = "truncated-block";
  char file[64];
  cldb_read_options_t options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents = {{"sentinel", 8}, true, true};
  fake_random_file_t fake;
  cldb_status_t status;
  size_t end;

  end = write_block(file, 0, kRaw, strlen(kRaw), CLDB_COMPRESSION_NONE);
  init_fake(&fake, file, end, false);
  cldb_read_options_init(&options);
  handle.offset = 0;
  handle.size = strlen(kRaw);

  fake.truncate = true;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "truncated block read") !=
                   NULL);
  CLDB_EXPECT_FALSE(contents.cachable);
  CLDB_EXPECT_FALSE(contents.heap_allocated);
  CLDB_EXPECT_EQ_UINT(0, contents.data.size);

  fake.truncate = false;
  fake.fail = true;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "injected failure") !=
                   NULL);
  CLDB_EXPECT_FALSE(contents.cachable);
  CLDB_EXPECT_FALSE(contents.heap_allocated);
}

CLDB_TEST(format_read_block_crc_and_type) {
  static const char kRaw[] = "checksum-block";
  char file[64];
  cldb_read_options_t options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents;
  fake_random_file_t fake;
  cldb_status_t status;
  size_t end;
  size_t crc_pos;

  end = write_block(file, 0, kRaw, strlen(kRaw), CLDB_COMPRESSION_NONE);
  init_fake(&fake, file, end, true); /* 借用返回，后续读取无需释放 */
  cldb_read_options_init(&options);
  handle.offset = 0;
  handle.size = strlen(kRaw);
  crc_pos = strlen(kRaw) + (size_t)CLDB_BLOCK_TRAILER_CRC_OFFSET;

  file[crc_pos] = (char)(file[crc_pos] ^ 1); /* 默认不校验，应忽略 CRC */
  CLDB_ASSERT_OK(cldb_read_block(&fake.base, &options, handle, &contents));
  CLDB_EXPECT_EQ_UINT(strlen(kRaw), contents.data.size);
  CLDB_EXPECT_FALSE(contents.cachable);

  options.verify_checksums = true;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status),
                          "block checksum mismatch") != NULL);
  CLDB_EXPECT_FALSE(contents.heap_allocated);

  /* 恢复 CRC，只改 type：CRC 覆盖 type，校验应先报错。 */
  file[crc_pos] = (char)(file[crc_pos] ^ 1);
  file[strlen(kRaw) + (size_t)CLDB_BLOCK_TRAILER_TYPE_OFFSET] =
      (char)CLDB_COMPRESSION_SNAPPY;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status),
                          "block checksum mismatch") != NULL);

  options.verify_checksums = false;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status),
                          "snappy compressed block length") != NULL);
  CLDB_EXPECT_FALSE(contents.cachable);
  CLDB_EXPECT_FALSE(contents.heap_allocated);
}

CLDB_TEST(format_read_block_compression_and_unknown_type) {
  static const char kRaw[] = "compressed-placeholder";
  char file[64];
  cldb_read_options_t options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents;
  fake_random_file_t fake;
  cldb_status_t status;
  size_t end;

  cldb_read_options_init(&options);
  options.verify_checksums = true;
  init_fake(&fake, file, 64, true);
  handle.offset = 0;
  handle.size = strlen(kRaw);

  end = write_block(file, 0, kRaw, strlen(kRaw),
                    (uint8_t)CLDB_COMPRESSION_SNAPPY);
  fake.storage_size = end;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status),
                          "corrupted snappy compressed block length") != NULL);

  end = write_block(file, 0, kRaw, strlen(kRaw),
                    (uint8_t)CLDB_COMPRESSION_ZSTD);
  fake.storage_size = end;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status),
                          "corrupted zstd compressed block length") != NULL);

  end = write_block(file, 0, kRaw, strlen(kRaw), UINT8_C(99));
  fake.storage_size = end;
  status = cldb_read_block(&fake.base, &options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "bad block type") !=
                   NULL);
  CLDB_EXPECT_EQ_UINT(0, contents.data.size);
  CLDB_EXPECT_FALSE(contents.heap_allocated);
}

CLDB_TEST(format_read_block_invalid_arguments_and_overflow) {
  static const char kRaw[] = "argument-block";
  char file[64];
  cldb_read_options_t read_options;
  cldb_block_handle_t handle;
  cldb_block_contents_t contents = {{"sentinel", 8}, true, true};
  fake_random_file_t fake;
  cldb_status_t status;
  size_t end;

  end = write_block(file, 0, kRaw, strlen(kRaw), CLDB_COMPRESSION_NONE);
  init_fake(&fake, file, end, false);
  cldb_read_options_init(&read_options);
  handle.offset = 0;
  handle.size = strlen(kRaw);

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_read_block(&fake.base, &read_options, handle, NULL));

  status = cldb_read_block(NULL, &read_options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);
  CLDB_EXPECT_EQ_UINT(0, contents.data.size);
  CLDB_EXPECT_FALSE(contents.cachable);
  CLDB_EXPECT_FALSE(contents.heap_allocated);

  status = cldb_read_block(&fake.base, NULL, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);

  handle.size = UINT64_MAX;
  status = cldb_read_block(&fake.base, &read_options, handle, &contents);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "bad block handle") !=
                   NULL);
  CLDB_EXPECT_FALSE(contents.heap_allocated);
}

CLDB_TEST(format_compression_hooks_unavailable) {
  cldb_buf_t output;
  size_t length = 123;
  char buffer[8] = {0};

  cldb_buf_init(&output);
  CLDB_EXPECT_FALSE(cldb_port_snappy_get_uncompressed_length("x", 1, &length));
  CLDB_EXPECT_EQ_UINT(123, length);
  CLDB_EXPECT_FALSE(cldb_port_snappy_uncompress("x", 1, buffer,
                                                sizeof(buffer)));
  CLDB_EXPECT_FALSE(cldb_port_snappy_compress("x", 1, &output));
  CLDB_EXPECT_EQ_UINT(0, output.size);

  CLDB_EXPECT_FALSE(cldb_port_zstd_get_uncompressed_length("x", 1, &length));
  CLDB_EXPECT_EQ_UINT(123, length);
  CLDB_EXPECT_FALSE(cldb_port_zstd_uncompress("x", 1, buffer, sizeof(buffer)));
  CLDB_EXPECT_FALSE(cldb_port_zstd_compress(1, "x", 1, &output));
  CLDB_EXPECT_EQ_UINT(0, output.size);
  cldb_buf_free(&output);
}

void cldb_register_format_tests(void) {
  CLDB_RUN_TEST(format_block_handle_roundtrip_and_limits);
  CLDB_RUN_TEST(format_block_handle_errors_and_prefix);
  CLDB_RUN_TEST(format_footer_layout_and_roundtrip);
  CLDB_RUN_TEST(format_footer_max_handles_fill_padding);
  CLDB_RUN_TEST(format_footer_decode_errors_and_trailing_data);
  CLDB_RUN_TEST(format_read_block_uncompressed_scratch_ownership);
  CLDB_RUN_TEST(format_read_block_mem_env);
  CLDB_RUN_TEST(format_read_block_borrowed_storage_at_offset);
  CLDB_RUN_TEST(format_read_block_truncated_and_io_error);
  CLDB_RUN_TEST(format_read_block_crc_and_type);
  CLDB_RUN_TEST(format_read_block_compression_and_unknown_type);
  CLDB_RUN_TEST(format_read_block_invalid_arguments_and_overflow);
  CLDB_RUN_TEST(format_compression_hooks_unavailable);
}
