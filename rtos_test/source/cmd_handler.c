/*******************************************************************************
 * File Name:   cmd_handler.c
 *
 * Description: Remote SMBus command handler — JSMN parse, JSON encode,
 *              QoS 1 dedupe cache, in-flight ID tracker.
 *
 * Related Document: agent.md §14
 *
 ******************************************************************************/

#include "cmd_handler.h"
#include "gateway_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* JSMN — header-only JSON tokeniser, included as static to avoid linker
 * symbol collisions with any other translation unit. */
#define JSMN_STATIC
#include "jsmn.h"

/*******************************************************************************
 * Constants
 ******************************************************************************/

/** Max JSMN tokens for the request JSON. Worst case:
 *  1 root object + 5 keys + 5 values + 1 array + 32 array elements = 44. */
#define JSMN_MAX_TOKENS     64u

/*******************************************************************************
 * Status string mapping
 ******************************************************************************/

const char *cmd_status_str(uint8_t status)
{
    switch (status)
    {
        /* pmbus_status_t values (0..10) */
        case PMBUS_OK:                return "OK";
        case PMBUS_ERR_NACK:          return "NACK";
        case PMBUS_ERR_TIMEOUT:       return "TIMEOUT";
        case PMBUS_ERR_ARB_LOST:      return "ARB_LOST";
        case PMBUS_ERR_BUS_FAULT:     return "BUS_FAULT";
        case PMBUS_ERR_NOT_READY:     return "NOT_READY";
        case PMBUS_ERR_RECOVERY_FAIL: return "RECOVERY_FAIL";
        case PMBUS_ERR_PEC:           return "PEC";
        case PMBUS_ERR_ARG:           return "ARG";
        case PMBUS_ERR_NOT_INIT:      return "NOT_INIT";
        case PMBUS_ERR_INIT:          return "INIT";

        /* cmd_status_t values (100+) */
        case CMD_STATUS_BAD_JSON:     return "BAD_JSON";
        case CMD_STATUS_BAD_REQUEST:  return "BAD_REQUEST";
        case CMD_STATUS_UNSUPPORTED:  return "UNSUPPORTED";
        case CMD_STATUS_QUEUE_FULL:   return "QUEUE_FULL";

        default:                      return "UNKNOWN";
    }
}

/*******************************************************************************
 * Recent-response cache (ring buffer)
 ******************************************************************************/

static cmd_response_t s_cache[CMD_CACHE_DEPTH];
static uint8_t        s_cache_head = 0;
static uint8_t        s_cache_count = 0;

/*******************************************************************************
 * In-flight ID tracker (ring buffer)
 ******************************************************************************/

static char    s_inflight[CMD_INFLIGHT_DEPTH][CMD_ID_MAX];
static uint8_t s_inflight_head = 0;
static uint8_t s_inflight_count = 0;

/*******************************************************************************
 * Initialization
 ******************************************************************************/

void cmd_handler_init(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_head  = 0;
    s_cache_count = 0;

    memset(s_inflight, 0, sizeof(s_inflight));
    s_inflight_head  = 0;
    s_inflight_count = 0;
}

/*******************************************************************************
 * JSMN helpers
 ******************************************************************************/

/** Compare a JSMN token to a NUL-terminated string. */
static bool tok_eq(const char *json, const jsmntok_t *tok, const char *s)
{
    size_t slen = strlen(s);
    if (tok->type != JSMN_STRING)     return false;
    if ((int)slen != tok->end - tok->start) return false;
    return memcmp(json + tok->start, s, slen) == 0;
}

/** Extract an integer from a JSMN primitive token. Returns false on error. */
static bool tok_to_int(const char *json, const jsmntok_t *tok, long *out)
{
    if (tok->type != JSMN_PRIMITIVE) return false;

    char buf[16];
    int len = tok->end - tok->start;
    if (len <= 0 || len >= (int)sizeof(buf)) return false;

    memcpy(buf, json + tok->start, (size_t)len);
    buf[len] = '\0';

    char *end;
    *out = strtol(buf, &end, 10);
    return (end != buf && *end == '\0');
}

/** Extract a boolean from a JSMN primitive token. */
static bool tok_to_bool(const char *json, const jsmntok_t *tok, bool *out)
{
    if (tok->type != JSMN_PRIMITIVE) return false;

    char c = json[tok->start];
    if (c == 't') { *out = true;  return true; }
    if (c == 'f') { *out = false; return true; }
    return false;
}

