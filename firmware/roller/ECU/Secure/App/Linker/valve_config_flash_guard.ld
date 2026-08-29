/* This script is linked in addition to CubeMX's primary script. */
ASSERT(DEFINED(ECU_OEMIROT_LAYOUT) || __data_source_end <= 0x0C0E0000,
       "Development Secure image overlaps Secure persistent Flash")

/* In OEMiROT builds, load data must stop before the NSC veneer.  The complete
   image (header, normal body, NSC, TLVs) must still end below the trailer at
   0x0C05E000. */
ASSERT(!DEFINED(ECU_OEMIROT_LAYOUT) || __data_source_end <= 0x0C05DC00,
       "Secure signed image body overlaps NSC/trailer")
ASSERT(!DEFINED(ECU_OEMIROT_LAYOUT) ||
       (ADDR(.gnu.sgstubs) == 0x0C05DC00 &&
        ADDR(.gnu.sgstubs) + SIZEOF(.gnu.sgstubs) <= 0x0C05E000),
       "Secure gateway veneer escapes OEMiROT NSC window")

ASSERT(_estack == 0x3003FB80,
       "Secure initial stack may overwrite the OEMiROT UID handoff")
ASSERT(_ebss <= 0x3003FB80,
       "Secure .bss overlaps the OEMiROT UID handoff")
ASSERT(ADDR(._user_heap_stack) + SIZEOF(._user_heap_stack) <= 0x3003FB80,
       "Secure heap/stack reservation overlaps the OEMiROT UID handoff")
ASSERT((0x3003FBC0 + 0x40) <= 0x30040000,
       "OEMiROT UID handoff escapes Secure SRAM1")
