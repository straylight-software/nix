// http3.cpp - HTTP/3 implementation using ngtcp2 + nghttp3
//
// QUIC transport via ngtcp2, HTTP/3 framing via nghttp3

#include "straylight/evring/http3.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <nghttp3/nghttp3.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_quictls.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>

#include "straylight/evring/evring.h"

namespace evring {

// ============================================================================
// Utility functions
// ============================================================================

namespace {

auto timestamp() -> ngtcp2_tstamp {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

auto generate_random_cid() -> quic_cid {
  quic_cid cid;
  cid.len = 16; // Standard CID length
  RAND_bytes(cid.data.data(), static_cast<int>(cid.len));
  return cid;
}

} // namespace

// ============================================================================
// Error string conversion
// ============================================================================

auto http3_error_string(http3_error_code error) noexcept -> std::string_view {
  switch (error) {
    case http3_error_code::no_error:
      return "no error";
    case http3_error_code::general_protocol_error:
      return "general protocol error";
    case http3_error_code::internal_error:
      return "internal error";
    case http3_error_code::stream_creation_error:
      return "stream creation error";
    case http3_error_code::closed_critical_stream:
      return "closed critical stream";
    case http3_error_code::frame_unexpected:
      return "frame unexpected";
    case http3_error_code::frame_error:
      return "frame error";
    case http3_error_code::excessive_load:
      return "excessive load";
    case http3_error_code::id_error:
      return "ID error";
    case http3_error_code::settings_error:
      return "settings error";
    case http3_error_code::missing_settings:
      return "missing settings";
    case http3_error_code::request_rejected:
      return "request rejected";
    case http3_error_code::request_cancelled:
      return "request cancelled";
    case http3_error_code::request_incomplete:
      return "request incomplete";
    case http3_error_code::message_error:
      return "message error";
    case http3_error_code::connect_error:
      return "connect error";
    case http3_error_code::version_fallback:
      return "version fallback";
    case http3_error_code::quic_error:
      return "QUIC error";
    case http3_error_code::tls_error:
      return "TLS error";
    case http3_error_code::connection_closed:
      return "connection closed";
    case http3_error_code::timeout:
      return "timeout";
    default:
      return "unknown error";
  }
}

// ============================================================================
// quic_cid implementation
// ============================================================================

auto quic_cid::generate() -> quic_cid {
  return generate_random_cid();
}

// ============================================================================
// http3_request implementation
// ============================================================================

auto http3_request::all_headers() const -> http3_headers {
  http3_headers result;
  result.reserve(4 + headers.size());

  // Pseudo-headers must come first
  result.push_back({":method", method});
  result.push_back({":scheme", scheme});
  result.push_back({":authority", authority});
  result.push_back({":path", path});

  // Regular headers
  for (const auto& h : headers) {
    result.push_back(h);
  }

  return result;
}

// ============================================================================
// http3_response implementation
// ============================================================================

auto http3_response::get_header(std::string_view name) const -> std::string_view {
  for (const auto& h : headers) {
    if (h.name.size() == name.size()) {
      bool match = true;
      for (std::size_t i = 0; i < name.size() && match; ++i) {
        if (std::tolower(static_cast<unsigned char>(h.name[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
          match = false;
        }
      }
      if (match) {
        return h.value;
      }
    }
  }
  return {};
}

// ============================================================================
// ngtcp2 callbacks
// ============================================================================

namespace {

int client_initial_cb([[maybe_unused]] ngtcp2_conn* conn, void* user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  if (ngtcp2_crypto_client_initial_cb(conn, session) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int recv_crypto_data_cb(ngtcp2_conn* conn, ngtcp2_encryption_level encryption_level,
                        [[maybe_unused]] uint64_t offset, const uint8_t* data, size_t datalen,
                        void* user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  if (ngtcp2_crypto_recv_crypto_data_cb(conn, encryption_level, offset, data, datalen, session) !=
      0) {
    return NGTCP2_ERR_CRYPTO;
  }
  return 0;
}

int encrypt_cb(uint8_t* dest, const ngtcp2_crypto_aead* aead,
               const ngtcp2_crypto_aead_ctx* aead_ctx, const uint8_t* plaintext,
               size_t plaintextlen, const uint8_t* nonce, size_t noncelen, const uint8_t* aad,
               size_t aadlen) {
  if (ngtcp2_crypto_encrypt_cb(dest, aead, aead_ctx, plaintext, plaintextlen, nonce, noncelen, aad,
                               aadlen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int decrypt_cb(uint8_t* dest, const ngtcp2_crypto_aead* aead,
               const ngtcp2_crypto_aead_ctx* aead_ctx, const uint8_t* ciphertext,
               size_t ciphertextlen, const uint8_t* nonce, size_t noncelen, const uint8_t* aad,
               size_t aadlen) {
  if (ngtcp2_crypto_decrypt_cb(dest, aead, aead_ctx, ciphertext, ciphertextlen, nonce, noncelen,
                               aad, aadlen) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int hp_mask_cb(uint8_t* dest, const ngtcp2_crypto_cipher* hp,
               const ngtcp2_crypto_cipher_ctx* hp_ctx, const uint8_t* sample) {
  if (ngtcp2_crypto_hp_mask_cb(dest, hp, hp_ctx, sample) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int recv_stream_data_cb(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id, uint64_t offset,
                        const uint8_t* data, size_t datalen, void* user_data,
                        void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  auto* http3_conn = session->raw_http3();

  if (!http3_conn) {
    return 0;
  }

  auto consumed = nghttp3_conn_read_stream(http3_conn, stream_id, data, datalen,
                                           flags & NGTCP2_STREAM_DATA_FLAG_FIN);
  if (consumed < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  ngtcp2_conn_extend_max_stream_offset(conn, stream_id, static_cast<uint64_t>(consumed));
  ngtcp2_conn_extend_max_offset(conn, static_cast<uint64_t>(consumed));

  return 0;
}

int acked_stream_data_offset_cb([[maybe_unused]] ngtcp2_conn* conn, int64_t stream_id,
                                uint64_t offset, uint64_t datalen, void* user_data,
                                [[maybe_unused]] void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  auto* http3_conn = session->raw_http3();

  if (!http3_conn) {
    return 0;
  }

  int rv = nghttp3_conn_add_ack_offset(http3_conn, stream_id, datalen);
  if (rv != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}

int stream_open_cb([[maybe_unused]] ngtcp2_conn* conn, int64_t stream_id, void* user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  auto* http3_conn = session->raw_http3();

  if (!http3_conn) {
    return 0;
  }

  // Only interested in server-initiated streams
  if (!ngtcp2_is_bidi_stream(stream_id)) {
    return 0;
  }

  return 0;
}

int stream_close_cb(ngtcp2_conn* conn, uint32_t flags, int64_t stream_id, uint64_t app_error_code,
                    void* user_data, [[maybe_unused]] void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  auto* http3_conn = session->raw_http3();

  if (http3_conn) {
    int rv =
        nghttp3_conn_close_stream(http3_conn, stream_id, static_cast<uint64_t>(app_error_code));
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }
  }

  session->handle_stream_close(stream_id, app_error_code);
  return 0;
}

int stream_reset_cb([[maybe_unused]] ngtcp2_conn* conn, int64_t stream_id,
                    [[maybe_unused]] uint64_t final_size, uint64_t app_error_code, void* user_data,
                    [[maybe_unused]] void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  auto* http3_conn = session->raw_http3();

  if (http3_conn) {
    int rv = nghttp3_conn_shutdown_stream_read(http3_conn, stream_id);
    if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }
  }

  return 0;
}

int get_new_connection_id_cb(ngtcp2_conn* conn, ngtcp2_cid* cid, uint8_t* token,
                             [[maybe_unused]] size_t cidlen, [[maybe_unused]] void* user_data) {
  if (RAND_bytes(cid->data, static_cast<int>(cid->datalen)) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int update_key_cb(ngtcp2_conn* conn, uint8_t* rx_secret, uint8_t* tx_secret,
                  ngtcp2_crypto_aead_ctx* rx_aead_ctx, uint8_t* rx_iv,
                  ngtcp2_crypto_aead_ctx* tx_aead_ctx, uint8_t* tx_iv,
                  const uint8_t* current_rx_secret, const uint8_t* current_tx_secret,
                  size_t secretlen, void* user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  if (ngtcp2_crypto_update_key_cb(conn, rx_secret, tx_secret, rx_aead_ctx, rx_iv, tx_aead_ctx,
                                  tx_iv, current_rx_secret, current_tx_secret, secretlen,
                                  session) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

int handshake_completed_cb([[maybe_unused]] ngtcp2_conn* conn, void* user_data) {
  // HTTP/3 setup will happen after handshake
  return 0;
}

int recv_version_negotiation_cb([[maybe_unused]] ngtcp2_conn* conn,
                                [[maybe_unused]] const ngtcp2_pkt_hd* hd,
                                [[maybe_unused]] const uint32_t* sv, [[maybe_unused]] size_t nsv,
                                [[maybe_unused]] void* user_data) {
  return 0;
}

void delete_crypto_aead_ctx_cb([[maybe_unused]] ngtcp2_conn* conn, ngtcp2_crypto_aead_ctx* aead_ctx,
                               [[maybe_unused]] void* user_data) {
  ngtcp2_crypto_delete_crypto_aead_ctx_cb(conn, aead_ctx, user_data);
}

void delete_crypto_cipher_ctx_cb([[maybe_unused]] ngtcp2_conn* conn,
                                 ngtcp2_crypto_cipher_ctx* cipher_ctx,
                                 [[maybe_unused]] void* user_data) {
  ngtcp2_crypto_delete_crypto_cipher_ctx_cb(conn, cipher_ctx, user_data);
}

} // namespace

// ============================================================================
// nghttp3 callbacks
// ============================================================================

namespace {

int nghttp3_acked_stream_data_cb([[maybe_unused]] nghttp3_conn* conn, int64_t stream_id,
                                 uint64_t datalen, void* user_data, void* stream_user_data) {
  // Nothing to do for simple client
  return 0;
}

int nghttp3_stream_close_cb(nghttp3_conn* conn, int64_t stream_id, uint64_t app_error_code,
                            void* user_data, void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  session->handle_stream_close(stream_id, app_error_code);
  return 0;
}

int nghttp3_recv_data_cb([[maybe_unused]] nghttp3_conn* conn, int64_t stream_id,
                         const uint8_t* data, size_t datalen, void* user_data,
                         [[maybe_unused]] void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  session->handle_data(
      stream_id, std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), datalen));
  return 0;
}

int nghttp3_deferred_consume_cb([[maybe_unused]] nghttp3_conn* conn, int64_t stream_id,
                                size_t nconsumed, void* user_data, void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  ngtcp2_conn_extend_max_stream_offset(session->raw_quic(), stream_id, nconsumed);
  ngtcp2_conn_extend_max_offset(session->raw_quic(), nconsumed);
  return 0;
}

int nghttp3_begin_headers_cb(nghttp3_conn* conn, int64_t stream_id, void* user_data,
                             void* stream_user_data) {
  return 0;
}

int nghttp3_recv_header_cb([[maybe_unused]] nghttp3_conn* conn, int64_t stream_id, int32_t token,
                           nghttp3_rcbuf* name, nghttp3_rcbuf* value, uint8_t flags,
                           void* user_data, [[maybe_unused]] void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);

  auto name_vec = nghttp3_rcbuf_get_buf(name);
  auto value_vec = nghttp3_rcbuf_get_buf(value);

  session->handle_header(
      stream_id, std::string_view(reinterpret_cast<const char*>(name_vec.base), name_vec.len),
      std::string_view(reinterpret_cast<const char*>(value_vec.base), value_vec.len));

  return 0;
}

int nghttp3_end_headers_cb([[maybe_unused]] nghttp3_conn* conn, int64_t stream_id, int fin,
                           void* user_data, void* stream_user_data) {
  return 0;
}

int nghttp3_end_stream_cb([[maybe_unused]] nghttp3_conn* conn, int64_t stream_id, void* user_data,
                          void* stream_user_data) {
  return 0;
}

int nghttp3_stop_sending_cb(nghttp3_conn* conn, int64_t stream_id, uint64_t app_error_code,
                            void* user_data, void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  ngtcp2_conn_shutdown_stream_read(session->raw_quic(), 0, stream_id, app_error_code);
  return 0;
}

int nghttp3_reset_stream_cb(nghttp3_conn* conn, int64_t stream_id, uint64_t app_error_code,
                            void* user_data, void* stream_user_data) {
  auto* session = static_cast<http3_session*>(user_data);
  ngtcp2_conn_shutdown_stream_write(session->raw_quic(), 0, stream_id, app_error_code);
  return 0;
}

} // namespace

// ============================================================================
// http3_session implementation
// ============================================================================

http3_session::http3_session() = default;

http3_session::~http3_session() {
  if (http3_conn_) {
    nghttp3_conn_del(http3_conn_);
  }
  if (quic_conn_) {
    ngtcp2_conn_del(quic_conn_);
  }
  if (ssl_) {
    SSL_free(ssl_);
  }
  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
  }
}

http3_session::http3_session(http3_session&& other) noexcept
    : quic_conn_(other.quic_conn_),
      http3_conn_(other.http3_conn_),
      ssl_ctx_(other.ssl_ctx_),
      ssl_(other.ssl_),
      scid_(other.scid_),
      dcid_(other.dcid_),
      local_addr_storage_(std::move(other.local_addr_storage_)),
      remote_addr_storage_(std::move(other.remote_addr_storage_)),
      pending_headers_(std::move(other.pending_headers_)),
      stream_responses_(std::move(other.stream_responses_)),
      closed_streams_(std::move(other.closed_streams_)),
      on_headers_(std::move(other.on_headers_)),
      on_data_(std::move(other.on_data_)),
      on_stream_close_(std::move(other.on_stream_close_)),
      error_message_(std::move(other.error_message_)) {
  other.quic_conn_ = nullptr;
  other.http3_conn_ = nullptr;
  other.ssl_ctx_ = nullptr;
  other.ssl_ = nullptr;
}

http3_session& http3_session::operator=(http3_session&& other) noexcept {
  if (this != &other) {
    if (http3_conn_) {
      nghttp3_conn_del(http3_conn_);
    }
    if (quic_conn_) {
      ngtcp2_conn_del(quic_conn_);
    }
    if (ssl_) {
      SSL_free(ssl_);
    }
    if (ssl_ctx_) {
      SSL_CTX_free(ssl_ctx_);
    }

    quic_conn_ = other.quic_conn_;
    http3_conn_ = other.http3_conn_;
    ssl_ctx_ = other.ssl_ctx_;
    ssl_ = other.ssl_;
    scid_ = other.scid_;
    dcid_ = other.dcid_;
    local_addr_storage_ = std::move(other.local_addr_storage_);
    remote_addr_storage_ = std::move(other.remote_addr_storage_);
    pending_headers_ = std::move(other.pending_headers_);
    stream_responses_ = std::move(other.stream_responses_);
    closed_streams_ = std::move(other.closed_streams_);
    on_headers_ = std::move(other.on_headers_);
    on_data_ = std::move(other.on_data_);
    on_stream_close_ = std::move(other.on_stream_close_);
    error_message_ = std::move(other.error_message_);

    other.quic_conn_ = nullptr;
    other.http3_conn_ = nullptr;
    other.ssl_ctx_ = nullptr;
    other.ssl_ = nullptr;
  }
  return *this;
}

auto http3_session::setup_ssl(const char* server_name) -> bool {
  ssl_ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ssl_ctx_) {
    error_message_ = "Failed to create SSL_CTX";
    return false;
  }

  SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);

  // Set ALPN to h3
  static const unsigned char alpn[] = {2, 'h', '3'};
  SSL_CTX_set_alpn_protos(ssl_ctx_, alpn, sizeof(alpn));

  // Load system CA certificates
  SSL_CTX_set_default_verify_paths(ssl_ctx_);

  // Configure SSL context for QUIC (quictls API - must be before SSL_new)
  if (ngtcp2_crypto_quictls_configure_client_context(ssl_ctx_) != 0) {
    error_message_ = "Failed to configure ngtcp2 crypto context";
    return false;
  }

  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    error_message_ = "Failed to create SSL";
    return false;
  }

  SSL_set_connect_state(ssl_);
  SSL_set_tlsext_host_name(ssl_, server_name);

  return true;
}

auto http3_session::setup_quic(const sockaddr* local_addr, socklen_t local_addrlen,
                               const sockaddr* remote_addr, socklen_t remote_addrlen) -> bool {
  // Generate connection IDs
  scid_ = quic_cid::generate();
  dcid_ = quic_cid::generate();

  // Store addresses
  local_addr_storage_.resize(local_addrlen);
  std::memcpy(local_addr_storage_.data(), local_addr, local_addrlen);

  remote_addr_storage_.resize(remote_addrlen);
  std::memcpy(remote_addr_storage_.data(), remote_addr, remote_addrlen);

  // Setup path
  ngtcp2_path path;
  path.local.addr = reinterpret_cast<sockaddr*>(local_addr_storage_.data());
  path.local.addrlen = local_addrlen;
  path.remote.addr = reinterpret_cast<sockaddr*>(remote_addr_storage_.data());
  path.remote.addrlen = remote_addrlen;

  // Setup CIDs
  ngtcp2_cid scid, dcid;
  scid.datalen = scid_.len;
  std::memcpy(scid.data, scid_.data.data(), scid_.len);
  dcid.datalen = dcid_.len;
  std::memcpy(dcid.data, dcid_.data.data(), dcid_.len);

  // Setup callbacks
  ngtcp2_callbacks callbacks{};
  callbacks.client_initial = client_initial_cb;
  callbacks.recv_crypto_data = recv_crypto_data_cb;
  callbacks.encrypt = encrypt_cb;
  callbacks.decrypt = decrypt_cb;
  callbacks.hp_mask = hp_mask_cb;
  callbacks.recv_stream_data = recv_stream_data_cb;
  callbacks.acked_stream_data_offset = acked_stream_data_offset_cb;
  callbacks.stream_open = stream_open_cb;
  callbacks.stream_close = stream_close_cb;
  callbacks.stream_reset = stream_reset_cb;
  callbacks.get_new_connection_id = get_new_connection_id_cb;
  callbacks.update_key = update_key_cb;
  callbacks.handshake_completed = handshake_completed_cb;
  callbacks.recv_version_negotiation = recv_version_negotiation_cb;
  callbacks.delete_crypto_aead_ctx = delete_crypto_aead_ctx_cb;
  callbacks.delete_crypto_cipher_ctx = delete_crypto_cipher_ctx_cb;

  // Setup settings
  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = timestamp();
  settings.log_printf = nullptr;

  // Setup transport params
  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);
  params.initial_max_streams_bidi = http3_default_max_streams_bidi;
  params.initial_max_streams_uni = 3; // For HTTP/3 control streams
  params.initial_max_stream_data_bidi_local = http3_default_max_stream_data;
  params.initial_max_stream_data_bidi_remote = http3_default_max_stream_data;
  params.initial_max_stream_data_uni = http3_default_max_stream_data;
  params.initial_max_data = http3_default_max_data;
  params.max_idle_timeout = http3_default_idle_timeout * NGTCP2_SECONDS;

  int rv = ngtcp2_conn_client_new(&quic_conn_, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks,
                                  &settings, &params, nullptr, this);
  if (rv != 0) {
    error_message_ = "Failed to create ngtcp2 connection: ";
    error_message_ += ngtcp2_strerror(rv);
    return false;
  }

  ngtcp2_conn_set_tls_native_handle(quic_conn_, ssl_);

  return true;
}

auto http3_session::setup_http3(const http3_settings& settings) -> bool {
  nghttp3_callbacks callbacks{};
  callbacks.acked_stream_data = nghttp3_acked_stream_data_cb;
  callbacks.stream_close = nghttp3_stream_close_cb;
  callbacks.recv_data = nghttp3_recv_data_cb;
  callbacks.deferred_consume = nghttp3_deferred_consume_cb;
  callbacks.begin_headers = nghttp3_begin_headers_cb;
  callbacks.recv_header = nghttp3_recv_header_cb;
  callbacks.end_headers = nghttp3_end_headers_cb;
  callbacks.end_stream = nghttp3_end_stream_cb;
  callbacks.stop_sending = nghttp3_stop_sending_cb;
  callbacks.reset_stream = nghttp3_reset_stream_cb;

  nghttp3_settings nghttp3_settings;
  nghttp3_settings_default(&nghttp3_settings);
  nghttp3_settings.max_field_section_size = settings.max_field_section_size;
  nghttp3_settings.qpack_max_dtable_capacity = settings.qpack_max_dtable_capacity;
  nghttp3_settings.qpack_blocked_streams = settings.qpack_blocked_streams;

  int rv = nghttp3_conn_client_new(&http3_conn_, &callbacks, &nghttp3_settings, nullptr, this);
  if (rv != 0) {
    error_message_ = "Failed to create nghttp3 connection: ";
    error_message_ += nghttp3_strerror(rv);
    return false;
  }

  // Open control streams
  int64_t ctrl_stream_id;
  rv = ngtcp2_conn_open_uni_stream(quic_conn_, &ctrl_stream_id, nullptr);
  if (rv != 0) {
    error_message_ = "Failed to open control stream";
    return false;
  }
  rv = nghttp3_conn_bind_control_stream(http3_conn_, ctrl_stream_id);
  if (rv != 0) {
    error_message_ = "Failed to bind control stream";
    return false;
  }

  int64_t qpack_enc_stream_id, qpack_dec_stream_id;
  rv = ngtcp2_conn_open_uni_stream(quic_conn_, &qpack_enc_stream_id, nullptr);
  if (rv != 0) {
    error_message_ = "Failed to open QPACK encoder stream";
    return false;
  }
  rv = ngtcp2_conn_open_uni_stream(quic_conn_, &qpack_dec_stream_id, nullptr);
  if (rv != 0) {
    error_message_ = "Failed to open QPACK decoder stream";
    return false;
  }
  rv = nghttp3_conn_bind_qpack_streams(http3_conn_, qpack_enc_stream_id, qpack_dec_stream_id);
  if (rv != 0) {
    error_message_ = "Failed to bind QPACK streams";
    return false;
  }

  return true;
}

auto http3_session::init_client(const char* server_name, const sockaddr* local_addr,
                                socklen_t local_addrlen, const sockaddr* remote_addr,
                                socklen_t remote_addrlen, const http3_settings& settings) -> bool {
  if (!setup_ssl(server_name)) {
    return false;
  }

  if (!setup_quic(local_addr, local_addrlen, remote_addr, remote_addrlen)) {
    return false;
  }

  // HTTP/3 setup happens after handshake completes
  // We'll call setup_http3 when handshake_complete() becomes true

  return true;
}

auto http3_session::handshake_complete() const noexcept -> bool {
  return quic_conn_ && ngtcp2_conn_get_handshake_completed(quic_conn_);
}

auto http3_session::submit_request(const http3_request& req) -> std::int64_t {
  if (!http3_conn_) {
    return -1;
  }

  auto all = req.all_headers();
  std::vector<nghttp3_nv> nva;
  nva.reserve(all.size());

  for (const auto& h : all) {
    nghttp3_nv nv;
    nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(h.name.data()));
    nv.namelen = h.name.size();
    nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(h.value.data()));
    nv.valuelen = h.value.size();
    nv.flags = NGHTTP3_NV_FLAG_NONE;
    nva.push_back(nv);
  }

  // Open bidirectional stream
  int64_t stream_id;
  int rv = ngtcp2_conn_open_bidi_stream(quic_conn_, &stream_id, nullptr);
  if (rv != 0) {
    return -1;
  }

  // Submit request
  rv =
      nghttp3_conn_submit_request(http3_conn_, stream_id, nva.data(), nva.size(), nullptr, nullptr);
  if (rv != 0) {
    return -1;
  }

  return stream_id;
}

auto http3_session::write_pkt(std::span<std::byte> dest) -> std::int64_t {
  if (!quic_conn_) {
    return -1;
  }

  ngtcp2_path_storage ps;
  ngtcp2_path_storage_zero(&ps);

  ngtcp2_pkt_info pi;
  uint8_t* buf = reinterpret_cast<uint8_t*>(dest.data());

  // First try to write HTTP/3 data if we have an HTTP/3 connection
  if (http3_conn_) {
    // Check for pending HTTP/3 stream data
    nghttp3_vec vec[16];
    int64_t stream_id;
    int fin;

    for (;;) {
      auto sveccnt = nghttp3_conn_writev_stream(http3_conn_, &stream_id, &fin, vec, 16);
      if (sveccnt < 0) {
        return -1;
      }
      if (sveccnt == 0) {
        break;
      }

      ngtcp2_vec* v = reinterpret_cast<ngtcp2_vec*>(vec);
      auto ndatalen = ngtcp2_conn_writev_stream(quic_conn_, &ps.path, &pi, buf, dest.size(),
                                                nullptr, NGTCP2_WRITE_STREAM_FLAG_MORE, stream_id,
                                                v, static_cast<size_t>(sveccnt), timestamp());
      if (ndatalen < 0) {
        if (ndatalen == NGTCP2_ERR_WRITE_MORE) {
          auto consumed =
              nghttp3_conn_add_write_offset(http3_conn_, stream_id, static_cast<size_t>(ndatalen));
          if (consumed < 0) {
            return -1;
          }
          continue;
        }
        return ndatalen;
      }

      if (ndatalen > 0) {
        return ndatalen;
      }
      break;
    }
  }

  // Write regular QUIC packet (handshake, ACKs, etc.)
  auto nwrite = ngtcp2_conn_write_pkt(quic_conn_, &ps.path, &pi, buf, dest.size(), timestamp());
  if (nwrite < 0) {
    if (nwrite == NGTCP2_ERR_WRITE_MORE) {
      return 0;
    }
    return nwrite;
  }

  return nwrite;
}

auto http3_session::read_pkt(std::span<const std::byte> data) -> int {
  if (!quic_conn_) {
    return -1;
  }

  ngtcp2_path path;
  path.local.addr = reinterpret_cast<sockaddr*>(local_addr_storage_.data());
  path.local.addrlen = static_cast<socklen_t>(local_addr_storage_.size());
  path.remote.addr = reinterpret_cast<sockaddr*>(remote_addr_storage_.data());
  path.remote.addrlen = static_cast<socklen_t>(remote_addr_storage_.size());

  ngtcp2_pkt_info pi{};

  int rv =
      ngtcp2_conn_read_pkt(quic_conn_, &path, &pi, reinterpret_cast<const uint8_t*>(data.data()),
                           data.size(), timestamp());
  if (rv != 0) {
    error_message_ = ngtcp2_strerror(rv);
    return rv;
  }

  // Setup HTTP/3 after handshake completes
  if (handshake_complete() && !http3_conn_) {
    if (!setup_http3({})) {
      return -1;
    }
  }

  return 0;
}

auto http3_session::handle_expiry() -> int {
  if (!quic_conn_) {
    return -1;
  }

  int rv = ngtcp2_conn_handle_expiry(quic_conn_, timestamp());
  if (rv != 0) {
    error_message_ = ngtcp2_strerror(rv);
    return rv;
  }

  return 0;
}

auto http3_session::get_timeout() const -> std::uint64_t {
  if (!quic_conn_) {
    return UINT64_MAX;
  }

  auto expiry = ngtcp2_conn_get_expiry(quic_conn_);
  auto now = timestamp();

  if (expiry <= now) {
    return 0;
  }

  return expiry - now;
}

auto http3_session::wants_write() const noexcept -> bool {
  if (!quic_conn_) {
    return false;
  }

  // Check if QUIC layer wants to write
  if (ngtcp2_conn_get_expiry(quic_conn_) != UINT64_MAX) {
    return true;
  }

  // Check if HTTP/3 layer has pending data
  if (http3_conn_) {
    int64_t stream_id;
    int fin;
    nghttp3_vec vec[1];
    auto n = nghttp3_conn_writev_stream(http3_conn_, &stream_id, &fin, vec, 1);
    if (n > 0) {
      return true;
    }
  }

  return false;
}

auto http3_session::is_draining() const noexcept -> bool {
  return quic_conn_ && ngtcp2_conn_in_draining_period(quic_conn_);
}

auto http3_session::close(http3_error_code error) -> int {
  // Connection close is handled via write_pkt with connection close frame
  return 0;
}

auto http3_session::get_stream_response(std::int64_t stream_id) -> http3_response* {
  auto it = stream_responses_.find(stream_id);
  if (it != stream_responses_.end()) {
    return &it->second;
  }
  return nullptr;
}

auto http3_session::is_stream_closed(std::int64_t stream_id) const -> bool {
  return closed_streams_.find(stream_id) != closed_streams_.end();
}

auto http3_session::error() const -> const char* {
  return error_message_.c_str();
}

void http3_session::handle_header(std::int64_t stream_id, std::string_view name,
                                  std::string_view value) {
  auto& resp = stream_responses_[stream_id];

  if (name == ":status") {
    resp.status_code = std::stoi(std::string(value));
  } else if (!name.empty() && name[0] != ':') {
    resp.headers.push_back({std::string(name), std::string(value)});
  }

  pending_headers_[stream_id].push_back({std::string(name), std::string(value)});

  if (on_headers_) {
    on_headers_(stream_id, pending_headers_[stream_id]);
  }
}

void http3_session::handle_data(std::int64_t stream_id, std::span<const std::byte> data) {
  auto& resp = stream_responses_[stream_id];
  resp.body.insert(resp.body.end(), data.begin(), data.end());

  if (on_data_) {
    on_data_(stream_id, data);
  }
}

void http3_session::handle_stream_close(std::int64_t stream_id, std::uint64_t error_code) {
  auto ec = static_cast<http3_error_code>(error_code);
  closed_streams_[stream_id] = ec;

  if (on_stream_close_) {
    on_stream_close_(stream_id, ec);
  }
}

// ============================================================================
// http3_client_machine implementation
// ============================================================================

http3_client_machine::http3_client_machine(http3_client_config config)
    : config_(std::move(config)) {}

auto http3_client_machine::initial() const -> state_type {
  state_type s;
  s.server_name = config_.server_name;
  s.port = config_.port;
  return s;
}

auto http3_client_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  std::vector<operation> ops;

  switch (s.current_phase) {
    case state_type::phase::initial: {
      // DNS resolution (blocking for now - could make async)
      struct addrinfo hints{};
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_DGRAM;
      hints.ai_protocol = IPPROTO_UDP;

      struct addrinfo* result = nullptr;
      std::string port_str = std::to_string(s.port);
      int rv = getaddrinfo(s.server_name.c_str(), port_str.c_str(), &hints, &result);
      if (rv != 0) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::connect_error;
        s.error_message = "DNS resolution failed: ";
        s.error_message += gai_strerror(rv);
        return {std::move(s), {}};
      }

      // Store remote address
      s.remote_addr.resize(result->ai_addrlen);
      std::memcpy(s.remote_addr.data(), result->ai_addr, result->ai_addrlen);
      freeaddrinfo(result);

      // Create UDP socket
      s.current_phase = state_type::phase::creating_socket;
      ops.push_back(operation::make_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, SOCK_CLOEXEC));
      break;
    }

    case state_type::phase::creating_socket: {
      if (!e.ok()) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::connect_error;
        s.error_message = "Socket creation failed";
        return {std::move(s), {}};
      }

      s.socket_handle = e.resource_handle;

      // Get local address (bind to any)
      struct sockaddr_in local_addr{};
      local_addr.sin_family = AF_INET;
      local_addr.sin_addr.s_addr = INADDR_ANY;
      local_addr.sin_port = 0;
      s.local_addr.resize(sizeof(local_addr));
      std::memcpy(s.local_addr.data(), &local_addr, sizeof(local_addr));

      // Connect UDP socket to enable send/recv semantics
      s.current_phase = state_type::phase::binding_socket;
      ops.push_back(operation::make_connect(
          s.socket_handle, reinterpret_cast<const sockaddr*>(s.remote_addr.data()),
          static_cast<std::uint32_t>(s.remote_addr.size()), ++s.operation_id));
      break;
    }

    case state_type::phase::binding_socket: {
      if (!e.ok()) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::connect_error;
        s.error_message = "UDP connect failed";
        return {std::move(s), {}};
      }

      // Initialize QUIC connection
      if (!session_.init_client(s.server_name.c_str(),
                                reinterpret_cast<const sockaddr*>(s.local_addr.data()),
                                static_cast<socklen_t>(s.local_addr.size()),
                                reinterpret_cast<const sockaddr*>(s.remote_addr.data()),
                                static_cast<socklen_t>(s.remote_addr.size()), config_.settings)) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::quic_error;
        s.error_message = session_.error();
        return {std::move(s), {}};
      }

      s.current_phase = state_type::phase::connecting;
      // Fall through to send initial packet
      return do_send(std::move(s));
    }

    case state_type::phase::connecting:
    case state_type::phase::waiting_send: {
      if (!e.ok()) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::quic_error;
        s.error_message = "Send failed";
        return {std::move(s), {}};
      }

      // Check if handshake is complete
      if (session_.handshake_complete()) {
        s.current_phase = state_type::phase::connected;
        return {std::move(s), {}};
      }

      // Wait for response
      s.current_phase = state_type::phase::waiting_recv;
      ops.push_back(operation::make_recv(s.socket_handle, make_stable_span(recv_buffer_), 0,
                                         ++s.operation_id));
      break;
    }

    case state_type::phase::waiting_recv: {
      return do_recv(std::move(s), e);
    }

    case state_type::phase::waiting_timeout: {
      return do_timeout(std::move(s));
    }

    case state_type::phase::connected:
    case state_type::phase::draining:
    case state_type::phase::done:
    case state_type::phase::error:
      break;

    default:
      break;
  }

