/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef _THRIFT_PROTOCOL_TCOMPACTPROTOCOL_H_
#define _THRIFT_PROTOCOL_TCOMPACTPROTOCOL_H_ 1

#include <thrift/protocol/TProtocol.h>

#include <stack>
#include <memory>

namespace apache {
namespace thrift {
namespace protocol {

/**
 * C++ Implementation of the Compact Protocol as described in THRIFT-110
 */
template <class Transport_>
class TCompactProtocolT {
public:
  static const int8_t PROTOCOL_ID = (int8_t)0x82u;
  static const int8_t VERSION_N = 1;
  static const int8_t VERSION_MASK = 0x1f;       // 0001 1111

protected:
  static const int8_t TYPE_MASK = (int8_t)0xE0u; // 1110 0000
  static const int8_t TYPE_BITS = 0x07;          // 0000 0111
  static const int32_t TYPE_SHIFT_AMOUNT = 5;

  Transport_* trans_;

  /**
   * (Writing) If we encounter a boolean field begin, save the TField here
   * so it can have the value incorporated.
   */
  struct {
    const char* name;
    TType fieldType;
    int16_t fieldId;
  } booleanField_;

  /**
   * (Reading) If we read a field header, and it's a boolean field, save
   * the boolean value here so that readBool can use it.
   */
  struct {
    bool hasBoolValue;
    bool boolValue;
  } boolValue_;

  /**
   * Used to keep track of the last field for the current and previous structs,
   * so we can do the delta stuff.
   */

  std::stack<int16_t> lastField_;
  int16_t lastFieldId_;

  // Recursion-depth guard against stack overflow on deeply nested input; see
  // TInputRecursionTracker in TProtocol.h. Input and output depths are tracked
  // separately so one protocol can be used for both. The limit comes from the
  // transport's TConfiguration (default DEFAULT_RECURSION_DEPTH = 64).
  uint32_t input_recursion_depth_;
  uint32_t output_recursion_depth_;
  uint32_t recursion_limit_;

public:
  TCompactProtocolT(std::shared_ptr<Transport_> trans)
    : trans_(trans.get()),
      lastFieldId_(0),
      input_recursion_depth_(0),
      output_recursion_depth_(0),
      recursion_limit_(trans->getConfiguration()->getRecursionLimit()),
      string_limit_(0),
      string_buf_(nullptr),
      string_buf_size_(0),
      container_limit_(0) {
    booleanField_.name = nullptr;
    boolValue_.hasBoolValue = false;
  }

  TCompactProtocolT(std::shared_ptr<Transport_> trans,
                    int32_t string_limit,
                    int32_t container_limit)
    : trans_(trans.get()),
      lastFieldId_(0),
      input_recursion_depth_(0),
      output_recursion_depth_(0),
      recursion_limit_(trans->getConfiguration()->getRecursionLimit()),
      string_limit_(string_limit),
      string_buf_(nullptr),
      string_buf_size_(0),
      container_limit_(container_limit) {
    booleanField_.name = nullptr;
    boolValue_.hasBoolValue = false;
  }

  ~TCompactProtocolT() { free(string_buf_); }

  // Recursion-depth tracking, driven by TInputRecursionTracker /
  // TOutputRecursionTracker (TProtocol.h). Throws once the configured limit is
  // exceeded, before the C++ stack overflows.
  void incrementInputRecursionDepth() {
    if (recursion_limit_ < ++input_recursion_depth_) {
      throw TProtocolException(TProtocolException::DEPTH_LIMIT);
    }
  }
  void decrementInputRecursionDepth() { --input_recursion_depth_; }

  void incrementOutputRecursionDepth() {
    if (recursion_limit_ < ++output_recursion_depth_) {
      throw TProtocolException(TProtocolException::DEPTH_LIMIT);
    }
  }
  void decrementOutputRecursionDepth() { --output_recursion_depth_; }

  /**
   * Writing functions
   */

  uint32_t writeMessageBegin(const std::string& name,
                                     const TMessageType messageType,
                                     const int32_t seqid);

  uint32_t writeStructBegin(const char* name);

  uint32_t writeStructEnd();

  uint32_t writeFieldBegin(const char* name, const TType fieldType, const int16_t fieldId);

  uint32_t writeFieldStop();

  uint32_t writeListBegin(const TType elemType, const uint32_t size);

