#include "settings_flash.h"
#include "gd32_sdk.h"
#include <string.h>
static uint32_t address(unsigned page) { return page ? SETTINGS_PAGE_B : SETTINGS_PAGE_A; }
static bool bounded(unsigned page,size_t offset,size_t length) {
    return page<2 && offset<=SETTINGS_PAGE_SIZE && length<=SETTINGS_PAGE_SIZE-offset;
}
static void clear_flags(void) {
    fmc_flag_clear(FMC_FLAG_BANK0_END);
    fmc_flag_clear(FMC_FLAG_BANK0_WPERR);
    fmc_flag_clear(FMC_FLAG_BANK0_PGERR);
}
static bool read_page(void *context,unsigned page,size_t offset,uint8_t *out,size_t length) {
    (void)context;
    if (!bounded(page,offset,length)) return false;
    memcpy(out,(const void *)(address(page)+offset),length); return true;
}
static bool erase_page(void *context,unsigned page) {
    (void)context;
    if (page>=2) return false;
    fmc_unlock(); clear_flags();
    fmc_state_enum result=fmc_page_erase(address(page)); fmc_lock();
    if (result!=FMC_READY) return false;
    const volatile uint16_t *words=(const volatile uint16_t *)address(page);
    for (unsigned i=0;i<SETTINGS_PAGE_SIZE/2;i++) if (words[i]!=0xffff) return false;
    return true;
}
static bool program_page(void *context,unsigned page,size_t offset,const uint8_t *data,size_t length) {
    (void)context;
    if (!bounded(page,offset,length) || (offset&1) || (length&1)) return false;
    fmc_unlock();
    bool ok=true;
    for (size_t i=0;i<length;i+=2) {
        uint32_t target=address(page)+offset+i;
        uint16_t value=data[i]|((uint16_t)data[i+1]<<8);
        if ((*(const volatile uint16_t *)target&value)!=value) { ok=false; break; }
        clear_flags();
        if (fmc_halfword_program(target,value)!=FMC_READY || *(const volatile uint16_t *)target!=value) { ok=false; break; }
    }
    fmc_lock(); return ok;
}
/* Page erase requires no ACL/inquiry and a drained UART. settings_append only
 * programs halfwords into erased positions; circular UART DMA remains active. */
const settings_io_t settings_flash_io={NULL,read_page,erase_page,program_page};