  return {std::move(s), std::move(ops)};
}

auto http3_client_machine::do_send(state_type s) const -> step_result<state_type> {
  std::vector<operation> ops;

  auto nwrite = session_.write_pkt(std::span{send_buffer_});
  if (nwrite < 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = session_.error();
    return {std::move(s), {}};
  }

  if (nwrite > 0) {
    s.current_phase = state_type::phase::waiting_send;
    // Using connected UDP socket, so we can use regular send
    ops.push_back(operation::make_send(
        s.socket_handle,
        std::span<const std::byte>{send_buffer_.data(), static_cast<std::size_t>(nwrite)}, 0,
        ++s.operation_id));
    return {std::move(s), std::move(ops)};
  }

  // Nothing to send, wait for recv or timeout
  auto timeout_ns = session_.get_timeout();
  if (timeout_ns < UINT64_MAX) {
    s.current_phase = state_type::phase::waiting_timeout;
    // Convert to milliseconds for timeout
    auto timeout_ms = timeout_ns / 1000000;
    if (timeout_ms == 0) {
      timeout_ms = 1;
    }
    ops.push_back(
        operation::make_timeout(static_cast<std::uint32_t>(timeout_ms), ++s.operation_id));
  } else {
    s.current_phase = state_type::phase::waiting_recv;
    ops.push_back(
        operation::make_recv(s.socket_handle, make_stable_span(recv_buffer_), 0, ++s.operation_id));
  }

  return {std::move(s), std::move(ops)};
}

auto http3_client_machine::do_recv(state_type s, const event& e) const -> step_result<state_type> {
  if (!e.ok()) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = "Recv failed";
    return {std::move(s), {}};
  }

