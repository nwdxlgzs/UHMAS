/*
 * UHMAS implementation unity translation unit.
 *
 * The implementation is split into include-only blocks so static helpers stay
 * private while the project moves away from a single giant source file.
 * Keep this order: later blocks depend on declarations and helpers above.
 */
#include "uhm_core_pages.inc"
#include "uhm_slab_tls.inc"
#include "uhm_malloc.inc"
#include "uhm_mmap_vm.inc"
#include "uhm_api_lifecycle.inc"
