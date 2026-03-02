// Kaitai Struct runtime - C++/STL implementation
// Use iconv for string encoding on Linux
#define KS_STR_ENCODING_ICONV

#include <kaitai/exceptions.h>
#include <kaitai/kaitaistream.h>

#if defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#  include <machine/endian.h>
#  define bswap_16(x) OSSwapInt16(x)
#  define bswap_32(x) OSSwapInt32(x)
#  define bswap_64(x) OSSwapInt64(x)
#  define __BYTE_ORDER BYTE_ORDER
#  define __BIG_ENDIAN BIG_ENDIAN
#  define __LITTLE_ENDIAN LITTLE_ENDIAN
#elif defined(_MSC_VER) // !__APPLE__
#  include <stdlib.h>
#  define __LITTLE_ENDIAN 1234
#  define __BIG_ENDIAN 4321
#  define __BYTE_ORDER __LITTLE_ENDIAN
#  define bswap_16(x) _byteswap_ushort(x)
#  define bswap_32(x) _byteswap_ulong(x)
#  define bswap_64(x) _byteswap_uint64(x)
#elif defined(__QNX__) // __QNX__
#  include <gulliver.h>
#  include <sys/param.h>
#  define bswap_16(x) ENDIAN_RET16(x)
#  define bswap_32(x) ENDIAN_RET32(x)
#  define bswap_64(x) ENDIAN_RET64(x)
#  define __BYTE_ORDER BYTE_ORDER
#  define __BIG_ENDIAN BIG_ENDIAN
#  define __LITTLE_ENDIAN LITTLE_ENDIAN
#else
#  include <sys/param.h>
#  if defined(BSD)
#    include <sys/endian.h>
#    include <sys/types.h>
#    define bswap_16(x) bswap16(x)
#    define bswap_32(x) bswap32(x)
#    define bswap_64(x) bswap64(x)
#    define __BYTE_ORDER BYTE_ORDER
#    define __BIG_ENDIAN BIG_ENDIAN
#    define __LITTLE_ENDIAN LITTLE_ENDIAN
#  else // !__APPLE__ or !_MSC_VER or !__QNX__ or !BSD
#    include <byteswap.h>
#    include <endian.h>
#  endif
#endif

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <istream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <stdint.h>

#ifdef KAITAI_STREAM_H_CPP11_SUPPORT
#  include <type_traits>

template <class To, class From>
typename std::enable_if<sizeof(To) == sizeof(From) && std::is_trivial<From>::value &&
                            std::is_trivial<To>::value,
                        To>::type static bit_cast(const From& src) noexcept {
  To dst;
  std::memcpy(&dst, &src, sizeof(To));
  return dst;
}
#else
template <bool b>
struct StaticAssert;

template <>
struct StaticAssert<true> {};

template <class To, class From>
To static bit_cast(const From& src) {
  StaticAssert<sizeof(To) == sizeof(From)>();

  To dst;
  std::memcpy(&dst, &src, sizeof(To));
  return dst;
}
#endif

kaitai::kstream::kstream(std::istream* io) {
  m_io = io;
  init();
}

kaitai::kstream::kstream(const std::string& data) : m_io_str(data) {
  m_io = &m_io_str;
  init();
}

void kaitai::kstream::init() {
  exceptions_enable();
  align_to_byte();
}

void kaitai::kstream::close() {}

void kaitai::kstream::exceptions_enable() const {
  m_io->exceptions(std::istream::eofbit | std::istream::failbit | std::istream::badbit);
}

// ========================================================================
// Stream positioning
// ========================================================================

bool kaitai::kstream::is_eof() const {
  if (m_bits_left > 0) {
    return false;
  }
  char t;
  m_io->exceptions(std::istream::badbit);
  m_io->get(t);
  if (m_io->eof()) {
    m_io->clear();
    exceptions_enable();
    return true;
  } else {
    m_io->unget();
    exceptions_enable();
    return false;
  }
}

void kaitai::kstream::seek(uint64_t pos) {
  align_to_byte();
  m_io->seekg(pos);
}

