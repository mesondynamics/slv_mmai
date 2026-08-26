/*
 * The Secure GTZC post-configuration exposes only
 * 0x20050000..0x2005FFFF to non-privileged ETH DMA transactions.  The normal
 * CPU heap/stack may remain above this address because the NonSecure CPU runs
 * privileged; all statically allocated packet buffers must remain below it.
 */
ASSERT(__bss_end__ <= 0x20060000,
       "NonSecure .bss exceeds the reviewed 64 KiB ETH DMA SRAM3 window")
