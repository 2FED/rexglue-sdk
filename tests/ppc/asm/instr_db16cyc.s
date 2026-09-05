# Raw Xenon hint encoding: the test assembler selects Power7, not Cell.
test_db16cyc_preserves_state:
  #_ MEMORY_IN 10001050 01234567 89ABCDEF
  #_ REGISTER_IN r3 0x123456789ABCDEF0
  #_ REGISTER_IN r31 0xFEDCBA9876543210
  li r4, -1
  addic r5, r4, 1
  mfxer r6
  cmpwi r4, 0
  mfcr r9
  .long 0x7ffffb78
  mfxer r7
  mfcr r10
  blr
  #_ REGISTER_OUT r3 0x123456789ABCDEF0
  #_ REGISTER_OUT r31 0xFEDCBA9876543210
  #_ REGISTER_OUT r6 0x20000000
  #_ REGISTER_OUT r7 0x20000000
  #_ REGISTER_OUT r9 0x80000000
  #_ REGISTER_OUT r10 0x80000000
  #_ MEMORY_OUT 10001050 01234567 89ABCDEF

test_db16cyc_bounded_loop:
  #_ REGISTER_IN r31 0x123456789ABCDEF0
  li r3, 4
  li r4, 0
db16cyc_loop:
  .long 0x7ffffb78
  .long 0x7ffffb78
  .long 0x7ffffb78
  .long 0x7ffffb78
  .long 0x7ffffb78
  .long 0x7ffffb78
  .long 0x7ffffb78
  .long 0x7ffffb78
  addi r4, r4, 1
  addic. r3, r3, -1
  bne db16cyc_loop
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r4 4
  #_ REGISTER_OUT r31 0x123456789ABCDEF0

test_db16cyc_neighbor_or_is_not_hint:
  #_ REGISTER_IN r30 0xF0
  #_ REGISTER_IN r31 0x0F
  or r31, r30, r31
  or. r31, r31, r31
  mfcr r3
  blr
  #_ REGISTER_OUT r30 0xF0
  #_ REGISTER_OUT r31 0xFF
  #_ REGISTER_OUT r3 0x40000000
