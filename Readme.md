# The JesFs Bootloader for nRF52 #
## V1.00 ##

Using a Filesystem for Firmware Updates is the most flexible solution, because it allows many different ways to get Firmware on the target board:

- Low Power Wireless (Bluetooth, ANT, 433/868 MHz Radio)
- WiFi
- UART
- USB
- JTAG
- Mobile Internet
- …
![BLE Terminal for File Transfer](https://github.com/joembedded/JesFs_Bootloader/blob/master/Docu/BLE_Term.jpg)

I wrote the JesFs Bootloader for my daily work. Feel free to use it too (*License: LGPL v3*).
For my application I am using BLE (Webbluetooth API) and Mobile Internet.

The nRF52840-DK (‘pca10056’) already has on on-board 8 MByte Serial Flash Memory (Ultra Low Power, < 0.5 µA in Deepsleep), thus making it a very convenient development platform! 

## The nRF52 Boot process (with JesFs) ##

Using any complex wireless protocol on a nRF52 CPU requires a „Softdevice“, that works in the background. The User‘s Software is only informed if necessary. This is a very convenient solution.
The User Software and the Softdevice can be updated „in one“ or separately.
Right after Power On or during Software Updates, the MBR (Master Boot Record) handles the Interrupts. Replacing the MBR is normally never necessary. But even for such rare cases, a 4kB Flash Block can be reserved in the CPU‘s Flash. 
After Reset the MBR gives control completely to the Bootloader, which then can replace User Software, Softdevice or both, The Bootloader then can start the Softdevive and the Softdevice starts the User Software.
Without Bootloader the MBR will directly start the Softdevice.
The JesFs Bootloader simply copies optionally new data from Files to Flash. A „Fingerprint“ of the installed Files is written in the „Bootloader-Param“ by the JesFs Bootloader. It only will read files. Writing new Firmware files is the User-Software‘s job! 
For generating Firmware Files (from one or more HEX Files) a small Tool “JesFsHex2Bin” is included (in C and as EXE for Windows).

![nRF52 Components](https://github.com/joembedded/JesFs_Bootloader/blob/master/Docu/Components.jpg)
***