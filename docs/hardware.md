# Hardware and acceptance

Target: GD32F103C8T6 (LQFP48), RDA5876, original bootloader with application base `0x08003400`. Pairing journals use `0x0801EC00` and `0x0801F000`; bootloader scratch begins at `0x0801F400`.

| Signal | MCU GPIO | Behavior |
| --- | --- | --- |
| Button | PA1 | Pull-up; pressed low |
| RGB red / green / blue | PB9 / PB8 / PB5 | Active low |
| Bluetooth TX / RX | PA2 / PA3 | GD32 USART1, H4, 115200 startup then 1500000 baud, no RTS/CTS |
| Radio setup I2C | PB10 / PB11 | Software clock/data, address 0x16 |
| Radio startup control | PB13 | Power-enable sequence; RDA-side endpoint inferred |
| USB pull-up | PB15 | Active low |

LED dimming uses hardware PWM. UART uses RX/TX DMA. USB SOF interrupts are disabled; pending input is collected by 1 ms host polling and completion interrupts. There is no per-byte RX interrupt or periodic duplicate-report stream.

## Observed results

On firmware 0x10029, two 30-second Pro Controller captures with sustained stick motion:

- Gyro off: 88.854 Hz, mean 11.254 ms, p99 14.030 ms, maximum 22.014 ms.
- Gyro on: 88.848 Hz, mean 11.255 ms, p99 14.031 ms, maximum 17.975 ms.
- The preceding periodic 24–40 ms stalls every 1.28 seconds disappeared after disabling page scan for a complete profile and switching USB IN polling from 8 ms to 1 ms.

The two changes were measured together. Their individual causal contributions were not isolated by an A/B test. USB timing is Windows read completion, not an air or bus analyzer. The isolated 22 ms gap is consistent with a missing report but its loss location was not established.

Two Joy-Cons reconnecting simultaneously and after profile switching were physically verified on 0x10027. The 0x10029 scan changes have native state-machine coverage for both Joy-Cons, pairing/cancel, profile switch and missing-peer recovery; repeat live acceptance on this revision before claiming identical coverage.

Known boundaries: two peer slots; no simultaneous third Pro; not a 125 Hz Bluetooth implementation; no claim of broad receiver-model or Switch-console compatibility. Standard HID battery semantics and neutral controls were checked with the Windows HID parser. Steam rumble was exercised earlier; repeat in your chosen game after installing.