uint64_t kaitai::kstream::pos() {
  return m_io->tellg();
}

uint64_t kaitai::kstream::size() {
  std::istream::pos_type cur_pos = m_io->tellg();
  m_io->seekg(0, std::istream::end);
  std::istream::pos_type len = m_io->tellg();
  m_io->seekg(cur_pos);
  return len;
}

// ========================================================================
// Integer numbers
// ========================================================================

int8_t kaitai::kstream::read_s1() {
  align_to_byte();
  char t;
  m_io->get(t);
  return t;
}

int16_t kaitai::kstream::read_s2be() {
  align_to_byte();
  int16_t t;
  m_io->read(reinterpret_cast<char*>(&t), 2);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_16(t);
#endif
  return t;
}

int32_t kaitai::kstream::read_s4be() {
  align_to_byte();
  int32_t t;
  m_io->read(reinterpret_cast<char*>(&t), 4);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_32(t);
#endif
  return t;
}

int64_t kaitai::kstream::read_s8be() {
  align_to_byte();
  int64_t t;
  m_io->read(reinterpret_cast<char*>(&t), 8);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_64(t);
#endif
  return t;
}

int16_t kaitai::kstream::read_s2le() {
  align_to_byte();
  int16_t t;
  m_io->read(reinterpret_cast<char*>(&t), 2);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_16(t);
#endif
  return t;
}

int32_t kaitai::kstream::read_s4le() {
  align_to_byte();
  int32_t t;
  m_io->read(reinterpret_cast<char*>(&t), 4);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_32(t);
#endif
  return t;
}

int64_t kaitai::kstream::read_s8le() {
  align_to_byte();
  int64_t t;
  m_io->read(reinterpret_cast<char*>(&t), 8);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_64(t);
#endif
  return t;
}

uint8_t kaitai::kstream::read_u1() {
  align_to_byte();
  char t;
  m_io->get(t);
  return t;
}

uint16_t kaitai::kstream::read_u2be() {
  align_to_byte();
  uint16_t t;
  m_io->read(reinterpret_cast<char*>(&t), 2);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_16(t);
#endif
  return t;
}

uint32_t kaitai::kstream::read_u4be() {
  align_to_byte();
  uint32_t t;
  m_io->read(reinterpret_cast<char*>(&t), 4);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_32(t);
#endif
  return t;
}

uint64_t kaitai::kstream::read_u8be() {
  align_to_byte();
  uint64_t t;
  m_io->read(reinterpret_cast<char*>(&t), 8);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_64(t);
#endif
  return t;
}

uint16_t kaitai::kstream::read_u2le() {
  align_to_byte();
  uint16_t t;
  m_io->read(reinterpret_cast<char*>(&t), 2);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_16(t);
#endif
  return t;
}

uint32_t kaitai::kstream::read_u4le() {
  align_to_byte();
  uint32_t t;
  m_io->read(reinterpret_cast<char*>(&t), 4);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_32(t);
#endif
  return t;
}

uint64_t kaitai::kstream::read_u8le() {
  align_to_byte();
  uint64_t t;
  m_io->read(reinterpret_cast<char*>(&t), 8);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_64(t);
#endif
  return t;
}

// ========================================================================
// Floating point numbers
// ========================================================================

float kaitai::kstream::read_f4be() {
  align_to_byte();
  uint32_t t;
  m_io->read(reinterpret_cast<char*>(&t), 4);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_32(t);
#endif
  return bit_cast<float>(t);
}

double kaitai::kstream::read_f8be() {
  align_to_byte();
  uint64_t t;
  m_io->read(reinterpret_cast<char*>(&t), 8);
#if __BYTE_ORDER == __LITTLE_ENDIAN
  t = bswap_64(t);
#endif
  return bit_cast<double>(t);
}

float kaitai::kstream::read_f4le() {
  align_to_byte();
  uint32_t t;
  m_io->read(reinterpret_cast<char*>(&t), 4);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_32(t);
#endif
  return bit_cast<float>(t);
}

