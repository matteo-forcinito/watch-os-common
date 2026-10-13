/* Vault-local LVGL hooks erase retired text buffers, including reallocations.
 * The shared OS and Remote allocator remain unchanged. */
#include "format.h"
#include "esp_heap_caps.h"
#include <string.h>

void *lv_malloc_core(size_t size); /* not wrapped: allocation needs no wiping */
void __real_lv_free_core(void *pointer);

void __wrap_lv_free_core(void *pointer)
{
    if (pointer != NULL)
        vault_wipe(pointer, heap_caps_get_allocated_size(pointer));
    __real_lv_free_core(pointer);
}

void *__wrap_lv_realloc_core(void *pointer, size_t size)
{
    if (size == 0)
    {
        __wrap_lv_free_core(pointer);
        return NULL;
    }
    void *next = lv_malloc_core(size);
    if (next == NULL)
        return NULL;
    if (pointer != NULL)
    {
        size_t previous = heap_caps_get_allocated_size(pointer);
        memcpy(next, pointer, previous < size ? previous : size);
        __wrap_lv_free_core(pointer);
    }
    return next;
}
