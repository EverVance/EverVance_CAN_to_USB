set pagination off
set confirm off
file F:/AS/NXP_Workspace/VBA_CAN/out/artifacts/Bootloader/bootloader.elf
add-symbol-file F:/AS/NXP_Workspace/VBA_CAN/out/artifacts/CAN_APP/can_app.elf 0x60020000
target remote :3333
info registers pc sp lr xpsr
x/8wx 0xE000ED00
x/4wx 0x60022000
bt
quit
