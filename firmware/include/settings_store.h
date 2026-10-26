#ifndef REDEV_SETTINGS_STORE_H
#define REDEV_SETTINGS_STORE_H
#include "bridge.h"
#define SETTINGS_PAGE_SIZE 1024u
#define SETTINGS_RECORD_SIZE 128u
#define SETTINGS_PAGE_A 0x0801ec00u
#define SETTINGS_PAGE_B 0x0801f000u
typedef struct {
    void *context;
    bool (*read)(void *context, unsigned page, size_t offset, uint8_t *out, size_t n);
    bool (*erase)(void *context, unsigned page);
    bool (*program)(void *context, unsigned page, size_t offset, const uint8_t *data, size_t n);
} settings_io_t;
/* Caller serializes access. Program is aligned halfword writes. Old page is
 * NEVER erased until a newer independently valid record exists. */
bool settings_load(const settings_io_t *io, settings_t *settings, uint32_t *sequence);
bool settings_save(const settings_io_t *io, const settings_t *settings);
typedef enum { SETTINGS_APPENDED, SETTINGS_APPEND_FULL, SETTINGS_APPEND_ERROR } settings_append_result_t;
/* Program an erased 128-byte journal position without a page erase. */
settings_append_result_t settings_append(const settings_io_t *io, const settings_t *settings);
#endif
