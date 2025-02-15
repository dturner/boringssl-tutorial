/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.]
 */
/* ====================================================================
 * Copyright (c) 1998-2002 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.openssl.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com). */

#include <openssl/ssl.h>

#include <assert.h>
#include <string.h>

#include <openssl/bytestring.h>
#include <openssl/err.h>
#include <openssl/mem.h>

#include "internal.h"
#include "../crypto/internal.h"


namespace bssl {

// kMaxEmptyRecords is the number of consecutive, empty records that will be
// processed. Without this limit an attacker could send empty records at a
// faster rate than we can process and cause record processing to loop
// forever.
static const uint8_t kMaxEmptyRecords = 32;

// kMaxEarlyDataSkipped is the maximum number of rejected early data bytes that
// will be skipped. Without this limit an attacker could send records at a
// faster rate than we can process and cause trial decryption to loop forever.
// This value should be slightly above kMaxEarlyDataAccepted, which is measured
// in plaintext.
static const size_t kMaxEarlyDataSkipped = 16384;

// kMaxWarningAlerts is the number of consecutive warning alerts that will be
// processed.
static const uint8_t kMaxWarningAlerts = 4;

// ssl_needs_record_splitting returns one if |ssl|'s current outgoing cipher
// state needs record-splitting and zero otherwise.
static int ssl_needs_record_splitting(const SSL *ssl) {
#if !defined(BORINGSSL_UNSAFE_FUZZER_MODE)
  return !ssl->s3->aead_write_ctx->is_null_cipher() &&
         ssl->s3->aead_write_ctx->version() < TLS1_1_VERSION &&
         (ssl->mode & SSL_MODE_CBC_RECORD_SPLITTING) != 0 &&
         SSL_CIPHER_is_block_cipher(ssl->s3->aead_write_ctx->cipher());
#else
  return 0;
#endif
}

int ssl_record_sequence_update(uint8_t *seq, size_t seq_len) {
  for (size_t i = seq_len - 1; i < seq_len; i--) {
    ++seq[i];
    if (seq[i] != 0) {
      return 1;
    }
  }
  OPENSSL_PUT_ERROR(SSL, ERR_R_OVERFLOW);
  return 0;
}

size_t ssl_record_prefix_len(const SSL *ssl) {
  size_t header_len;
  if (SSL_is_dtls(ssl)) {
    header_len = DTLS1_RT_HEADER_LENGTH;
  } else {
    header_len = SSL3_RT_HEADER_LENGTH;
  }

  return header_len + ssl->s3->aead_read_ctx->ExplicitNonceLen();
}

size_t ssl_seal_align_prefix_len(const SSL *ssl) {
  if (SSL_is_dtls(ssl)) {
    return DTLS1_RT_HEADER_LENGTH + ssl->s3->aead_write_ctx->ExplicitNonceLen();
  }

  size_t ret =
      SSL3_RT_HEADER_LENGTH + ssl->s3->aead_write_ctx->ExplicitNonceLen();
  if (ssl_needs_record_splitting(ssl)) {
    ret += SSL3_RT_HEADER_LENGTH;
    ret += ssl_cipher_get_record_split_len(ssl->s3->aead_write_ctx->cipher());
  }
  return ret;
}

enum ssl_open_record_t tls_open_record(SSL *ssl, uint8_t *out_type, CBS *out,
                                       size_t *out_consumed, uint8_t *out_alert,
                                       uint8_t *in, size_t in_len) {
  *out_consumed = 0;

  CBS cbs;
  CBS_init(&cbs, in, in_len);

  // Decode the record header.
  uint8_t type;
  uint16_t version, ciphertext_len;
  if (!CBS_get_u8(&cbs, &type) ||
      !CBS_get_u16(&cbs, &version) ||
      !CBS_get_u16(&cbs, &ciphertext_len)) {
    *out_consumed = SSL3_RT_HEADER_LENGTH;
    return ssl_open_record_partial;
  }

  int version_ok;
  if (ssl->s3->aead_read_ctx->is_null_cipher()) {
    // Only check the first byte. Enforcing beyond that can prevent decoding
    // version negotiation failure alerts.
    version_ok = (version >> 8) == SSL3_VERSION_MAJOR;
  } else if (ssl3_protocol_version(ssl) < TLS1_3_VERSION) {
    // Earlier versions of TLS switch the record version.
    version_ok = version == ssl->version;
  } else {
    // Starting TLS 1.3, the version field is frozen at {3, 1}.
    version_ok = version == TLS1_VERSION;
  }

  if (!version_ok) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_WRONG_VERSION_NUMBER);
    *out_alert = SSL_AD_PROTOCOL_VERSION;
    return ssl_open_record_error;
  }

  // Check the ciphertext length.
  if (ciphertext_len > SSL3_RT_MAX_ENCRYPTED_LENGTH) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_ENCRYPTED_LENGTH_TOO_LONG);
    *out_alert = SSL_AD_RECORD_OVERFLOW;
    return ssl_open_record_error;
  }

  // Extract the body.
  CBS body;
  if (!CBS_get_bytes(&cbs, &body, ciphertext_len)) {
    *out_consumed = SSL3_RT_HEADER_LENGTH + (size_t)ciphertext_len;
    return ssl_open_record_partial;
  }

  ssl_do_msg_callback(ssl, 0 /* read */, SSL3_RT_HEADER, in,
                      SSL3_RT_HEADER_LENGTH);

  *out_consumed = in_len - CBS_len(&cbs);

  // Skip early data received when expecting a second ClientHello if we rejected
  // 0RTT.
  if (ssl->s3->skip_early_data &&
      ssl->s3->aead_read_ctx->is_null_cipher() &&
      type == SSL3_RT_APPLICATION_DATA) {
    goto skipped_data;
  }

  // Decrypt the body in-place.
  if (!ssl->s3->aead_read_ctx->Open(out, type, version, ssl->s3->read_sequence,
                                    (uint8_t *)CBS_data(&body),
                                    CBS_len(&body))) {
    if (ssl->s3->skip_early_data && !ssl->s3->aead_read_ctx->is_null_cipher()) {
      ERR_clear_error();
      goto skipped_data;
    }

    OPENSSL_PUT_ERROR(SSL, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC);
    *out_alert = SSL_AD_BAD_RECORD_MAC;
    return ssl_open_record_error;
  }

  ssl->s3->skip_early_data = false;

  if (!ssl_record_sequence_update(ssl->s3->read_sequence, 8)) {
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return ssl_open_record_error;
  }

  // TLS 1.3 hides the record type inside the encrypted data.
  if (!ssl->s3->aead_read_ctx->is_null_cipher() &&
      ssl->s3->aead_read_ctx->version() >= TLS1_3_VERSION) {
    // The outer record type is always application_data.
    if (type != SSL3_RT_APPLICATION_DATA) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_INVALID_OUTER_RECORD_TYPE);
      *out_alert = SSL_AD_DECODE_ERROR;
      return ssl_open_record_error;
    }

    do {
      if (!CBS_get_last_u8(out, &type)) {
        OPENSSL_PUT_ERROR(SSL, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC);
        *out_alert = SSL_AD_DECRYPT_ERROR;
        return ssl_open_record_error;
      }
    } while (type == 0);
  }

  // Check the plaintext length.
  if (CBS_len(out) > SSL3_RT_MAX_PLAIN_LENGTH) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_DATA_LENGTH_TOO_LONG);
    *out_alert = SSL_AD_RECORD_OVERFLOW;
    return ssl_open_record_error;
  }

  // Limit the number of consecutive empty records.
  if (CBS_len(out) == 0) {
    ssl->s3->empty_record_count++;
    if (ssl->s3->empty_record_count > kMaxEmptyRecords) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_TOO_MANY_EMPTY_FRAGMENTS);
      *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
      return ssl_open_record_error;
    }
    // Apart from the limit, empty records are returned up to the caller. This
    // allows the caller to reject records of the wrong type.
  } else {
    ssl->s3->empty_record_count = 0;
  }

  if (type == SSL3_RT_ALERT) {
    // Return end_of_early_data alerts as-is for the caller to process.
    if (CBS_len(out) == 2 &&
        CBS_data(out)[0] == SSL3_AL_WARNING &&
        CBS_data(out)[1] == TLS1_AD_END_OF_EARLY_DATA) {
      *out_type = type;
      return ssl_open_record_success;
    }

    return ssl_process_alert(ssl, out_alert, CBS_data(out), CBS_len(out));
  }

  ssl->s3->warning_alert_count = 0;

  *out_type = type;
  return ssl_open_record_success;

