#ifndef REDEV_H4_H
#define REDEV_H4_H
#include <stddef.h>
#include <stdint.h>
#define H4_MAX_FRAME 1029u
typedef void (*h4_packet_fn)(void *context, const uint8_t *packet, size_t length);
typedef struct {
    uint8_t data[H4_MAX_FRAME];
    size_t used, header_size, total, discard;
    uint32_t bad_type, oversized;
    h4_packet_fn callback;
    void *context;
} h4_parser_t;
void h4_init(h4_parser_t *parser, h4_packet_fn callback, void *context);
void h4_feed(h4_parser_t *parser, const uint8_t *data, size_t size);
#endif
