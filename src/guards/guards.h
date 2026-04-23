#pragma once

#include <stdint.h>
#include <sys/types.h>
#include "../myfs.h"

// Validate metadata cơ bản của chunk
int guard_chunk_metadata(const myfs_chunk_t *chunk, uint32_t chunk_idx);

// Validate offset logic giữa current offset và chunk
int guard_chunk_logical_offset(off_t cur_offset, off_t chunk_logical_start);

// Validate offset bên trong chunk (sau khi tính chunk_offset)
int guard_chunk_bounds(size_t chunk_offset, size_t chunk_size);

// Validate kết quả đọc từ disk (pread)
int guard_pread_result(ssize_t n, size_t expected);

// Validate allocation
int guard_malloc(void *ptr);

// Validate codec type
int guard_codec_type(int codec_type);

// Validate kết quả decompress
int guard_decompress_size(size_t actual, size_t expected);

// Dọn dẹp tài nguyên (như đóng file descriptor) trước khi trả về lỗi
int cleanup_fd(int fd, int use_fi_fd, int err);