double kaitai::kstream::read_f8le() {
  align_to_byte();
  uint64_t t;
  m_io->read(reinterpret_cast<char*>(&t), 8);
#if __BYTE_ORDER == __BIG_ENDIAN
  t = bswap_64(t);
#endif
  return bit_cast<double>(t);
}

// ========================================================================
// Unaligned bit values
// ========================================================================

void kaitai::kstream::align_to_byte() {
  m_bits_left = 0;
  m_bits = 0;
}

uint64_t kaitai::kstream::read_bits_int_be(int n) {
  uint64_t res = 0;

  int bits_needed = n - m_bits_left;
  m_bits_left = -bits_needed & 7;

  if (bits_needed > 0) {
    int bytes_needed = ((bits_needed - 1) / 8) + 1;
    if (bytes_needed > 8)
      throw std::runtime_error("read_bits_int_be: more than 8 bytes requested");
    uint8_t buf[8];
    m_io->read(reinterpret_cast<char*>(buf), bytes_needed);
    for (int i = 0; i < bytes_needed; i++) {
      res = res << 8 | buf[i];
    }

    uint64_t new_bits = res;
    res = res >> m_bits_left | (bits_needed < 64 ? m_bits << bits_needed : 0);
    m_bits = new_bits;
  } else {
    res = m_bits >> -bits_needed;
  }

  uint64_t mask = (static_cast<uint64_t>(1) << m_bits_left) - 1;
  m_bits &= mask;

  return res;
}

uint64_t kaitai::kstream::read_bits_int(int n) {
  return read_bits_int_be(n);
}

uint64_t kaitai::kstream::read_bits_int_le(int n) {
  uint64_t res = 0;
  int bits_needed = n - m_bits_left;

  if (bits_needed > 0) {
    int bytes_needed = ((bits_needed - 1) / 8) + 1;
    if (bytes_needed > 8)
      throw std::runtime_error("read_bits_int_le: more than 8 bytes requested");
    uint8_t buf[8];
    m_io->read(reinterpret_cast<char*>(buf), bytes_needed);
    for (int i = 0; i < bytes_needed; i++) {
      res |= static_cast<uint64_t>(buf[i]) << (i * 8);
    }

    uint64_t new_bits = bits_needed < 64 ? res >> bits_needed : 0;
    res = res << m_bits_left | m_bits;
    m_bits = new_bits;
  } else {
    res = m_bits;
    m_bits >>= n;
  }

  m_bits_left = -bits_needed & 7;

  if (n < 64) {
    uint64_t mask = (static_cast<uint64_t>(1) << n) - 1;
    res &= mask;
  }
  return res;
}

// ========================================================================
// Byte arrays
// ========================================================================

std::string kaitai::kstream::read_bytes(std::streamsize len) {
  align_to_byte();
  std::vector<char> result(len);

  if (len < 0) {
    throw std::runtime_error("read_bytes: requested a negative amount");
  }

  if (len > 0) {
    m_io->read(&result[0], len);
  }

  return std::string(result.begin(), result.end());
}

std::string kaitai::kstream::read_bytes_full() {
  align_to_byte();
  std::istream::pos_type p1 = m_io->tellg();
  m_io->seekg(0, std::istream::end);
  std::istream::pos_type p2 = m_io->tellg();
  std::size_t len = p2 - p1;

  std::string result(len, ' ');
  m_io->seekg(p1);
  m_io->read(&result[0], len);

  return result;
}

std::string kaitai::kstream::read_bytes_term(char term, bool include, bool consume,
                                             bool eos_error) {
  align_to_byte();
  std::string result;
  std::getline(*m_io, result, term);
  if (m_io->eof()) {
    if (eos_error) {
      throw std::runtime_error("read_bytes_term: encountered EOF");
    }
  } else {
    if (include)
      result.push_back(term);
    if (!consume)
      m_io->unget();
  }
  return result;
}