skipped_data:
  ssl->s3->early_data_skipped += *out_consumed;
  if (ssl->s3->early_data_skipped < *out_consumed) {
    ssl->s3->early_data_skipped = kMaxEarlyDataSkipped + 1;
  }

  if (ssl->s3->early_data_skipped > kMaxEarlyDataSkipped) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_TOO_MUCH_SKIPPED_EARLY_DATA);
    *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
    return ssl_open_record_error;
  }

  return ssl_open_record_discard;
}

static int do_seal_record(SSL *ssl, uint8_t *out_prefix, uint8_t *out,
                          uint8_t *out_suffix, uint8_t type, const uint8_t *in,
                          const size_t in_len) {
  uint8_t *extra_in = NULL;
  size_t extra_in_len = 0;
  if (!ssl->s3->aead_write_ctx->is_null_cipher() &&
      ssl->s3->aead_write_ctx->version() >= TLS1_3_VERSION) {
    // TLS 1.3 hides the actual record type inside the encrypted data.
    extra_in = &type;
    extra_in_len = 1;
  }

  size_t suffix_len;
  if (!ssl->s3->aead_write_ctx->SuffixLen(&suffix_len, in_len, extra_in_len)) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_RECORD_TOO_LARGE);
    return 0;
  }
  size_t ciphertext_len =
      ssl->s3->aead_write_ctx->ExplicitNonceLen() + suffix_len;
  if (ciphertext_len + in_len < ciphertext_len) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_RECORD_TOO_LARGE);
    return 0;
  }
  ciphertext_len += in_len;

  assert(in == out || !buffers_alias(in, in_len, out, in_len));
  assert(!buffers_alias(in, in_len, out_prefix, ssl_record_prefix_len(ssl)));
  assert(!buffers_alias(in, in_len, out_suffix, suffix_len));

  if (extra_in_len) {
    out_prefix[0] = SSL3_RT_APPLICATION_DATA;
  } else {
    out_prefix[0] = type;
  }

  // The TLS record-layer version number is meaningless and, starting in
  // TLS 1.3, is frozen at TLS 1.0. But for historical reasons, SSL 3.0
  // ClientHellos should use SSL 3.0 and pre-TLS-1.3 expects the version
  // to change after version negotiation.
  uint16_t wire_version = TLS1_VERSION;
  if (ssl->s3->hs != NULL && ssl->s3->hs->max_version == SSL3_VERSION) {
    wire_version = SSL3_VERSION;
  }
  if (ssl->s3->have_version && ssl3_protocol_version(ssl) < TLS1_3_VERSION) {
    wire_version = ssl->version;
  }
  out_prefix[1] = wire_version >> 8;
  out_prefix[2] = wire_version & 0xff;
  out_prefix[3] = ciphertext_len >> 8;
  out_prefix[4] = ciphertext_len & 0xff;

  if (!ssl->s3->aead_write_ctx->SealScatter(out_prefix + SSL3_RT_HEADER_LENGTH,
                                            out, out_suffix, type, wire_version,
                                            ssl->s3->write_sequence, in, in_len,
                                            extra_in, extra_in_len) ||
      !ssl_record_sequence_update(ssl->s3->write_sequence, 8)) {
    return 0;
  }

  ssl_do_msg_callback(ssl, 1 /* write */, SSL3_RT_HEADER, out_prefix,
                      SSL3_RT_HEADER_LENGTH);
  return 1;
}

