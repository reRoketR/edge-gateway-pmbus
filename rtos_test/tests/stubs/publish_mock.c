/*******************************************************************************
 * File Name:   publish_mock.c
 *
 * Description: Mock MQTT publish implementation that records calls and
 *              supports failure injection. For host-side integration tests.
 ******************************************************************************/

#include "publish_mock.h"
#include <string.h>
#include <stdlib.h>

#define PUBLISH_MOCK_MAX_RECORDS 256u

typedef struct {
    char *topic;
    char *payload;
} publish_record_t;

static publish_record_t s_records[PUBLISH_MOCK_MAX_RECORDS];
static uint32_t s_count;
static uint32_t s_fail_after;   /* 0 = never fail */
static bool     s_fail_enabled;

void publish_mock_reset(void)
{
    for (uint32_t i = 0; i < s_count && i < PUBLISH_MOCK_MAX_RECORDS; i++)
    {
        free(s_records[i].topic);
        free(s_records[i].payload);
    }
    memset(s_records, 0, sizeof(s_records));
    s_count = 0u;
    s_fail_after = 0u;
    s_fail_enabled = false;
}

void publish_mock_set_fail_after(uint32_t n)
{
    s_fail_after = n;
    s_fail_enabled = true;
}

bool publish_mock_fn(const char *topic, const char *payload, size_t len)
{
    if (s_fail_enabled && s_count >= s_fail_after)
    {
        return false;
    }

    if (s_count >= PUBLISH_MOCK_MAX_RECORDS)
    {
        return false;
    }

    s_records[s_count].topic = strdup(topic ? topic : "");
    s_records[s_count].payload = (char *)malloc(len + 1u);
    if (s_records[s_count].payload != NULL)
    {
        memcpy(s_records[s_count].payload, payload, len);
        s_records[s_count].payload[len] = '\0';
    }
    s_count++;
    return true;
}

uint32_t publish_mock_get_count(void)
{
    return s_count;
}

const char *publish_mock_get_topic(uint32_t index)
{
    if (index >= s_count) return "";
    return s_records[index].topic;
}

const char *publish_mock_get_payload(uint32_t index)
{
    if (index >= s_count) return "";
    return s_records[index].payload;
}

/* [] END OF FILE */