  uint32_t writeSetBegin(const TType elemType, const uint32_t size);

  uint32_t writeMapBegin(const TType keyType, const TType valType, const uint32_t size);

  uint32_t writeBool(const bool value);

  uint32_t writeByte(const int8_t byte);

  uint32_t writeI16(const int16_t i16);

  uint32_t writeI32(const int32_t i32);

  uint32_t writeI64(const int64_t i64);

  uint32_t writeDouble(const double dub);

  uint32_t writeString(const std::string& str);

  uint32_t writeBinary(const std::string& str);

  int getMinSerializedSize(TType type);

  void checkReadBytesAvailable(TSet& set)
  {
      trans_->checkReadBytesAvailable(set.size_ * getMinSerializedSize(set.elemType_));
  }

  void checkReadBytesAvailable(TList& list)
  {
      trans_->checkReadBytesAvailable(list.size_ * getMinSerializedSize(list.elemType_));
  }

  void checkReadBytesAvailable(TMap& map)
  {
      int elmSize = getMinSerializedSize(map.keyType_) + getMinSerializedSize(map.valueType_);
      trans_->checkReadBytesAvailable(map.size_ * elmSize);
  }

  /**
  * These methods are called by structs, but don't actually have any wired
  * output or purpose
  */
  uint32_t writeMessageEnd() { return 0; }
  uint32_t writeMapEnd() { return 0; }
  uint32_t writeListEnd() { return 0; }
  uint32_t writeSetEnd() { return 0; }
  uint32_t writeFieldEnd() { return 0; }

protected:
  int32_t writeFieldBeginInternal(const char* name,
                                  const TType fieldType,
                                  const int16_t fieldId,
                                  int8_t typeOverride);
  uint32_t writeCollectionBegin(const TType elemType, int32_t size);
  uint32_t writeVarint32(uint32_t n);
  uint32_t writeVarint64(uint64_t n);
  uint64_t i64ToZigzag(const int64_t l);
  uint32_t i32ToZigzag(const int32_t n);
  inline int8_t getCompactType(const TType ttype);

public:
  uint32_t readMessageBegin(std::string& name, TMessageType& messageType, int32_t& seqid);

  uint32_t readStructBegin(std::string& name);

  uint32_t readStructEnd();

  uint32_t readFieldBegin(std::string& name, TType& fieldType, int16_t& fieldId);

  uint32_t readMapBegin(TType& keyType, TType& valType, uint32_t& size);

  uint32_t readListBegin(TType& elemType, uint32_t& size);

  uint32_t readSetBegin(TType& elemType, uint32_t& size);

  uint32_t readBool(bool& value);

  uint32_t readBool(std::vector<bool>::reference value) {
    bool b = false;
    uint32_t ret = readBool(b);
    value = b;
    return ret;
  }

  uint32_t readByte(int8_t& byte);

  uint32_t readI16(int16_t& i16);

  uint32_t readI32(int32_t& i32);

  uint32_t readI64(int64_t& i64);

  uint32_t readDouble(double& dub);

  uint32_t readString(std::string& str);
  // Firebolt addition: read string as a std::string_view instead of a std::string.
  // Caller needs to be aware that the string_view is only valid for the lifetime of the protocol.
  uint32_t readStringView(std::string_view& str);

  uint32_t readBinary(std::string& str);

  uint32_t skip(TType type) { return ::apache::thrift::protocol::skip(*this, type); }

  /*
   *These methods are here for the struct to call, but don't have any wire
   * encoding.
   */
  uint32_t readMessageEnd() { return 0; }
  uint32_t readFieldEnd() { return 0; }
  uint32_t readMapEnd() { return 0; }
  uint32_t readListEnd() { return 0; }
  uint32_t readSetEnd() { return 0; }

protected:
  uint32_t readVarint32(int32_t& i32);
  uint32_t readVarint64(int64_t& i64);
  int32_t zigzagToI32(uint32_t n);
  int64_t zigzagToI64(uint64_t n);
  TType getTType(int8_t type);

  // Buffer for reading strings, save for the lifetime of the protocol to
  // avoid memory churn allocating memory on every string read
  int32_t string_limit_;
  uint8_t* string_buf_;
  int32_t string_buf_size_;
  int32_t container_limit_;
};

}
}
} // apache::thrift::protocol

#include <thrift/protocol/TCompactProtocol.tcc>

#endif