  if (e.result == 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::connection_closed;
    s.error_message = "Connection closed";
    return {std::move(s), {}};
  }

  // Process received packet
  std::span<const std::byte> data(recv_buffer_.data(), static_cast<std::size_t>(e.result));
  int rv = session_.read_pkt(data);
  if (rv != 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = session_.error();
    return {std::move(s), {}};
  }

  // Check if handshake is complete
  if (session_.handshake_complete()) {
    s.current_phase = state_type::phase::connected;
    return {std::move(s), {}};
  }

  // Send response packets
  return do_send(std::move(s));
}

auto http3_client_machine::do_timeout(state_type s) const -> step_result<state_type> {
  int rv = session_.handle_expiry();
  if (rv != 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = session_.error();
    return {std::move(s), {}};
  }

  return do_send(std::move(s));
}

auto http3_client_machine::done(const state_type& s) const -> bool {
  return s.current_phase == state_type::phase::connected ||
         s.current_phase == state_type::phase::done || s.current_phase == state_type::phase::error;
}

// ============================================================================
// http3_request_machine implementation
// ============================================================================

http3_request_machine::http3_request_machine(http3_session& session, handle socket,
                                             http3_request request)
    : session_(&session), socket_(socket), request_(std::move(request)) {}

auto http3_request_machine::initial() const -> state_type {
  state_type s;
  s.socket_handle = socket_;
  s.request = request_;
  return s;
}

