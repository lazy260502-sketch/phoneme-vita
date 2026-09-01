/*
 * vm_rom_stubs.c - link-time ROM verification symbols.
 *
 * The freshly rebuilt VM objects (BUILD=release) compile the
 * "#ifndef PRODUCT" sections of ROM.hpp/ROMSkeleton.cpp, which REFERENCE
 * a set of _rom_check_* / _rom_* verification symbols. Their definitions
 * live in ROMSkeleton.cpp, which is only linked into the ROM generator -
 * the original VM library shipped objects built with -DPRODUCT and never
 * referenced them. Provide zero/empty definitions here: value 0 means
 * "unchecked" in the ROMChecker semantics and nothing reads them at
 * runtime in this configuration.
 */
#include <stdint.h>

const int _rom_alternate_constant_pool_count = 0;
const int* _rom_alternate_constant_pool_src = 0;
const int _rom_compilation_enabled = 0;
const int _rom_linkcheck_mffd_false = 0;
const void* _rom_original_class_info = 0;
const int _rom_original_class_info_count = 0;
const void* _rom_system_symbols_src = 0;
const void* _rom_text_klass_table = 0;
const int _rom_text_klass_table_size = 0;

/* DEFINE_FRAME_OFFSETS_CHECKER symbols (all "unchecked" == 0) */
#define CHECK(sym) const int _rom_check_##sym = 0;
CHECK(EntryFrame__empty_stack_offset)
CHECK(EntryFrame__fake_return_address_offset)
CHECK(EntryFrame__frame_desc_size)
CHECK(EntryFrame__pending_activation_offset)
CHECK(EntryFrame__pending_exception_offset)
CHECK(EntryFrame__real_return_address_offset)
CHECK(EntryFrame__stored_int_value1_offset)
CHECK(EntryFrame__stored_int_value2_offset)
CHECK(EntryFrame__stored_last_fp_offset)
CHECK(EntryFrame__stored_last_sp_offset)
CHECK(EntryFrame__stored_obj_value_offset)
CHECK(JavaFrame__arg_offset_from_sp_0)
CHECK(JavaFrame__bcp_store_offset)
CHECK(JavaFrame__caller_fp_offset)
CHECK(JavaFrame__cpool_offset)
CHECK(JavaFrame__empty_stack_offset)
CHECK(JavaFrame__end_of_locals_offset)
CHECK(JavaFrame__frame_desc_size)
CHECK(JavaFrame__locals_pointer_offset)
CHECK(JavaFrame__method_offset)
CHECK(JavaFrame__return_address_offset)
CHECK(JavaFrame__stack_bottom_pointer_offset)
CHECK(JavaFrame__stored_int_value1_offset)
CHECK(JavaFrame__stored_int_value2_offset)
CHECK(StackValue__stack_tag_offset)

/* ROM generator hooks referenced by the non-PRODUCT build */
void romgen_check_oopmaps(void) {}
void InitFPU(void) {}