static size_t tls_seal_scatter_prefix_len(const SSL *ssl, uint8_t type,
                                   size_t in_len) {
  size_t ret = SSL3_RT_HEADER_LENGTH;
  if (type == SSL3_RT_APPLICATION_DATA && in_len > 1 &&
      ssl_needs_record_splitting(ssl)) {
    // In the case of record splitting, the 1-byte record (of the 1/n-1 split)
    // will be placed in the prefix, as will four of the five bytes of the
    // record header for the main record. The final byte will replace the first
    // byte of the plaintext that was used in the small record.
    ret += ssl_cipher_get_record_split_len(ssl->s3->aead_write_ctx->cipher());
    ret += SSL3_RT_HEADER_LENGTH - 1;
  } else {
    ret += ssl->s3->aead_write_ctx->ExplicitNonceLen();
  }
  return ret;
}

static bool tls_seal_scatter_suffix_len(const SSL *ssl, size_t *out_suffix_len,
                                        uint8_t type, size_t in_len) {
  size_t extra_in_len = 0;
  if (!ssl->s3->aead_write_ctx->is_null_cipher() &&
      ssl->s3->aead_write_ctx->version() >= TLS1_3_VERSION) {
    // TLS 1.3 adds an extra byte for encrypted record type.
    extra_in_len = 1;
  }
  if (type == SSL3_RT_APPLICATION_DATA &&  // clang-format off
      in_len > 1 &&
      ssl_needs_record_splitting(ssl)) {
    // With record splitting enabled, the first byte gets sealed into a separate
    // record which is written into the prefix.
    in_len -= 1;
  }
  return ssl->s3->aead_write_ctx->SuffixLen(out_suffix_len, in_len, extra_in_len);
}