/** Copy a JSMN string token into a fixed buffer. Returns false on overflow. */
static bool tok_to_str(const char *json, const jsmntok_t *tok,
                       char *buf, size_t buf_sz)
{
    if (tok->type != JSMN_STRING) return false;

    int len = tok->end - tok->start;
    if (len < 0 || (size_t)len >= buf_sz) return false;

    memcpy(buf, json + tok->start, (size_t)len);
    buf[len] = '\0';
    return true;
}

/** Try to extract "id" from tokens even if overall parse is incomplete.
 *  Returns true if id was recovered. */
static bool try_recover_id(const char *json, const jsmntok_t *tokens,
                           int token_count, char id_out[CMD_ID_MAX])
{
    for (int i = 0; i < token_count - 1; i++)
    {
        if (tok_eq(json, &tokens[i], "id"))
        {
            return tok_to_str(json, &tokens[i + 1], id_out, CMD_ID_MAX);
        }
    }
    return false;
}

/*******************************************************************************
 * Parse
 ******************************************************************************/

cmd_parse_result_t cmd_handler_parse(const cmd_raw_t *raw,
                                     cmd_request_t *req,
                                     char id_out[CMD_ID_MAX])
{
    if (raw == NULL || req == NULL || id_out == NULL)
    {
        return CMD_PARSE_BAD_JSON;
    }

    id_out[0] = '\0';
    memset(req, 0, sizeof(*req));

    /* --- Tokenise --- */
    jsmn_parser parser;
    jsmntok_t tokens[JSMN_MAX_TOKENS];
    jsmn_init(&parser);

    int r = jsmn_parse(&parser, raw->payload, raw->payload_len,
                       tokens, JSMN_MAX_TOKENS);
    int token_count = (r >= 0) ? r : (int)parser.toknext;

    if (r < 1 || tokens[0].type != JSMN_OBJECT)
    {
        /* Try to recover id even from partial parse */
        if (token_count > 1)
        {
            if (try_recover_id(raw->payload, tokens, token_count, id_out))
            {
                return CMD_PARSE_BAD_JSON_WITH_ID;
            }
        }
        return CMD_PARSE_BAD_JSON;
    }

    /* --- Pre-scan for "id" so error classification is order-independent --- */
    bool have_id   = false;
    bool have_addr = false;
    bool have_pec  = false;
    bool pec_val   = g_config.i2c.pec_enabled;  /* default from config */

    /* Scan all key-value pairs for "id" first */
    {
        int scan_i = 1;
        int scan_size = tokens[0].size;
        for (int kv = 0; kv < scan_size && scan_i < r - 1; kv++)
        {
            if (tok_eq(raw->payload, &tokens[scan_i], "id"))
            {
                have_id = tok_to_str(raw->payload, &tokens[scan_i + 1],
                                     req->id, CMD_ID_MAX);
                if (have_id)
                {
                    strncpy(id_out, req->id, CMD_ID_MAX - 1u);
                    id_out[CMD_ID_MAX - 1u] = '\0';
                }
                break;
            }
            /* Skip key + value */
            scan_i += 2;
            /* If value is array/object, skip its children */
            if (tokens[scan_i - 1].type == JSMN_ARRAY ||
                tokens[scan_i - 1].type == JSMN_OBJECT)
            {
                int children = tokens[scan_i - 1].size;
                for (int ci = 0; ci < children && scan_i < r; ci++)
                {
                    scan_i++;
                }
            }
        }
    }

    int i = 1;  /* skip root object token */
    int root_size = tokens[0].size;

    for (int kv = 0; kv < root_size && i < r - 1; kv++)
    {
        jsmntok_t *key = &tokens[i];
        jsmntok_t *val = &tokens[i + 1];

        if (tok_eq(raw->payload, key, "id"))
        {
            have_id = tok_to_str(raw->payload, val, req->id, CMD_ID_MAX);
            if (have_id)
            {
                strncpy(id_out, req->id, CMD_ID_MAX - 1u);
                id_out[CMD_ID_MAX - 1u] = '\0';
            }
            i += 2;
        }
        else if (tok_eq(raw->payload, key, "addr"))
        {
            long v;
            if (!tok_to_int(raw->payload, val, &v) || v < 0 || v > 127)
            {
                return have_id ? CMD_PARSE_BAD_REQUEST
                               : CMD_PARSE_BAD_JSON;
            }
            req->addr_7bit = (uint8_t)v;
            have_addr = true;
            i += 2;
        }
        else if (tok_eq(raw->payload, key, "rd_len"))
        {
            long v;
            if (!tok_to_int(raw->payload, val, &v) || v < 0 || v > (long)CMD_MAX_READ_LEN)
            {
                return have_id ? CMD_PARSE_BAD_REQUEST
                               : CMD_PARSE_BAD_JSON;
            }
            req->read_len = (uint8_t)v;
            i += 2;
        }
        else if (tok_eq(raw->payload, key, "pec"))
        {
            if (!tok_to_bool(raw->payload, val, &pec_val))
            {
                return have_id ? CMD_PARSE_BAD_REQUEST
                               : CMD_PARSE_BAD_JSON;
            }
            have_pec = true;
            i += 2;
        }
        else if (tok_eq(raw->payload, key, "wr"))
        {
            /* Parse array of byte values */
            if (val->type != JSMN_ARRAY)
            {
                return have_id ? CMD_PARSE_BAD_REQUEST
                               : CMD_PARSE_BAD_JSON;
            }

            int arr_size = val->size;
            if (arr_size > (int)CMD_MAX_WRITE_LEN)
            {
                return have_id ? CMD_PARSE_BAD_REQUEST
                               : CMD_PARSE_BAD_JSON;
            }

            i += 2;  /* skip key + array token */
            for (int ai = 0; ai < arr_size && i < r; ai++, i++)
            {
                long bv;
                if (tok_to_int(raw->payload, &tokens[i], &bv) &&
                    bv >= 0 && bv <= 255)
                {
                    req->write_data[ai] = (uint8_t)bv;
                    req->write_len++;
                }
                else
                {
                    return have_id ? CMD_PARSE_BAD_REQUEST
                                   : CMD_PARSE_BAD_JSON;
                }
            }
        }
        else
        {
            /* Unknown key — skip key + value (and any children) */
            i += 2;
            /* If value is array/object, skip its children too */
            if (val->type == JSMN_ARRAY || val->type == JSMN_OBJECT)
            {
                int children = val->size;
                for (int ci = 0; ci < children && i < r; ci++)
                {
                    i++;
                }
            }
        }
    }

    /* Apply PEC */
    req->pec = have_pec ? pec_val : g_config.i2c.pec_enabled;

    /* --- Validate required fields --- */
    if (!have_id)
    {
        return CMD_PARSE_BAD_JSON;
    }

    if (!have_addr)
    {
        return CMD_PARSE_BAD_REQUEST;
    }

    /* --- Validate transfer shape --- */
    if (req->write_len == 0 && req->read_len == 0)
    {
        return CMD_PARSE_UNSUPPORTED;
    }

    return CMD_PARSE_OK;
}

