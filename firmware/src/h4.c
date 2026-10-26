#include "h4.h"
#include <string.h>
void h4_init(h4_parser_t *p, h4_packet_fn callback, void *context) {
    memset(p, 0, sizeof(*p));
    p->callback = callback;
    p->context = context;
}
void h4_feed(h4_parser_t *p, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (p->discard) {
            --p->discard;
            continue;
        }
        if (p->used == 0) {
            switch (data[i]) {
            case 0x04:
                p->header_size = 3;
                break; /* event */
            case 0x02:
                p->header_size = 5;
                break; /* ACL */
            case 0x03:
                p->header_size = 4;
                break; /* SCO (ignored by host callback) */
            default:
                ++p->bad_type;
                continue;
            }
            p->total = 0;
        }
        p->data[p->used++] = data[i];
        if (p->used == p->header_size) {
            size_t payload = p->data[p->header_size - 1];
            if (p->data[0] == 0x02)
                payload = (size_t)p->data[3] | ((size_t)p->data[4] << 8);
            p->total = p->header_size + payload;
            if (p->total > sizeof(p->data)) {
                /* Quarantine entire declared payload. Searching it for H4
                 * bytes would interpret arbitrary data as trusted events. */
                ++p->oversized;
                p->discard = payload;
                p->used = 0;
                continue;
            }
        }
        if (p->total && p->used == p->total) {
            p->callback(p->context, p->data, p->total);
            p->used = 0;
        }
    }
}