auto http3_request_machine::step(state_type s, const event& e) const -> step_result<state_type> {
  std::vector<operation> ops;

  switch (s.current_phase) {
    case state_type::phase::initial: {
      // Submit request
      s.stream_id = session_->submit_request(s.request);
      if (s.stream_id < 0) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::internal_error;
        s.error_message = "Failed to submit request";
        return {std::move(s), {}};
      }

      s.current_phase = state_type::phase::submitting;
      return do_send(std::move(s));
    }

    case state_type::phase::submitting:
    case state_type::phase::waiting_send: {
      if (!e.ok()) {
        s.current_phase = state_type::phase::error;
        s.error_code = http3_error_code::quic_error;
        s.error_message = "Send failed";
        return {std::move(s), {}};
      }

      // Check if stream is closed (response complete)
      if (session_->is_stream_closed(s.stream_id)) {
        auto* resp = session_->get_stream_response(s.stream_id);
        if (resp) {
          s.response = *resp;
        }
        s.current_phase = state_type::phase::done;
        return {std::move(s), {}};
      }

      return do_send(std::move(s));
    }

    case state_type::phase::waiting_recv: {
      return do_recv(std::move(s), e);
    }

    case state_type::phase::done:
    case state_type::phase::error:
      break;

    default:
      break;
  }

  return {std::move(s), std::move(ops)};
}