/*******************************************************************************
 * Encode response
 ******************************************************************************/

int cmd_handler_encode_response(const cmd_response_t *resp,
                                char *buf, size_t buf_sz)
{
    if (resp == NULL || buf == NULL || buf_sz == 0)
    {
        return -1;
    }

    /* Start building JSON */
    int written = snprintf(buf, buf_sz,
        "{\"id\":\"%s\",\"addr\":%u,\"status\":\"%s\"",
        resp->id,
        (unsigned)resp->addr_7bit,
        cmd_status_str(resp->status));

    if (written < 0 || (size_t)written >= buf_sz)
    {
        return -1;
    }

    /* Add data array */
    size_t pos = (size_t)written;
    int n;

    if (resp->status == PMBUS_OK && resp->read_len > 0)
    {
        n = snprintf(buf + pos, buf_sz - pos, ",\"data\":[");
        if (n < 0 || pos + (size_t)n >= buf_sz) return -1;
        pos += (size_t)n;

        for (uint8_t i = 0; i < resp->read_len; i++)
        {
            n = snprintf(buf + pos, buf_sz - pos,
                         "%s%u", (i > 0) ? "," : "",
                         (unsigned)resp->read_data[i]);
            if (n < 0 || pos + (size_t)n >= buf_sz) return -1;
            pos += (size_t)n;
        }

        n = snprintf(buf + pos, buf_sz - pos, "]");
        if (n < 0 || pos + (size_t)n >= buf_sz) return -1;
        pos += (size_t)n;
    }

    /* Add exec_ms and close */
    n = snprintf(buf + pos, buf_sz - pos,
                 ",\"exec_ms\":%u}", (unsigned)resp->exec_ms);
    if (n < 0 || pos + (size_t)n >= buf_sz) return -1;
    pos += (size_t)n;

    return (int)pos;
}

