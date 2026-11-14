#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Volcengine "end-to-end realtime voice" O2.0 binary framing.
 * Header (4 bytes, big-endian payloads):
 *   [0] (version << 4) | header_size_in_32bit_words
 *   [1] (message_type << 4) | message_flags
 *   [2] (serialization << 4) | compression
 *   [3] reserved
 * Payload: [seq?] [event_id?] [session_id_len+id?] payload_len + payload
 */

#define O2_PROTO_VERSION 1
#define O2_HEADER_WORDS 1

#define O2_MSG_CLIENT_FULL 0x01
#define O2_MSG_CLIENT_AUDIO 0x02
#define O2_MSG_SERVER_FULL 0x09
#define O2_MSG_SERVER_ACK 0x0B
#define O2_MSG_SERVER_ERROR 0x0F

#define O2_FLAG_SEQ_PRESENT 0x03
#define O2_FLAG_EVENT 0x04

#define O2_SERIAL_NONE 0x00
#define O2_SERIAL_JSON 0x01
#define O2_COMP_NONE 0x00
#define O2_COMP_GZIP 0x01

#define O2_EVT_START_CONNECTION 1u
#define O2_EVT_FINISH_CONNECTION 2u
#define O2_EVT_START_SESSION 100u
#define O2_EVT_FINISH_SESSION 102u
#define O2_EVT_TASK_REQUEST 200u
#define O2_EVT_CHAT_TTS_TEXT 500u
#define O2_EVT_CHAT_TEXT_QUERY 501u

#define O2_EVT_CONN_STARTED 50u
#define O2_EVT_CONN_FAILED 51u
#define O2_EVT_CONN_FINISHED 52u
#define O2_EVT_SESSION_STARTED 150u
#define O2_EVT_SESSION_FINISHED 152u
#define O2_EVT_SESSION_FAILED 153u
#define O2_EVT_USAGE 154u
#define O2_EVT_TTS_SENTENCE_START 350u
#define O2_EVT_TTS_SENTENCE_END 351u
#define O2_EVT_TTS_RESPONSE 352u
#define O2_EVT_TTS_ENDED 359u
#define O2_EVT_ASR_INFO 450u
#define O2_EVT_ASR_RESPONSE 451u
#define O2_EVT_ASR_ENDED 459u
#define O2_EVT_CHAT_RESPONSE 550u
#define O2_EVT_CHAT_QUERY_CONFIRMED 553u
#define O2_EVT_CHAT_ENDED 559u

typedef struct {
    uint8_t message_type;
    uint8_t flags;
    uint8_t serialization;
    uint8_t compression;
    uint32_t event_id;      /* error code for O2_MSG_SERVER_ERROR */
    const uint8_t *payload; /* points into the input buffer */
    size_t payload_len;
} o2_frame_t;

esp_err_t o2_build_start_connection(uint8_t *out, size_t cap, size_t *out_len);
esp_err_t o2_build_start_session(uint8_t *out, size_t cap, size_t *out_len,
                                 const char *session_id, const char *config);
esp_err_t o2_build_finish_session(uint8_t *out, size_t cap, size_t *out_len,
                                  const char *session_id);
esp_err_t o2_build_text_query(uint8_t *out, size_t cap, size_t *out_len,
                              const char *session_id, const char *text);
esp_err_t o2_build_audio(uint8_t *out, size_t cap, size_t *out_len,
                         const char *session_id, const int16_t *pcm,
                         size_t samples);

/* Parses one frame. Returns ESP_ERR_INVALID_SIZE when more data is needed,
 * ESP_ERR_INVALID_RESPONSE for a malformed header, ESP_OK with *consumed set. */
esp_err_t o2_parse_frame(const uint8_t *data, size_t len, o2_frame_t *out,
                         size_t *consumed);

/* gzip/zlib/raw-deflate payload -> out. Returns decompressed length or -1. */
int o2_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