auto http3_request_machine::do_send(state_type s) const -> step_result<state_type> {
  std::vector<operation> ops;
  std::array<std::byte, http3_max_pktlen> buf{};

  auto nwrite = session_->write_pkt(std::span{buf});
  if (nwrite < 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = session_->error();
    return {std::move(s), {}};
  }

  if (nwrite > 0) {
    s.current_phase = state_type::phase::waiting_send;
    // Note: Need access to remote address from session
    // For now, we'll use recvmsg/sendmsg pattern
    // This is simplified - real implementation would store the buffer
    return {std::move(s), std::move(ops)};
  }

  // Wait for response
  s.current_phase = state_type::phase::waiting_recv;
  ops.push_back(operation::make_recv(s.socket_handle, recv_buffer_span(), 0, ++s.operation_id));

  return {std::move(s), std::move(ops)};
}

auto http3_request_machine::do_recv(state_type s, const event& e) const -> step_result<state_type> {
  if (!e.ok()) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = "Recv failed";
    return {std::move(s), {}};
  }

  if (e.result == 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::connection_closed;
    s.error_message = "Connection closed";
    return {std::move(s), {}};
  }

  // Process received packet
  int rv = session_->read_pkt(e.data);
  if (rv != 0) {
    s.current_phase = state_type::phase::error;
    s.error_code = http3_error_code::quic_error;
    s.error_message = session_->error();
    return {std::move(s), {}};
  }

  // Check if stream is closed (response complete)
  if (session_->is_stream_closed(s.stream_id)) {
    auto* resp = session_->get_stream_response(s.stream_id);
    if (resp) {
      s.response = *resp;
    }
    s.current_phase = state_type::phase::done;
    return {std::move(s), {}};
  }

  // Continue sending/receiving
  return do_send(std::move(s));
}

auto http3_request_machine::done(const state_type& s) const -> bool {
  return s.current_phase == state_type::phase::done || s.current_phase == state_type::phase::error;
}

} // namespace evring