// tls_seal_scatter_record seals a new record of type |type| and body |in| and
// splits it between |out_prefix|, |out|, and |out_suffix|. Exactly
// |tls_seal_scatter_prefix_len| bytes are written to |out_prefix|, |in_len|
// bytes to |out|, and |tls_seal_scatter_suffix_len| bytes to |out_suffix|. It
// returns one on success and zero on error. If enabled,
// |tls_seal_scatter_record| implements TLS 1.0 CBC 1/n-1 record splitting and
// may write two records concatenated.
static int tls_seal_scatter_record(SSL *ssl, uint8_t *out_prefix, uint8_t *out,
                            uint8_t *out_suffix, uint8_t type,
                            const uint8_t *in, size_t in_len) {
  if (type == SSL3_RT_APPLICATION_DATA && in_len > 1 &&
      ssl_needs_record_splitting(ssl)) {
    assert(ssl->s3->aead_write_ctx->ExplicitNonceLen() == 0);
    const size_t prefix_len = SSL3_RT_HEADER_LENGTH;

    // Write the 1-byte fragment into |out_prefix|.
    uint8_t *split_body = out_prefix + prefix_len;
    uint8_t *split_suffix = split_body + 1;

    if (!do_seal_record(ssl, out_prefix, split_body, split_suffix, type, in,
                        1)) {
      return 0;
    }

    size_t split_record_suffix_len;
    if (!ssl->s3->aead_write_ctx->SuffixLen(&split_record_suffix_len, 1, 0)) {
      assert(false);
      return 0;
    }
    const size_t split_record_len = prefix_len + 1 + split_record_suffix_len;
    assert(SSL3_RT_HEADER_LENGTH + ssl_cipher_get_record_split_len(
                                       ssl->s3->aead_write_ctx->cipher()) ==
           split_record_len);

    // Write the n-1-byte fragment. The header gets split between |out_prefix|
    // (header[:-1]) and |out| (header[-1:]).
    uint8_t tmp_prefix[SSL3_RT_HEADER_LENGTH];
    if (!do_seal_record(ssl, tmp_prefix, out + 1, out_suffix, type, in + 1,
                        in_len - 1)) {
      return 0;
    }
    assert(tls_seal_scatter_prefix_len(ssl, type, in_len) ==
           split_record_len + SSL3_RT_HEADER_LENGTH - 1);
    OPENSSL_memcpy(out_prefix + split_record_len, tmp_prefix,
                   SSL3_RT_HEADER_LENGTH - 1);
    OPENSSL_memcpy(out, tmp_prefix + SSL3_RT_HEADER_LENGTH - 1, 1);
    return 1;
  }

  return do_seal_record(ssl, out_prefix, out, out_suffix, type, in, in_len);
}

