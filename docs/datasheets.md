# Target datasheets & reference manuals

Reference documents for the fault-injection targets supported by this project.
Each is also vendored as a local copy in this `docs/` directory for offline use.
The local copies are snapshots — always check the vendor link for the latest revision.

> Datasheets are copyright of their respective vendors (STMicroelectronics,
> Silicon Labs, Nordic Semiconductor, Microchip) and are redistributed here only
> for convenience. Refer to the official source of record below.

| Target | Document | Local copy | Official source |
|---|---|---|---|
| STM32F217 | Datasheet (DS8995) | [`STM32F217IGT6.PDF`](STM32F217IGT6.PDF) | [st.com – STM32F217IG](https://www.st.com/en/microcontrollers-microprocessors/stm32f217ig.html) · [direct PDF](https://www.st.com/resource/en/datasheet/stm32f217ig.pdf) |
| STM32F2 series | Reference manual (RM0033) | [`4aaa11.pdf`](4aaa11.pdf) *(excerpt)* | [RM0033 PDF](https://www.st.com/resource/en/reference_manual/rm0033-stm32f205xx-stm32f207xx-stm32f215xx-and-stm32f217xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf) |
| EFM32 Leopard Gecko | Datasheet (EFM32LG) | [`efm32lg-datasheet.pdf`](efm32lg-datasheet.pdf) | [datasheet PDF](https://www.silabs.com/documents/public/data-sheets/efm32lg-datasheet.pdf) · [reference manual](https://www.silabs.com/documents/public/reference-manuals/EFM32LG-RM.pdf) |
| nRF52840 | Product Specification (OPS v0.5) | [`nRF52840_OPS_v0.5-1074816.pdf`](nRF52840_OPS_v0.5-1074816.pdf) | [nordicsemi.com – nRF52840](https://www.nordicsemi.com/Products/nRF52840) · [latest PS v1.9 PDF](https://docs.nordicsemi.com/bundle/nRF52840_PS_v1.9/resource/nRF52840_PS_v1.9.pdf) |
| PIC18F4320 | Datasheet (DS39599C) | [`PIC18F4320_DS39599c.pdf`](PIC18F4320_DS39599c.pdf) | [microchip.com – PIC18F4320](https://www.microchip.com/en-us/product/pic18f4320) · [direct PDF](https://ww1.microchip.com/downloads/en/devicedoc/39599c.pdf) |
| PIC18F4321 | Datasheet (DS39689E) | [`PIC18F4321_DS39689e.pdf`](PIC18F4321_DS39689e.pdf) | [microchip.com – PIC18F4321](https://www.microchip.com/en-us/product/pic18f4321) · [direct PDF](https://ww1.microchip.com/downloads/en/devicedoc/39689e.pdf) |
| PIC18F2X20 family | Flash Programming Specification (DS39592E) | [`PIC18F2X20_progspec_DS39592e.pdf`](PIC18F2X20_progspec_DS39592e.pdf) | [direct PDF](https://ww1.microchip.com/downloads/en/devicedoc/39592e.pdf) |

The PIC18F2X20 programming specification (DS39592E) covers the in-circuit
serial programming (ICSP) entry, command set and code-protection (CP) bits for
the PIC18F2220/2320/4220/4320 family — the basis for the PIC18 ICSP glitch path.