/*******************************************************************************
 * Build error response
 ******************************************************************************/

void cmd_handler_build_error(cmd_response_t *resp,
                             const char *id, uint8_t addr,
                             cmd_status_t status)
{
    if (resp == NULL) return;

    memset(resp, 0, sizeof(*resp));

    if (id != NULL)
    {
        strncpy(resp->id, id, CMD_ID_MAX - 1u);
        resp->id[CMD_ID_MAX - 1u] = '\0';
    }

    resp->addr_7bit = addr;
    resp->status    = (uint8_t)status;
    resp->read_len  = 0;
    resp->exec_ms   = 0;
}

/*******************************************************************************
 * Recent-response cache
 ******************************************************************************/

bool cmd_cache_lookup(const char *id, cmd_response_t *resp)
{
    if (id == NULL || id[0] == '\0') return false;

    for (uint8_t i = 0; i < s_cache_count; i++)
    {
        uint8_t idx = (s_cache_head + CMD_CACHE_DEPTH - 1u - i) % CMD_CACHE_DEPTH;
        if (strncmp(s_cache[idx].id, id, CMD_ID_MAX) == 0)
        {
            if (resp != NULL)
            {
                *resp = s_cache[idx];
            }
            return true;
        }
    }
    return false;
}

void cmd_cache_put(const cmd_response_t *resp)
{
    if (resp == NULL) return;

    s_cache[s_cache_head] = *resp;
    s_cache_head = (s_cache_head + 1u) % CMD_CACHE_DEPTH;

    if (s_cache_count < CMD_CACHE_DEPTH)
    {
        s_cache_count++;
    }
}

/*******************************************************************************
 * In-flight ID tracker
 ******************************************************************************/

bool cmd_inflight_check(const char *id)
{
    if (id == NULL || id[0] == '\0') return false;

    for (uint8_t i = 0; i < s_inflight_count; i++)
    {
        uint8_t idx = (s_inflight_head + CMD_INFLIGHT_DEPTH - 1u - i)
                      % CMD_INFLIGHT_DEPTH;
        if (strncmp(s_inflight[idx], id, CMD_ID_MAX) == 0)
        {
            return true;
        }
    }
    return false;
}

bool cmd_inflight_add(const char *id)
{
    if (id == NULL || id[0] == '\0') return false;

    /* Fail-fast: no room — caller must reject the command */
    if (s_inflight_count >= CMD_INFLIGHT_DEPTH)
    {
        return false;
    }

    strncpy(s_inflight[s_inflight_head], id, CMD_ID_MAX - 1u);
    s_inflight[s_inflight_head][CMD_ID_MAX - 1u] = '\0';

    s_inflight_head = (s_inflight_head + 1u) % CMD_INFLIGHT_DEPTH;
    s_inflight_count++;

    return true;
}

void cmd_inflight_remove(const char *id)
{
    if (id == NULL || id[0] == '\0') return;

    for (uint8_t i = 0; i < s_inflight_count; i++)
    {
        uint8_t idx = (s_inflight_head + CMD_INFLIGHT_DEPTH - 1u - i)
                      % CMD_INFLIGHT_DEPTH;
        if (strncmp(s_inflight[idx], id, CMD_ID_MAX) == 0)
        {
            /* Compact: shift newer entries down to fill the gap.
             * 'i' is the reverse-iteration index (0 = newest).
             * We shift entries at positions [0..i-1] one slot older. */
            for (uint8_t j = i; j > 0; j--)
            {
                uint8_t dst = (s_inflight_head + CMD_INFLIGHT_DEPTH - 1u - j)
                              % CMD_INFLIGHT_DEPTH;
                uint8_t src = (s_inflight_head + CMD_INFLIGHT_DEPTH - j)
                              % CMD_INFLIGHT_DEPTH;
                memcpy(s_inflight[dst], s_inflight[src], CMD_ID_MAX);
            }

            /* Retract head and decrement count */
            s_inflight_head = (s_inflight_head + CMD_INFLIGHT_DEPTH - 1u)
                              % CMD_INFLIGHT_DEPTH;
            s_inflight[s_inflight_head][0] = '\0';
            if (s_inflight_count > 0)
            {
                s_inflight_count--;
            }
            return;
        }
    }
}
