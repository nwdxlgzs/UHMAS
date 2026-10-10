/*
 * Core implementation include wrapper.
 *
 * These fragments are still included into one translation unit so internal
 * helpers keep static linkage while the project stays navigable.
 */
#include "uhm_core_base.inc"
#include "uhm_core_vm_ops.inc"
#include "uhm_core_control.inc"
#include "uhm_core_metadata.inc"
#include "uhm_core_page_map.inc"
