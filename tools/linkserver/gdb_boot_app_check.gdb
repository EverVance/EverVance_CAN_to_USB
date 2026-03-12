set pagination off
set confirm off
file F:/AS/NXP_Workspace/VBA_CAN/out/artifacts/Bootloader/bootloader.elf
add-symbol-file F:/AS/NXP_Workspace/VBA_CAN/out/artifacts/CAN_APP/can_app.elf 0x60020000
target remote :3333
break *0x60008b49
break *0x60040449
break *0x60035d51
break *0x6003647d
monitor reset
continue
info breakpoints
info registers pc sp lr xpsr
x/4wx 0x60022000
x/8wx 0x401BC000
quit
