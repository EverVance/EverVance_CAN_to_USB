set pagination off
set confirm off
set breakpoint pending on

file F:/AS/NXP_Workspace/VBA_CAN/out/artifacts/Bootloader/bootloader.elf
target remote 127.0.0.1:3333

break *0x60008b39
break *0x600406b5
break *0x60035e91
break *0x60036871
break *0x60036609

monitor reset

echo === stop 1 ===\n
continue
info registers pc sp lr xpsr
x/4wx 0x60022000

echo === stop 2 ===\n
continue
info registers pc sp lr xpsr
x/4wx 0x60022000

echo === stop 3 ===\n
continue
info registers pc sp lr xpsr
x/8wx 0x401BC000

echo === stop 4 ===\n
continue
info registers pc sp lr xpsr
x/8wx 0x401BC000

echo === stop 5 ===\n
continue
info registers pc sp lr xpsr
x/8wx 0x401BC000

quit
