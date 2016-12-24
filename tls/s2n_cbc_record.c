/*
 * Copyright 2014 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <stdint.h>
#include <sys/param.h>

#include "error/s2n_errno.h"

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "tls/s2n_crypto.h"

#include "stuffer/s2n_stuffer.h"

#include "crypto/s2n_sequence.h"
#include "crypto/s2n_cipher.h"
#include "crypto/s2n_hmac.h"

#include "utils/s2n_safety.h"
#include "utils/s2n_random.h"
#include "utils/s2n_blob.h"

int s2n_cbc_record_parse(struct s2n_connection *conn)
{
    struct s2n_blob iv;
    struct s2n_blob en;
    uint8_t content_type;
    uint16_t fragment_length;
    uint8_t ivpad[S2N_TLS_MAX_IV_LEN];

    uint8_t *sequence_number = conn->client->client_sequence_number;
    struct s2n_hmac_state *mac = &conn->client->client_record_mac;
    struct s2n_session_key *session_key = &conn->client->client_key;
    struct s2n_cipher_suite *cipher_suite = conn->client->cipher_suite;
    uint8_t *implicit_iv = conn->client->client_implicit_iv;

    if (conn->mode == S2N_CLIENT) {
        sequence_number = conn->server->server_sequence_number;
        mac = &conn->server->server_record_mac;
        session_key = &conn->server->server_key;
        cipher_suite = conn->server->cipher_suite;
        implicit_iv = conn->server->server_implicit_iv;
    }

    GUARD(s2n_record_header_parse(conn, &content_type, &fragment_length));

    /* Add the header to the HMAC */
    uint8_t *header = s2n_stuffer_raw_read(&conn->header_in, S2N_TLS_RECORD_HEADER_LENGTH);
    notnull_check(header);

    uint16_t encrypted_length = fragment_length;
    iv.data = implicit_iv;
    iv.size = cipher_suite->record_alg->cipher->io.cbc.record_iv_size;
    lte_check(cipher_suite->record_alg->cipher->io.cbc.record_iv_size, S2N_TLS_MAX_IV_LEN);

    /* For TLS >= 1.1 the IV is in the packet */
    if (conn->actual_protocol_version > S2N_TLS10) {
        GUARD(s2n_stuffer_read(&conn->in, &iv));
        gte_check(encrypted_length, iv.size);
        encrypted_length -= iv.size;
    }


    en.size = encrypted_length;
    en.data = s2n_stuffer_raw_read(&conn->in, en.size);
    notnull_check(en.data);

    uint16_t payload_length = encrypted_length;
    uint8_t mac_digest_size;
    GUARD(s2n_hmac_digest_size(mac->alg, &mac_digest_size));

    gte_check(mac_digest_size, 0);
    gte_check(payload_length, mac_digest_size);
    payload_length -= mac_digest_size;

    /* Check that we have some data to decrypt */
    ne_check(en.size, 0);

    /* ... and that we have a multiple of the block size */
    eq_check(en.size % iv.size, 0);

    /* Copy the last encrypted block to be the next IV */
    if (conn->actual_protocol_version < S2N_TLS11) {
        memcpy_check(ivpad, en.data + en.size - iv.size, iv.size);
    }

    GUARD(cipher_suite->record_alg->cipher->io.cbc.decrypt(session_key, &iv, &en, &en));

    if (conn->actual_protocol_version < S2N_TLS11) {
        memcpy_check(implicit_iv, ivpad, iv.size);
    }

    /* Subtract the padding length */
    gt_check(en.size, 0);
    payload_length -= (en.data[en.size - 1] + 1);

    /* Update the MAC */
    header[3] = (payload_length >> 8);
    header[4] = payload_length & 0xff;
    GUARD(s2n_hmac_reset(mac));
    GUARD(s2n_hmac_update(mac, sequence_number, S2N_TLS_SEQUENCE_NUM_LEN));

    if (conn->actual_protocol_version == S2N_SSLv3) {
        GUARD(s2n_hmac_update(mac, header, 1));
        GUARD(s2n_hmac_update(mac, header + 3, 2));
    } else {
        GUARD(s2n_hmac_update(mac, header, S2N_TLS_RECORD_HEADER_LENGTH));
    }

    struct s2n_blob seq = {.data = sequence_number,.size = S2N_TLS_SEQUENCE_NUM_LEN };
    GUARD(s2n_increment_sequence_number(&seq));

    /* Padding */
    if (s2n_verify_cbc(conn, mac, &en) < 0) {
        GUARD(s2n_stuffer_wipe(&conn->in));
        S2N_ERROR(S2N_ERR_BAD_MESSAGE);
    }

    /* O.k., we've successfully read and decrypted the record, now we need to align the stuffer
     * for reading the plaintext data.
     */
    GUARD(s2n_stuffer_reread(&conn->in));
    GUARD(s2n_stuffer_reread(&conn->header_in));

    /* Skip the IV, if any */
    if (conn->actual_protocol_version > S2N_TLS10) {
        GUARD(s2n_stuffer_skip_read(&conn->in, cipher_suite->record_alg->cipher->io.cbc.record_iv_size));
    }

    /* Truncate and wipe the MAC and any padding */
    GUARD(s2n_stuffer_wipe_n(&conn->in, s2n_stuffer_data_available(&conn->in) - payload_length));
    conn->in_status = PLAINTEXT;

    return 0;
}

