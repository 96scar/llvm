# Test object to verify that llvm-dwarfdump handles an invalid string offsets
# table.
#
# To generate the test object:
# llvm-mc -triple x86_64-unknown-linux dwarfdump-str-offsets-invalid-3.s -filetype=obj \
#         -o dwarfdump-str-offsets-invalid-3.x86_64.o

        .section .debug_str,"MS",@progbits,1
str_producer:
        .asciz "Handmade DWARF producer"
str_CU1:
        .asciz "Compile_Unit_1"
str_CU1_dir:
        .asciz "/home/test/CU1"
str_CU2:
        .asciz "Compile_Unit_2"
str_CU2_dir:
        .asciz "/home/test/CU2"
str_TU:
        .asciz "Type_Unit"
str_TU_type:
        .asciz "MyStruct"

        .section .debug_str.dwo,"MS",@progbits,1
dwo_str_CU_5_producer:
        .asciz "Handmade split DWARF producer"
dwo_str_CU_5_name:
        .asciz "V5_split_compile_unit"
dwo_str_CU_5_comp_dir:
        .asciz "/home/test/splitCU"
dwo_str_TU_5:
        .asciz "V5_split_type_unit"
dwo_str_TU_5_type:
        .asciz "V5_split_Mystruct"

# A rudimentary abbrev section.
        .section .debug_abbrev,"",@progbits
        .byte 0x01  # Abbrev code
        .byte 0x11  # DW_TAG_compile_unit
        .byte 0x00  # DW_CHILDREN_no
        .byte 0x00  # EOM(1)
        .byte 0x00  # EOM(2)
        .byte 0x00  # EOM(3)

# A rudimentary compile unit to convince dwarfdump that we are dealing with a
# DWARF v5 string offsets table.
        .section .debug_info,"",@progbits

# DWARF v5 CU header.
        .long  CU1_5_end-CU1_5_version  # Length of Unit
CU1_5_version:
        .short 5               # DWARF version number
        .byte 1                # DWARF Unit Type
        .byte 8                # Address Size (in bytes)
        .long .debug_abbrev    # Offset Into Abbrev. Section
# A compile-unit DIE, which has no attributes.
        .byte 1                # Abbreviation code
CU1_5_end:

        .section .debug_str_offsets,"",@progbits
# CU1's contribution
# Invalid length
        .long 0xfffffffe
        .long .debug_str_offsets_segment0_end-.debug_str_offsets_base0
        .short 5    # DWARF version
        .short 0    # Padding
.debug_str_offsets_base0:
        .long str_producer
        .long str_CU1
        .long str_CU1_dir
.debug_str_offsets_segment0_end:
# CU2's contribution
        .long .debug_str_offsets_segment1_end-.debug_str_offsets_base1
        .short 5    # DWARF version
        .short 0    # Padding
.debug_str_offsets_base1:
        .long str_producer
        .long str_CU2
        .long str_CU2_dir
.debug_str_offsets_segment1_end:
# The TU's contribution
        .long .debug_str_offsets_segment2_end-.debug_str_offsets_base2
        .short 5    # DWARF version
        .short 0    # Padding
.debug_str_offsets_base2:
        .long str_TU
        .long str_TU_type
.debug_str_offsets_segment2_end:
