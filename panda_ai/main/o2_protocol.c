#include "o2_protocol.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "o2_proto";

static void put_u32(uint8_t *buf, uint32_t v)
{
    buf[0] = (uint8_t)(v >> 24);
    buf[1] = (uint8_t)(v >> 16);
    buf[2] = (uint8_t)(v >> 8);
    buf[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static esp_err_t build_frame(uint8_t *out, size_t cap, size_t *out_len,
                             uint8_t msg_type, uint8_t flags, uint8_t serial,
                             uint32_t event_id, const char *session_id,
                             const uint8_t *payload, size_t payload_len)
{
    if (!out || !out_len || cap < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t sid_len = (session_id && session_id[0]) ? strlen(session_id) : 0;
    size_t need = 4 + ((flags & O2_FLAG_EVENT) ? 4 : 0) +
                  (sid_len ? 4 + sid_len : 0) + 4 + payload_len;
    if (need > cap) {
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    out[off++] = (O2_PROTO_VERSION << 4) | O2_HEADER_WORDS;
    out[off++] = (uint8_t)((msg_type << 4) | flags);
    out[off++] = (uint8_t)((serial << 4) | O2_COMP_NONE);
    out[off++] = 0;
    if (flags & O2_FLAG_EVENT) {
        put_u32(out + off, event_id);
        off += 4;
    }
    if (sid_len) {
        put_u32(out + off, (uint32_t)sid_len);
        off += 4;
        memcpy(out + off, session_id, sid_len);
        off += sid_len;
    }
    put_u32(out + off, (uint32_t)payload_len);
    off += 4;
    if (payload_len) {
        memcpy(out + off, payload, payload_len);
        off += payload_len;
    }
    *out_len = off;
    return ESP_OK;
}

static esp_err_t build_json(uint8_t *out, size_t cap, size_t *out_len,
                            uint32_t event_id, const char *session_id,
                            const char *json)
{
    const char *body = json ? json : "{}";
    return build_frame(out, cap, out_len, O2_MSG_CLIENT_FULL, O2_FLAG_EVENT,
                       O2_SERIAL_JSON, event_id, session_id,
                       (const uint8_t *)body, strlen(body));
}

esp_err_t o2_build_start_connection(uint8_t *out, size_t cap, size_t *out_len)
{
    return build_json(out, cap, out_len, O2_EVT_START_CONNECTION, NULL, "{}");
}

esp_err_t o2_build_start_session(uint8_t *out, size_t cap, size_t *out_len,
                                 const char *session_id, const char *config)
{
    return build_json(out, cap, out_len, O2_EVT_START_SESSION, session_id, config);
}

esp_err_t o2_build_finish_session(uint8_t *out, size_t cap, size_t *out_len,
                                  const char *session_id)
{
    return build_json(out, cap, out_len, O2_EVT_FINISH_SESSION, session_id, "{}");
}

esp_err_t o2_build_text_query(uint8_t *out, size_t cap, size_t *out_len,
                              const char *session_id, const char *text)
{
    if (!text) {
        text = "";
    }
    char esc[512];
    size_t o = 0;
    for (const char *p = text; *p && o + 6 < sizeof(esc); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            esc[o++] = '\\';
            esc[o++] = (char)c;
        } else if (c == '\n') {
            esc[o++] = '\\';
            esc[o++] = 'n';
        } else if (c == '\r') {
            esc[o++] = '\\';
            esc[o++] = 'r';
        } else if (c == '\t') {
            esc[o++] = '\\';
            esc[o++] = 't';
        } else if (c < 0x20) {
            o += (size_t)snprintf(esc + o, sizeof(esc) - o, "\\u%04x", c);
        } else {
            esc[o++] = (char)c;
        }
    }
    esc[o] = '\0';

    char json[600];
    int n = snprintf(json, sizeof(json), "{\"content\":\"%s\"}", esc);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return ESP_ERR_NO_MEM;
    }
    return build_json(out, cap, out_len, O2_EVT_CHAT_TEXT_QUERY, session_id, json);
}

esp_err_t o2_build_audio(uint8_t *out, size_t cap, size_t *out_len,
                         const char *session_id, const int16_t *pcm,
                         size_t samples)
{
    return build_frame(out, cap, out_len, O2_MSG_CLIENT_AUDIO, O2_FLAG_EVENT,
                       O2_SERIAL_NONE, O2_EVT_TASK_REQUEST, session_id,
                       (const uint8_t *)pcm, samples * sizeof(int16_t));
}

esp_err_t o2_parse_frame(const uint8_t *data, size_t len, o2_frame_t *out,
                         size_t *consumed)
{
    if (!data || !out || !consumed) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t version = data[0] >> 4;
    uint8_t hdr_words = data[0] & 0x0F;
    if (version != O2_PROTO_VERSION || hdr_words < 1 || hdr_words > 4) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t off = (size_t)hdr_words * 4;
    if (len < off) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out, 0, sizeof(*out));
    out->message_type = data[1] >> 4;
    out->flags = data[1] & 0x0F;
    out->serialization = data[2] >> 4;
    out->compression = data[2] & 0x0F;

    if (out->message_type == O2_MSG_SERVER_ERROR) {
        /* code(4) + payload_len(4) + message */
        if (off + 8 > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        out->event_id = get_u32(data + off);
        uint32_t plen = get_u32(data + off + 4);
        off += 8;
        if (off + plen > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        out->payload = data + off;
        out->payload_len = plen;
        *consumed = off + plen;
        return ESP_OK;
    }

    if ((out->message_type == O2_MSG_SERVER_FULL ||
         out->message_type == O2_MSG_SERVER_ACK) &&
        (out->flags & O2_FLAG_SEQ_PRESENT)) {
        if (off + 4 > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        off += 4;
    }
    if (out->flags & O2_FLAG_EVENT) {
        if (off + 4 > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        out->event_id = get_u32(data + off);
        off += 4;
    }
    if (out->message_type == O2_MSG_SERVER_FULL ||
        out->message_type == O2_MSG_SERVER_ACK) {
        if (off + 4 > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t sid_len = get_u32(data + off);
        off += 4;
        if (sid_len > 64) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (off + sid_len > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        off += sid_len;
    }
    if (off + 4 > len) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t plen = get_u32(data + off);
    off += 4;
    if (off + plen > len) {
        return ESP_ERR_INVALID_SIZE;
    }
    out->payload = data + off;
    out->payload_len = plen;
    *consumed = off + plen;
    return ESP_OK;
}

int o2_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    if (!in || !out || in_len < 2 || out_cap == 0) {
        return -1;
    }
    size_t off = 0;
    int flags = 0;

    if (in[0] == 0x1F && in[1] == 0x8B) {
        /* gzip wrapper: skip the header, feed the raw deflate stream. */
        if (in_len < 10) {
            return -1;
        }
        off = 10;
        uint8_t flg = in[3];
        if (flg & 0x04) {
            if (off + 2 > in_len) {
                return -1;
            }
            uint16_t xlen = (uint16_t)in[off] | ((uint16_t)in[off + 1] << 8);
            off += 2 + xlen;
        }
        if (flg & 0x08) {
            while (off < in_len && in[off] != 0) off++;
            off++;
        }
        if (flg & 0x10) {
            while (off < in_len && in[off] != 0) off++;
            off++;
        }
        if (flg & 0x02) {
            off += 2;
        }
        if (off >= in_len) {
            return -1;
        }
    } else if (in[0] == 0x78) {
        /* zlib header */
        flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    }

    size_t n = tinfl_decompress_mem_to_mem(out, out_cap, in + off, in_len - off, flags);
    if (n == (size_t)-1) {
        ESP_LOGW(TAG, "inflate failed (%u bytes, header %02x %02x)",
                 (unsigned)in_len, in[0], in[1]);
        return -1;
    }
    return (int)n;
}
