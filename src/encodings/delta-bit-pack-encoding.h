// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PARQUET_DELTA_BIT_PACK_ENCODING_H
#define PARQUET_DELTA_BIT_PACK_ENCODING_H

#include "encodings.h"

namespace parquet_cpp {

template<int num_values, int bit_width>
void DecodeMiniBlock(uint8_t* data) {
}

class DeltaBitPackDecoder : public Decoder {
 public:
  DeltaBitPackDecoder(const parquet::Type::type& type)
    : Decoder(type, parquet::Encoding::DELTA_BINARY_PACKED) {
    if (type != parquet::Type::INT32 && type != parquet::Type::INT64) {
      throw ParquetException("Delta bit pack encoding should only be for integer data.");
    }
  }

  virtual void SetData(int num_values, const uint8_t* data, int len) {
    num_values_ = num_values;
    decoder_ = impala::BitReader(data, len);
    values_current_block_ = 0;
    values_current_mini_block_ = 0;
  }

  virtual int GetInt32(int32_t* buffer, int max_values) {
    return GetInternal(buffer, max_values);
  }

  virtual int GetInt64(int64_t* buffer, int max_values) {
    return GetInternal(buffer, max_values);
  }

  static void Init() {
    Init(decode_data_14_, 14);
    Init(decode_data_15_, 15);
  }

 private:
  void InitBlock() {
    uint64_t block_size;
    if (!decoder_.GetVlqInt(&block_size)) ParquetException::EofException();
    if (!decoder_.GetVlqInt(&num_mini_blocks_)) ParquetException::EofException();
    if (!decoder_.GetVlqInt(&values_current_block_)) {
      ParquetException::EofException();
    }
    if (!decoder_.GetZigZagVlqInt(&last_value_)) ParquetException::EofException();
    delta_bit_widths_.resize(num_mini_blocks_);

    if (!decoder_.GetZigZagVlqInt(&min_delta_)) ParquetException::EofException();
    for (int i = 0; i < num_mini_blocks_; ++i) {
      if (!decoder_.GetAligned<uint8_t>(1, &delta_bit_widths_[i])) {
        ParquetException::EofException();
      }
    }
    values_per_mini_block_ = block_size / num_mini_blocks_;
    mini_block_idx_ = 0;
    delta_bit_width_ = delta_bit_widths_[0];
    values_current_mini_block_ = values_per_mini_block_;
  }

  template <typename T>
  int GetInternal(T* buffer, int max_values) {
    max_values = std::min(max_values, num_values_);
    for (int i = 0; i < max_values; ++i) {
      if (UNLIKELY(values_current_mini_block_ == 0)) {
        ++mini_block_idx_;
        if (mini_block_idx_ < delta_bit_widths_.size()) {
          delta_bit_width_ = delta_bit_widths_[mini_block_idx_];
          values_current_mini_block_ = values_per_mini_block_;
        } else {
          InitBlock();
          buffer[i] = last_value_;
          continue;
        }
      }

      if (max_values == 32) {
        if (delta_bit_width_ == 14) {
          const uint8_t* data = decoder_.current_ptr();
          DecodeBlock<T>(decode_data_14_, data, buffer);
        } else if (delta_bit_width_ == 15) {
          const uint8_t* data = decoder_.current_ptr();
          DecodeBlock<T>(decode_data_15_, data, buffer);
        } else {
          // TODO: the key to this algorithm is to decode the entire miniblock at once.
          ParquetException::NYI("miniblocks");
        }
        for (int i = 0; i < 32; ++i) {
          buffer[i] = last_value_ + min_delta_ + buffer[i];
          last_value_ = buffer[i];
        }
        decoder_.SkipBytes(32 * delta_bit_width_ / 8);
        values_current_mini_block_ = 0;
        num_values_ -= 32;
        return 32;
      }
      int64_t delta;
      if (!decoder_.GetValue(delta_bit_width_, &delta)) ParquetException::EofException();
      delta += min_delta_;
      last_value_ += delta;
      buffer[i] = last_value_;
      --values_current_mini_block_;
    }
    num_values_ -= max_values;
    return max_values;
  }

  impala::BitReader decoder_;
  uint64_t values_current_block_;
  uint64_t num_mini_blocks_;
  int values_per_mini_block_;
  int values_current_mini_block_;

  int64_t min_delta_;
  int mini_block_idx_;
  std::vector<uint8_t> delta_bit_widths_;
  int delta_bit_width_;

  int64_t last_value_;

  struct DecodeData {
    int byte_offset;
    int shift;
  };

  static DecodeData decode_data_14_[32];
  static DecodeData decode_data_15_[32];

  template <typename T>
  void DecodeBlock(const DecodeData* offsets, const uint8_t* data, T* buffer) {
    uint64_t mask = -1;
    mask = mask >> (64 - delta_bit_width_);
    for (int i = 0; i < 32; i += 4) {
      buffer[i + 0] = *reinterpret_cast<const T*>(data + offsets[i + 0].byte_offset);
      buffer[i + 1] = *reinterpret_cast<const T*>(data + offsets[i + 1].byte_offset);
      buffer[i + 2] = *reinterpret_cast<const T*>(data + offsets[i + 2].byte_offset);
      buffer[i + 3] = *reinterpret_cast<const T*>(data + offsets[i + 3].byte_offset);

      buffer[i + 0] >>= offsets[i + 0].shift;
      buffer[i + 1] >>= offsets[i + 1].shift;
      buffer[i + 2] >>= offsets[i + 2].shift;
      buffer[i + 3] >>= offsets[i + 3].shift;

      buffer[i + 0] &= mask;
      buffer[i + 1] &= mask;
      buffer[i + 2] &= mask;
      buffer[i + 3] &= mask;
    }
  }

  static void Init(DecodeData* data, int bit_width) {
    int bit_offset = 0;
    for (int i = 0; i < 32; ++i) {
      data[i].byte_offset = bit_offset / 8;
      data[i].shift = bit_offset % 8;
      bit_offset += bit_width;
    }
  }
};

}

#endif