std::string kaitai::kstream::read_bytes_term_multi(std::string term, bool include, bool consume,
                                                   bool eos_error) {
  align_to_byte();
  std::size_t term_len = term.length();
  if (term_len > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error("read_bytes_term_multi: terminator too long");
  }
  std::streamsize unit_size = static_cast<std::streamsize>(term_len);

  std::string result;
  std::string c(term_len, ' ');
  m_io->exceptions(std::istream::badbit);
  while (true) {
    m_io->read(&c[0], unit_size);
    if (m_io->eof()) {
      m_io->clear();
      exceptions_enable();
      if (eos_error) {
        throw std::runtime_error("read_bytes_term_multi: encountered EOF");
      }
      result.append(c, 0, static_cast<std::size_t>(m_io->gcount()));
      return result;
    }

    if (c == term) {
      exceptions_enable();
      if (include)
        result += c;
      if (!consume)
        m_io->seekg(-unit_size, std::istream::cur);

      return result;
    }

    result += c;
  }
}

std::string kaitai::kstream::ensure_fixed_contents(std::string expected) {
  std::string actual = read_bytes(expected.length());

  if (actual != expected) {
    throw std::runtime_error("ensure_fixed_contents: actual data does not match expected data");
  }

  return actual;
}

std::string kaitai::kstream::bytes_strip_right(std::string src, char pad_byte) {
  std::size_t new_len = src.length();

  while (new_len > 0 && src[new_len - 1] == pad_byte)
    new_len--;

  return src.substr(0, new_len);
}

std::string kaitai::kstream::bytes_terminate(std::string src, char term, bool include) {
  std::size_t new_len = 0;
  std::size_t max_len = src.length();

  while (new_len < max_len && src[new_len] != term)
    new_len++;

  if (include && new_len < max_len)
    new_len++;

  return src.substr(0, new_len);
}

std::string kaitai::kstream::bytes_terminate_multi(std::string src, std::string term,
                                                   bool include) {
  std::size_t unit_size = term.length();
  if (unit_size == 0) {
    return std::string();
  }
  std::size_t len = src.length();
  std::size_t i_term = 0;
  for (std::size_t i_src = 0; i_src < len;) {
    if (src[i_src] != term[i_term]) {
      i_src += unit_size - i_term;
      i_term = 0;
      continue;
    }
    i_src++;
    i_term++;
    if (i_term == unit_size) {
      return src.substr(0, i_src - (include ? 0 : unit_size));
    }
  }
  return src;
}

// ========================================================================
// Byte array processing
// ========================================================================

std::string kaitai::kstream::process_xor_one(std::string data, uint8_t key) {
  std::size_t len = data.length();
  std::string result(len, ' ');

  for (std::size_t i = 0; i < len; i++)
    result[i] = data[i] ^ key;

  return result;
}

std::string kaitai::kstream::process_xor_many(std::string data, std::string key) {
  std::size_t len = data.length();
  std::size_t kl = key.length();
  std::string result(len, ' ');

  std::size_t ki = 0;
  for (std::size_t i = 0; i < len; i++) {
    result[i] = data[i] ^ key[ki];
    ki++;
    if (ki >= kl)
      ki = 0;
  }

  return result;
}

std::string kaitai::kstream::process_rotate_left(std::string data, int amount) {
  std::size_t len = data.length();
  std::string result(len, ' ');

  for (std::size_t i = 0; i < len; i++) {
    uint8_t bits = data[i];
    result[i] = (bits << amount) | (bits >> (8 - amount));
  }

  return result;
}

// ========================================================================
// Misc utility methods
// ========================================================================

int kaitai::kstream::mod(int a, int b) {
  if (b <= 0)
    throw std::invalid_argument("mod: divisor b <= 0");
  int r = a % b;
  if (r < 0)
    r += b;
  return r;
}

void kaitai::kstream::unsigned_to_decimal(uint64_t number, char* buf,
                                          std::size_t& buf_contents_start) {
  do {
    buf[--buf_contents_start] = static_cast<char>('0' + (number % 10));
    number /= 10;
  } while (number != 0);
}

