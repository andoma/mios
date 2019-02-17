OUTPUT_ARCH( "riscv" )

__stack_size = 0x2000;;


SECTIONS
{
  . = 0x80000000;
  .text : {
        *(.text)
         _etext = .;
         }

  .data : {
         . = ALIGN(8);
         *(.data)
         *(.data.*)
         . = ALIGN(8);
         _edata = .;
         }

  .stack : {
         . = ALIGN(8);
         _estack = .;
         . = . + __stack_size;
         . = ALIGN(8);
         _sstack = .;
         }

  .bss : {
         . = ALIGN(8);
          _sbss = .;
          *(.bss)
          *(.sbss)
          *(COMMON)
         . = ALIGN(8);
         _ebss = .;
         }
}
