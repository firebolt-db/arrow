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

#ifndef _THRIFT_PROTOCOL_TPROTOCOL_H_
#define _THRIFT_PROTOCOL_TPROTOCOL_H_ 1

#ifdef _WIN32
// Need to come before any Windows.h includes
#include <Winsock2.h>
#endif

#include <thrift/transport/TTransport.h>
#include <thrift/protocol/TProtocolException.h>
#include <thrift/protocol/TEnum.h>
#include <thrift/protocol/TList.h>
#include <thrift/protocol/TSet.h>
#include <thrift/protocol/TMap.h>

#include <memory>

#include <sys/types.h>
#include <string>
#include <map>
#include <vector>
#include <climits>

// Use this to get around strict aliasing rules.
// For example, uint64_t i = bitwise_cast<uint64_t>(returns_double());
// The most obvious implementation is to just cast a pointer,
// but that doesn't work.
// For a pretty in-depth explanation of the problem, see
// http://cellperformance.beyond3d.com/articles/2006/06/understanding-strict-aliasing.html
template <typename To, typename From>
static inline To bitwise_cast(From from) {
  static_assert(sizeof(From) == sizeof(To), "sizeof(From) == sizeof(To)");

  // BAD!!!  These are all broken with -O2.
  //return *reinterpret_cast<To*>(&from);  // BAD!!!
  //return *static_cast<To*>(static_cast<void*>(&from));  // BAD!!!
  //return *(To*)(void*)&from;  // BAD!!!

  // Super clean and paritally blessed by section 3.9 of the standard.
  //unsigned char c[sizeof(from)];
  //memcpy(c, &from, sizeof(from));
  //To to;
  //memcpy(&to, c, sizeof(c));
  //return to;

  // Slightly more questionable.
  // Same code emitted by GCC.
  //To to;
  //memcpy(&to, &from, sizeof(from));
  //return to;

  // Technically undefined, but almost universally supported,
  // and the most efficient implementation.
  union {
    From f;
    To t;
  } u;
  u.f = from;
  return u.t;
}


#ifndef __THRIFT_BYTE_ORDER
# if defined(BYTE_ORDER) && defined(LITTLE_ENDIAN) && defined(BIG_ENDIAN)
#  define __THRIFT_BYTE_ORDER BYTE_ORDER
#  define __THRIFT_LITTLE_ENDIAN LITTLE_ENDIAN
#  define __THRIFT_BIG_ENDIAN BIG_ENDIAN
# else
#  include <boost/predef/other/endian.h>
#  if BOOST_ENDIAN_BIG_BYTE
#    define __THRIFT_BYTE_ORDER 4321
#    define __THRIFT_LITTLE_ENDIAN 0
#    define __THRIFT_BIG_ENDIAN __THRIFT_BYTE_ORDER
#  elif BOOST_ENDIAN_LITTLE_BYTE
#    define __THRIFT_BYTE_ORDER 1234
#    define __THRIFT_LITTLE_ENDIAN __THRIFT_BYTE_ORDER
#    define __THRIFT_BIG_ENDIAN 0
#  endif
#  ifdef BOOST_LITTLE_ENDIAN
#  else
#  endif
# endif
#endif

#if __THRIFT_BYTE_ORDER == __THRIFT_BIG_ENDIAN
# if !defined(THRIFT_ntohll)
#  define THRIFT_ntohll(n) (n)
#  define THRIFT_htonll(n) (n)
# endif
# if defined(__GNUC__) && defined(__GLIBC__)
#  include <byteswap.h>
#  define THRIFT_htolell(n) bswap_64(n)
#  define THRIFT_letohll(n) bswap_64(n)
#  define THRIFT_htolel(n) bswap_32(n)
#  define THRIFT_letohl(n) bswap_32(n)
#  define THRIFT_htoles(n) bswap_16(n)
#  define THRIFT_letohs(n) bswap_16(n)
# else /* GNUC & GLIBC */
#  define bswap_64(n) \
      ( (((n) & 0xff00000000000000ull) >> 56) \
      | (((n) & 0x00ff000000000000ull) >> 40) \
      | (((n) & 0x0000ff0000000000ull) >> 24) \
      | (((n) & 0x000000ff00000000ull) >> 8)  \
      | (((n) & 0x00000000ff000000ull) << 8)  \
      | (((n) & 0x0000000000ff0000ull) << 24) \
      | (((n) & 0x000000000000ff00ull) << 40) \
      | (((n) & 0x00000000000000ffull) << 56) )
#  define bswap_32(n) \
      ( (((n) & 0xff000000ul) >> 24) \
      | (((n) & 0x00ff0000ul) >> 8)  \
      | (((n) & 0x0000ff00ul) << 8)  \
      | (((n) & 0x000000fful) << 24) )