int s2n_cbc_record_write(struct s2n_connection *conn, uint8_t content_type, struct s2n_blob *in)
{
    struct s2n_blob out, iv;
    uint8_t padding = 0;
    uint16_t block_size = 0;
    uint8_t protocol_version[S2N_TLS_PROTOCOL_VERSION_LEN];

    uint8_t *sequence_number = conn->server->server_sequence_number;
    struct s2n_hmac_state *mac = &conn->server->server_record_mac;
    struct s2n_session_key *session_key = &conn->server->server_key;
    struct s2n_cipher_suite *cipher_suite = conn->server->cipher_suite;
    uint8_t *implicit_iv = conn->server->server_implicit_iv;

    if (conn->mode == S2N_CLIENT) {
        sequence_number = conn->client->client_sequence_number;
        mac = &conn->client->client_record_mac;
        session_key = &conn->client->client_key;
        cipher_suite = conn->client->cipher_suite;
        implicit_iv = conn->client->client_implicit_iv;
    }

    if (s2n_stuffer_data_available(&conn->out)) {
        S2N_ERROR(S2N_ERR_BAD_MESSAGE);
    }

    uint8_t mac_digest_size;
    GUARD(s2n_hmac_digest_size(mac->alg, &mac_digest_size));
    gte_check(mac_digest_size, 0);

    /* Before we do anything, we need to figure out what the length of the
     * fragment is going to be. 
     */
    uint16_t data_bytes_to_take = MIN(in->size, s2n_record_max_write_payload_size(conn));

    uint16_t extra = overhead(conn);

    /* If we have padding to worry about, figure that out too */
    block_size = cipher_suite->record_alg->cipher->io.cbc.block_size;
    if (((data_bytes_to_take + extra) % block_size)) {
        padding = block_size - ((data_bytes_to_take + extra) % block_size);
    }
    /* Start the MAC with the sequence number */
    GUARD(s2n_hmac_update(mac, sequence_number, S2N_TLS_SEQUENCE_NUM_LEN));

    /* Now that we know the length, start writing the record */
    protocol_version[0] = conn->actual_protocol_version / 10;
    protocol_version[1] = conn->actual_protocol_version % 10;
    GUARD(s2n_stuffer_write_uint8(&conn->out, content_type));
    GUARD(s2n_stuffer_write_bytes(&conn->out, protocol_version, S2N_TLS_PROTOCOL_VERSION_LEN));

    /* First write a header that has the payload length, this is for the MAC */
    GUARD(s2n_stuffer_write_uint16(&conn->out, data_bytes_to_take));

    if (conn->actual_protocol_version > S2N_SSLv3) {
        GUARD(s2n_hmac_update(mac, conn->out.blob.data, S2N_TLS_RECORD_HEADER_LENGTH));
    } else {
        /* SSLv3 doesn't include the protocol version in the MAC */
        GUARD(s2n_hmac_update(mac, conn->out.blob.data, 1));
        GUARD(s2n_hmac_update(mac, conn->out.blob.data + 3, 2));
    }

    /* Rewrite the length to be the actual fragment length */
    uint16_t actual_fragment_length = data_bytes_to_take + padding + extra;
    GUARD(s2n_stuffer_wipe_n(&conn->out, 2));
    GUARD(s2n_stuffer_write_uint16(&conn->out, actual_fragment_length));

    iv.size = block_size;
    iv.data = implicit_iv;

    /* For TLS1.1/1.2; write the IV with random data */
    if (conn->actual_protocol_version > S2N_TLS10) {
        GUARD(s2n_get_public_random_data(&iv));
        GUARD(s2n_stuffer_write(&conn->out, &iv));
    }

    /* We are done with this sequence number, so we can increment it */
    struct s2n_blob seq = {.data = sequence_number,.size = S2N_TLS_SEQUENCE_NUM_LEN };
    GUARD(s2n_increment_sequence_number(&seq));

    /* Write the plaintext data */
    out.data = in->data;
    out.size = data_bytes_to_take;
    GUARD(s2n_stuffer_write(&conn->out, &out));
    GUARD(s2n_hmac_update(mac, out.data, out.size));

    /* Write the digest */
    uint8_t *digest = s2n_stuffer_raw_write(&conn->out, mac_digest_size);
    notnull_check(digest);

    GUARD(s2n_hmac_digest(mac, digest, mac_digest_size));
    GUARD(s2n_hmac_reset(mac));

    /* Include padding bytes, each with the value 'p', and
     * include an extra padding length byte, also with the value 'p'.
     */
    for (int i = 0; i <= padding; i++) {
        GUARD(s2n_stuffer_write_uint8(&conn->out, padding));
    }

    /* Rewind to rewrite/encrypt the packet */
    GUARD(s2n_stuffer_rewrite(&conn->out));

    /* Skip the header */
    GUARD(s2n_stuffer_skip_write(&conn->out, S2N_TLS_RECORD_HEADER_LENGTH));

    uint16_t encrypted_length = data_bytes_to_take + mac_digest_size;
    if (conn->actual_protocol_version > S2N_TLS10) {
        /* Leave the IV alone and unencrypted */
        GUARD(s2n_stuffer_skip_write(&conn->out, iv.size));
    }
    /* Encrypt the padding and the padding length byte too */
    encrypted_length += padding + 1;

    /* Do the encryption */
    struct s2n_blob en;
    en.size = encrypted_length;
    en.data = s2n_stuffer_raw_write(&conn->out, en.size);
    notnull_check(en.data);

    GUARD(cipher_suite->record_alg->cipher->io.cbc.encrypt(session_key, &iv, &en, &en));

    /* Copy the last encrypted block to be the next IV */
    if (conn->actual_protocol_version < S2N_TLS11) {
        gte_check(en.size, block_size);
        memcpy_check(implicit_iv, en.data + en.size - block_size, block_size);
    }

    conn->wire_bytes_out += actual_fragment_length + S2N_TLS_RECORD_HEADER_LENGTH;
    return data_bytes_to_take;
}