int tls_seal_record(SSL *ssl, uint8_t *out, size_t *out_len, size_t max_out_len,
                    uint8_t type, const uint8_t *in, size_t in_len) {
  if (buffers_alias(in, in_len, out, max_out_len)) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_OUTPUT_ALIASES_INPUT);
    return 0;
  }

  const size_t prefix_len = tls_seal_scatter_prefix_len(ssl, type, in_len);
  size_t suffix_len;
  if (!tls_seal_scatter_suffix_len(ssl, &suffix_len, type, in_len)) {
    return false;
  }
  if (in_len + prefix_len < in_len ||
      prefix_len + in_len + suffix_len < prefix_len + in_len) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_RECORD_TOO_LARGE);
    return 0;
  }
  if (max_out_len < in_len + prefix_len + suffix_len) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_BUFFER_TOO_SMALL);
    return 0;
  }

  uint8_t *prefix = out;
  uint8_t *body = out + prefix_len;
  uint8_t *suffix = body + in_len;
  if (!tls_seal_scatter_record(ssl, prefix, body, suffix, type, in, in_len)) {
    return 0;
  }

  *out_len = prefix_len + in_len + suffix_len;
  return 1;
}

enum ssl_open_record_t ssl_process_alert(SSL *ssl, uint8_t *out_alert,
                                         const uint8_t *in, size_t in_len) {
  // Alerts records may not contain fragmented or multiple alerts.
  if (in_len != 2) {
    *out_alert = SSL_AD_DECODE_ERROR;
    OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ALERT);
    return ssl_open_record_error;
  }

  ssl_do_msg_callback(ssl, 0 /* read */, SSL3_RT_ALERT, in, in_len);

  const uint8_t alert_level = in[0];
  const uint8_t alert_descr = in[1];

  uint16_t alert = (alert_level << 8) | alert_descr;
  ssl_do_info_callback(ssl, SSL_CB_READ_ALERT, alert);

  if (alert_level == SSL3_AL_WARNING) {
    if (alert_descr == SSL_AD_CLOSE_NOTIFY) {
      ssl->s3->recv_shutdown = ssl_shutdown_close_notify;
      return ssl_open_record_close_notify;
    }

    // Warning alerts do not exist in TLS 1.3.
    if (ssl->s3->have_version &&
        ssl3_protocol_version(ssl) >= TLS1_3_VERSION) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ALERT);
      return ssl_open_record_error;
    }

    ssl->s3->warning_alert_count++;
    if (ssl->s3->warning_alert_count > kMaxWarningAlerts) {
      *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
      OPENSSL_PUT_ERROR(SSL, SSL_R_TOO_MANY_WARNING_ALERTS);
      return ssl_open_record_error;
    }
    return ssl_open_record_discard;
  }

  if (alert_level == SSL3_AL_FATAL) {
    ssl->s3->recv_shutdown = ssl_shutdown_fatal_alert;

    char tmp[16];
    OPENSSL_PUT_ERROR(SSL, SSL_AD_REASON_OFFSET + alert_descr);
    BIO_snprintf(tmp, sizeof(tmp), "%d", alert_descr);
    ERR_add_error_data(2, "SSL alert number ", tmp);
    return ssl_open_record_fatal_alert;
  }

  *out_alert = SSL_AD_ILLEGAL_PARAMETER;
  OPENSSL_PUT_ERROR(SSL, SSL_R_UNKNOWN_ALERT_TYPE);
  return ssl_open_record_error;
}

