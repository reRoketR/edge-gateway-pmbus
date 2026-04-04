/* Mock publish callback for integration tests. */
#ifndef PUBLISH_MOCK_H
#define PUBLISH_MOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef bool (*publish_fn_t)(const char *topic, const char *payload, size_t len);

void        publish_mock_reset(void);
void        publish_mock_set_fail_after(uint32_t n);
bool        publish_mock_fn(const char *topic, const char *payload, size_t len);
uint32_t    publish_mock_get_count(void);
const char *publish_mock_get_topic(uint32_t index);
const char *publish_mock_get_payload(uint32_t index);

#endif /* PUBLISH_MOCK_H */
