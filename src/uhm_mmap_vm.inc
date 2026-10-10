/*
 * mmap/munmap/mremap/mprotect implementation unity block.
 *
 * Split into include-only blocks so helpers remain private while each
 * behavioral area stays navigable.
 */
#include "uhm_mmap_free_vma.inc"
#include "uhm_mmap_ops.inc"
#include "uhm_munmap_mremap.inc"
#include "uhm_mprotect.inc"