#  define bswap_16(n) \
      ( (((n) & ((unsigned short)0xff00ul)) >> 8)  \
      | (((n) & ((unsigned short)0x00fful)) << 8)  )
#  define THRIFT_htolell(n) bswap_64(n)
#  define THRIFT_letohll(n) bswap_64(n)
#  define THRIFT_htolel(n) bswap_32(n)
#  define THRIFT_letohl(n) bswap_32(n)
#  define THRIFT_htoles(n) bswap_16(n)
#  define THRIFT_letohs(n) bswap_16(n)
# endif /* GNUC & GLIBC */
#elif __THRIFT_BYTE_ORDER == __THRIFT_LITTLE_ENDIAN
#  define THRIFT_htolell(n) (n)
#  define THRIFT_letohll(n) (n)
#  define THRIFT_htolel(n) (n)
#  define THRIFT_letohl(n) (n)
#  define THRIFT_htoles(n) (n)
#  define THRIFT_letohs(n) (n)
# if defined(__GNUC__) && defined(__GLIBC__)
#  include <byteswap.h>
#  define THRIFT_ntohll(n) bswap_64(n)
#  define THRIFT_htonll(n) bswap_64(n)
# elif defined(_MSC_VER) /* Microsoft Visual C++ */
#  define THRIFT_ntohll(n) ( _byteswap_uint64((uint64_t)n) )
#  define THRIFT_htonll(n) ( _byteswap_uint64((uint64_t)n) )
# elif !defined(THRIFT_ntohll) /* Not GNUC/GLIBC or MSVC */
#  define THRIFT_ntohll(n) ( (((uint64_t)ntohl((uint32_t)n)) << 32) + ntohl((uint32_t)(n >> 32)) )
#  define THRIFT_htonll(n) ( (((uint64_t)htonl((uint32_t)n)) << 32) + htonl((uint32_t)(n >> 32)) )
# endif /* GNUC/GLIBC or MSVC or something else */
#else /* __THRIFT_BYTE_ORDER */
# error "Can't define THRIFT_htonll or THRIFT_ntohll!"
#endif

namespace apache {
namespace thrift {
namespace protocol {

using apache::thrift::transport::TTransport;

/**
 * Helper template for implementing TProtocol::skip().
 *
 * Templatized to avoid having to make virtual function calls.
 */
template <class Protocol_>
uint32_t skip(Protocol_& prot, TType type) {
  switch (type) {
  case T_BOOL: {
    bool boolv;
    return prot.readBool(boolv);
  }
  case T_BYTE: {
    int8_t bytev = 0;
    return prot.readByte(bytev);
  }
  case T_I16: {
    int16_t i16;
    return prot.readI16(i16);
  }
  case T_I32: {
    int32_t i32;
    return prot.readI32(i32);
  }
  case T_I64: {
    int64_t i64;
    return prot.readI64(i64);
  }
  case T_DOUBLE: {
    double dub;
    return prot.readDouble(dub);
  }
  case T_STRING: {
    std::string str;
    return prot.readBinary(str);
  }
  case T_STRUCT: {
    uint32_t result = 0;
    std::string name;
    int16_t fid;
    TType ftype;
    result += prot.readStructBegin(name);
    while (true) {
      result += prot.readFieldBegin(name, ftype, fid);
      if (ftype == T_STOP) {
        break;
      }
      result += skip(prot, ftype);
      result += prot.readFieldEnd();
    }
    result += prot.readStructEnd();
    return result;
  }
  case T_MAP: {
    uint32_t result = 0;
    TType keyType;
    TType valType;
    uint32_t i, size;
    result += prot.readMapBegin(keyType, valType, size);
    for (i = 0; i < size; i++) {
      result += skip(prot, keyType);
      result += skip(prot, valType);
    }
    result += prot.readMapEnd();
    return result;
  }
  case T_SET: {
    uint32_t result = 0;
    TType elemType;
    uint32_t i, size;
    result += prot.readSetBegin(elemType, size);
    for (i = 0; i < size; i++) {
      result += skip(prot, elemType);
    }
    result += prot.readSetEnd();
    return result;
  }
  case T_LIST: {
    uint32_t result = 0;
    TType elemType;
    uint32_t i, size;
    result += prot.readListBegin(elemType, size);
    for (i = 0; i < size; i++) {
      result += skip(prot, elemType);
    }
    result += prot.readListEnd();
    return result;
  }
  default:
    break;
  }

  throw TProtocolException(TProtocolException::INVALID_DATA,
                           "invalid TType");
}

}}} // apache::thrift::protocol

#endif // #define _THRIFT_PROTOCOL_TPROTOCOL_H_ 1
