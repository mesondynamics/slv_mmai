/* This script is linked in addition to CubeMX's primary script.  It remains
   effective even if a future CubeMX version regenerates the primary MEMORY
   length and prevents loadable Secure data from entering parameter sectors
   125/126 at 0x0C0FA000..0x0C0FDFFF. */
ASSERT(__data_source_end <= 0x0C0FA000,
       "Secure image overlaps reserved valve parameter Flash")