OpenRecordResult OpenRecord(SSL *ssl, Span<uint8_t> *out,
                            size_t *out_record_len, uint8_t *out_alert,
                            const Span<uint8_t> in) {
  // This API is a work in progress and currently only works for TLS 1.2 servers
  // and below.
  if (SSL_in_init(ssl) ||
      SSL_is_dtls(ssl) ||
      ssl3_protocol_version(ssl) > TLS1_2_VERSION) {
    assert(false);
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return OpenRecordResult::kError;
  }

  CBS plaintext;
  uint8_t type;
  const ssl_open_record_t result = tls_open_record(
      ssl, &type, &plaintext, out_record_len, out_alert, in.data(), in.size());

  switch (result) {
    case ssl_open_record_success:
      if (type != SSL3_RT_APPLICATION_DATA && type != SSL3_RT_ALERT) {
        *out_alert = SSL_AD_UNEXPECTED_MESSAGE;
        return OpenRecordResult::kError;
      }
      *out = MakeSpan(
          const_cast<uint8_t*>(CBS_data(&plaintext)), CBS_len(&plaintext));
      return OpenRecordResult::kOK;
    case ssl_open_record_discard:
      return OpenRecordResult::kDiscard;
    case ssl_open_record_partial:
      return OpenRecordResult::kIncompleteRecord;
    case ssl_open_record_close_notify:
      return OpenRecordResult::kAlertCloseNotify;
    case ssl_open_record_fatal_alert:
      return OpenRecordResult::kAlertFatal;
    case ssl_open_record_error:
      return OpenRecordResult::kError;
  }
  assert(false);
  return OpenRecordResult::kError;
}

size_t SealRecordPrefixLen(const SSL *ssl, const size_t record_len) {
  return tls_seal_scatter_prefix_len(ssl, SSL3_RT_APPLICATION_DATA, record_len);
}

size_t SealRecordSuffixLen(const SSL *ssl, const size_t plaintext_len) {
  assert(plaintext_len <= SSL3_RT_MAX_PLAIN_LENGTH);
  size_t suffix_len;
  if (!tls_seal_scatter_suffix_len(ssl, &suffix_len, SSL3_RT_APPLICATION_DATA,
                                   plaintext_len)) {
    assert(false);
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return 0;
  }
  assert(suffix_len <= SSL3_RT_MAX_ENCRYPTED_OVERHEAD);
  return suffix_len;
}

bool SealRecord(SSL *ssl, const Span<uint8_t> out_prefix,
                const Span<uint8_t> out, Span<uint8_t> out_suffix,
                const Span<const uint8_t> in) {
  // This API is a work in progress and currently only works for TLS 1.2 servers
  // and below.
  if (SSL_in_init(ssl) ||
      SSL_is_dtls(ssl) ||
      ssl3_protocol_version(ssl) > TLS1_2_VERSION) {
    assert(false);
    OPENSSL_PUT_ERROR(SSL, ERR_R_INTERNAL_ERROR);
    return false;
  }

  if (out_prefix.size() != SealRecordPrefixLen(ssl, in.size()) ||
      out.size() != in.size() ||
      out_suffix.size() != SealRecordSuffixLen(ssl, in.size())) {
    OPENSSL_PUT_ERROR(SSL, SSL_R_BUFFER_TOO_SMALL);
    return false;
  }
  return tls_seal_scatter_record(ssl, out_prefix.data(), out.data(),
                                 out_suffix.data(), SSL3_RT_APPLICATION_DATA,
                                 in.data(), in.size());
}

}  // namespace bssl

using namespace bssl;

size_t SSL_max_seal_overhead(const SSL *ssl) {
  if (SSL_is_dtls(ssl)) {
    return dtls_max_seal_overhead(ssl, dtls1_use_current_epoch);
  }

  size_t ret = SSL3_RT_HEADER_LENGTH;
  ret += ssl->s3->aead_write_ctx->MaxOverhead();
  // TLS 1.3 needs an extra byte for the encrypted record type.
  if (!ssl->s3->aead_write_ctx->is_null_cipher() &&
      ssl->s3->aead_write_ctx->version() >= TLS1_3_VERSION) {
    ret += 1;
  }
  if (ssl_needs_record_splitting(ssl)) {
    ret *= 2;
  }
  return ret;
}
