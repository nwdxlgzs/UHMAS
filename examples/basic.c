#include "uhm/uhm_platform.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    uhm_platform_heap owner;
    uhm_heap *heap = NULL;
    const uhm_api *api;
    void *object = NULL;
    void *mapping = NULL;
    size_t page_size;
    int failed = 0;

    if (uhm_platform_heap_open_default(&owner, &heap) != UHM_OK) {
        fputs("heap open failed\n", stderr);
        return 1;
    }
    api = uhm_get_api(heap);
    page_size = owner.vm.page_size;
    object = api->malloc(heap, 64);
    if (!object) {
        failed = 1;
        goto close_heap;
    }
    memset(object, 0x2a, 64);
    if (api->mmap(heap, &mapping, page_size,
                  UHM_PROT_READ | UHM_PROT_WRITE, UHM_MAP_PRIVATE) != UHM_OK) {
        failed = 1;
        goto free_object;
    }
    memset(mapping, 0x31, page_size);
    if (api->mprotect(heap, mapping, page_size, UHM_PROT_READ) != UHM_OK) {
        failed = 1;
    } else if (((const unsigned char *)mapping)[0] != 0x31) {
        failed = 1;
    }
    if (api->munmap(heap, mapping, page_size) != UHM_OK) {
        failed = 1;
    }
free_object:
    api->free(heap, object);
close_heap:
    if (uhm_platform_heap_close(&owner) != UHM_OK) {
        failed = 1;
    }
    puts(failed ? "UHMAS example failed" : "UHMAS example passed");
    return failed;
}
