/* SPDX-License-Identifier: BSD-2-Clause */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <random>

using namespace std;

extern "C" {

#include <tilck/common/utils.h>

#include <tilck/kernel/system_mmap.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/paging.h>

extern bool kmalloc_initialized;
extern bool suppress_printk;

void *kernel_va = nullptr;
bool mock_kmalloc = false;

static unordered_map<uptr, uptr> mappings;

void initialize_test_kernel_heap()
{
   uptr test_mem_size = 256 * MB;

   if (kernel_va != nullptr) {
      bzero(kernel_va, test_mem_size);
      mappings.clear();
      return;
   }

   kernel_va = aligned_alloc(MB, test_mem_size);

   mem_regions_count = 1;
   mem_regions[0] = (memory_region_t) {
      .addr = 0,
      .len = test_mem_size,
      .type = MULTIBOOT_MEMORY_AVAILABLE,
      .extra = 0,
   };
}

void init_kmalloc_for_tests()
{
   kmalloc_initialized = false;
   initialize_test_kernel_heap();
   suppress_printk = true;
   init_kmalloc();
   suppress_printk = false;
}

int map_page(pdir_t *, void *vaddr, uptr paddr, bool us, bool rw)
{
   ASSERT(!((uptr)vaddr & OFFSET_IN_PAGE_MASK)); // check page-aligned
   ASSERT(!(paddr & OFFSET_IN_PAGE_MASK)); // check page-aligned

   mappings[(uptr)vaddr] = paddr;
   return 0;
}

size_t
map_pages(pdir_t *pdir,
          void *vaddr,
          uptr paddr,
          size_t page_count,
          bool big_pages_allowed,
          bool us,
          bool rw)
{
   for (size_t i = 0; i < page_count; i++) {
      int rc = map_page(pdir,
                        (char *)vaddr + (i << PAGE_SHIFT),
                        paddr + (i << PAGE_SHIFT),
                        us,
                        rw);
      VERIFY(rc == 0);
   }

   return page_count;
}

void unmap_page(pdir_t *, void *vaddrp, bool free_pageframe)
{
   mappings[(uptr)vaddrp] = INVALID_PADDR;
}

int unmap_page_permissive(pdir_t *, void *vaddrp, bool free_pageframe)
{
   unmap_page(nullptr, vaddrp, free_pageframe);
   return 0;
}

void
unmap_pages(pdir_t *pdir,
            void *vaddr,
            size_t page_count,
            bool free_pageframes)
{
   for (size_t i = 0; i < page_count; i++) {
      unmap_page(pdir, (char *)vaddr + (i << PAGE_SHIFT), free_pageframes);
   }
}

bool is_mapped(pdir_t *, void *vaddrp)
{
   uptr vaddr = (uptr)vaddrp & PAGE_MASK;

   if (vaddr + PAGE_SIZE < LINEAR_MAPPING_END)
      return true;

   return mappings.find(vaddr) != mappings.end();
}

uptr get_mapping(pdir_t *, void *vaddrp)
{
   return mappings[(uptr)vaddrp];
}

void *kmalloc(size_t size)
{
   if (mock_kmalloc)
      return malloc(size);

   return general_kmalloc(&size, 0);
}

void kfree2(void *ptr, size_t size)
{
   if (mock_kmalloc)
      return free(ptr);

   return general_kfree(ptr, &size, 0);
}

void *kmalloc_get_first_heap(size_t *size)
{
   static void *buf;

   if (!buf) {
      buf = aligned_alloc(KMALLOC_MAX_ALIGN, KMALLOC_FIRST_HEAP_SIZE);
      VERIFY(buf);
      VERIFY( ((uptr)buf & (KMALLOC_MAX_ALIGN - 1)) == 0 );
   }

   if (size)
      *size = KMALLOC_FIRST_HEAP_SIZE;

   return buf;
}

} // extern "C"