std::string kaitai::kstream::to_string_signed(int64_t val) {
  char buf[std::numeric_limits<int64_t>::digits10 + 2];
  std::size_t buf_contents_start = sizeof(buf);
  if (val < 0) {
    unsigned_to_decimal((std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(val)) + 1,
                        buf, buf_contents_start);
    buf[--buf_contents_start] = '-';
  } else {
    unsigned_to_decimal(static_cast<uint64_t>(val), buf, buf_contents_start);
  }
  return std::string(&buf[buf_contents_start], sizeof(buf) - buf_contents_start);
}

std::string kaitai::kstream::to_string_unsigned(uint64_t val) {
  char buf[std::numeric_limits<uint64_t>::digits10 + 1];
  std::size_t buf_contents_start = sizeof(buf);
  unsigned_to_decimal(val, buf, buf_contents_start);
  return std::string(&buf[buf_contents_start], sizeof(buf) - buf_contents_start);
}

int64_t kaitai::kstream::string_to_int(const std::string& str, int base) {
  char* str_end;

  errno = 0;
  int64_t res = std::strtoll(str.c_str(), &str_end, base);

  if (str_end != str.c_str() + str.size()) {
    throw std::invalid_argument("string_to_int");
  }

  if (errno == ERANGE) {
    throw std::out_of_range("string_to_int");
  }

  return res;
}

std::string kaitai::kstream::reverse(std::string val) {
  std::reverse(val.begin(), val.end());
  return val;
}

uint8_t kaitai::kstream::byte_array_min(const std::string val) {
  uint8_t min = 0xff;
  std::string::const_iterator end = val.end();
  for (std::string::const_iterator it = val.begin(); it != end; ++it) {
    uint8_t cur = static_cast<uint8_t>(*it);
    if (cur < min) {
      min = cur;
    }
  }
  return min;
}

uint8_t kaitai::kstream::byte_array_max(const std::string val) {
  uint8_t max = 0;
  std::string::const_iterator end = val.end();
  for (std::string::const_iterator it = val.begin(); it != end; ++it) {
    uint8_t cur = static_cast<uint8_t>(*it);
    if (cur > max) {
      max = cur;
    }
  }
  return max;
}

// ========================================================================
// String encoding via iconv
// ========================================================================

#ifdef KS_STR_ENCODING_ICONV
#  include <iconv.h>

#  ifndef KS_STR_DEFAULT_ENCODING
#    define KS_STR_DEFAULT_ENCODING "UTF-8"
#  endif

std::string kaitai::kstream::bytes_to_str(const std::string src, const char* src_enc) {
  iconv_t cd = iconv_open(KS_STR_DEFAULT_ENCODING, src_enc);

  if (cd == (iconv_t)-1) {
    if (errno == EINVAL) {
      throw unknown_encoding(src_enc);
    } else {
      throw bytes_to_str_error("error opening iconv");
    }
  }

  std::size_t src_len = src.length();
  std::size_t src_left = src_len;

  std::size_t dst_len = src_len * 2;
  std::string dst(dst_len, ' ');
  std::size_t dst_left = dst_len;

  char* src_ptr = const_cast<char*>(src.data());
  char* dst_ptr = &dst[0];

  while (true) {
    std::size_t res = iconv(cd, &src_ptr, &src_left, &dst_ptr, &dst_left);

    if (res == (std::size_t)-1) {
      const int saved_errno = errno;
      if (saved_errno == E2BIG) {
        std::size_t dst_used = dst_len - dst_left;
        dst_left += dst_len;
        dst_len += dst_len;
        dst.resize(dst_len);
        dst_ptr = &dst[dst_used];
      } else {
        iconv_close(cd);
        if (saved_errno == EILSEQ) {
          throw illegal_seq_in_encoding("EILSEQ");
        }
        if (saved_errno == EINVAL) {
          throw illegal_seq_in_encoding("EINVAL");
        }
        throw bytes_to_str_error(to_string(saved_errno));
      }
    } else {
      dst.resize(dst_len - dst_left);
      break;
    }
  }

  if (iconv_close(cd) != 0) {
    throw bytes_to_str_error("iconv close error");
  }

  return dst;
}
#endif
