the target is a pico2 not a pico so ensure all code is for the RP2350

use /dev/ttyACM0 to do your own debugging

always close the tty when finished and never open it unless it is for flashing or testing

use the command "REBOOT BL" to put the pico2 into bootloader mode for flashing new code

the SDK path is PICO_SDK_PATH=/home/software/unpacked/pico-sdk

you have full control of the pico2 and can flash it and test it under your own power. make sure everything works before notifying me. do not ask me for confirmation unless you need verification with external tools like oscilloscope.
