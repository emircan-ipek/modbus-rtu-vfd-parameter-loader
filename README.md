# Modbus RTU Parameter Loader for Industrial VFDs

A portable, battery-powered handheld device that writes complete parameter sets to industrial variable frequency drives (VFDs) over Modbus RTU / RS-485 — at the push of a single button.

Built on an ESP32-C3 during an R&D internship at **Yılmaz Redüktör** (Automation & R&D department, 2026).

---

## The problem

Commissioning an industrial motor drive means entering dozens of parameters by hand through the drive's keypad — slow, repetitive, and easy to get wrong. When an engineer has to commission many drives in the field, or reconfigure the same drive repeatedly during testing, this becomes the bottleneck.

This device removes that step. Parameter sets are loaded once from a PC, stored permanently on the device, and then written to any drive in the field with one button press — no laptop required on site.

---

## How it works

The ESP32-C3 operates in **two Modbus roles simultaneously**, which is the core design idea:

| Role | Interface | Purpose |
|---|---|---|
| **Modbus Slave** | USB serial (to PC) | Receives parameter sets from QModMaster or any Modbus master on the PC. |
| **Modbus Master** | RS-485 (to drive) | Writes those parameters to the industrial drive and reads them back to verify. |

```
   PC / QModMaster                  ESP32-C3                       VFD Drive
  ┌───────────────┐            ┌──────────────────┐            ┌──────────────┐
  │               │   USB      │  Modbus SLAVE    │            │              │
  │  parameter    ├───────────>│        +         │   RS-485   │  industrial  │
  │  set entry    │  serial    │  Modbus MASTER   ├───────────>│  motor drive │
  │               │            │  + NVS storage   │  (MAX485)  │              │
  └───────────────┘            └──────────────────┘            └──────────────┘
                                  ▲            │
                            button│            │RGB status LED
```

---

## Features

- **Up to 100 parameters per bank**, written to the drive sequentially in the exact order they were entered.
- **Three independent parameter banks**, each appearing to the PC as a separate Modbus slave ID (1, 2, 3) — so three different drive configurations can be stored and switched between on the fly.
- **Stack-style capture.** Every incoming value is appended to the next free slot, even when written to the same register address repeatedly. This allows staged parameter sequences where a register must pass through intermediate values before reaching its final one.
- **Write-and-verify.** Each parameter is written with up to 3 retries, then read back from the drive and compared. For registers written multiple times, only the final (persistent) value is read back — intermediate writes are verified by their Modbus ACK.
- **Non-volatile storage.** All banks survive power loss via the ESP32 NVS flash partition.
- **Visual feedback.** An RGB LED shows the active bank by colour, flashes green on a fully successful transfer, red on any failure, and signals a full bank.
- **Deep sleep** after a period of inactivity, with wake-on-button — critical for a battery-powered field tool.
- **Safety interlock.** Transfers are blocked while the device is connected to a PC over USB, preventing an accidental write while parameters are still being edited.

---

## Controls

| Action | Result |
|---|---|
| Short press (main button) | Write the active bank to the connected drive |
| Long press (> 600 ms) | Cycle to the next bank (1 → 2 → 3 → 1) |
| Hold BOOT for 2 s | Erase all parameters in the active bank |

| LED colour | Meaning |
|---|---|
| Blue | Bank 1 active |
| Magenta | Bank 2 active |
| Cyan | Bank 3 active |
| Flashing green | Transfer completed and verified successfully |
| Flashing red | Transfer failed, bank empty, or USB connected |

---

## Hardware

| Component | Part |
|---|---|
| MCU | ESP32-C3 (RISC-V, USB Serial/JTAG) |
| Transceiver | MAX485 / RS-485 module (3.3 V) |
| Target | Industrial VFDs, 0.75 kW – 220 kW |
| Power | 18650 Li-ion cell |
| Indicators | Common-cathode RGB LED, status/power LED |

### Pin assignment

| Signal | GPIO |
|---|---|
| RS-485 RX | 20 |
| RS-485 TX | 21 |
| RS-485 DE/RE (direction) | 10 |
| Main button | 4 |
| BOOT button (erase) | 9 |
| RGB — Red / Green / Blue | 1 / 2 / 5 |
| Status power LED | 6 |

**Modbus settings:** 9600 baud, 8N1, drive slave ID 1.

---

## Implementation notes

A few problems that shaped the final design, documented here because they are not obvious:

**Detecting a write of value `0`.** New parameter values are captured by comparing the Modbus holding-register array against a shadow copy. With both arrays initialised to `0`, the very first write of `0` to an address was invisible — old and new value matched. The arrays are therefore initialised to `0xFFFF` as a "never written" sentinel, so a genuine `0` registers as a change.

**Verifying repeated writes to one address.** When the same register is written several times in one bank, only the last value survives on the drive. Reading back every entry therefore produced false failures. The verification pass now identifies the final entry per address and reads back only that one; earlier entries are validated by their write acknowledgement instead.

**Fixed-point of the problem is the drive, not the MCU.** Each Modbus transaction needs a short settling delay before the drive responds reliably; a 15 ms gap plus a 3-attempt retry loop gives a ~4 s transfer time for 90 parameters with full verification.

---

## Repository structure

```
.
├── src/
│   └── parameter_loader.ino    # Main firmware (Arduino framework)
├── docs/
│   ├── wiring_diagram.png      # ESP32-C3 ↔ RS-485 ↔ drive connections
│   ├── pinout.md
│   └── serial_output.txt       # Example transfer log
├── media/
│   ├── device.jpg              # Photos of the assembled device
│   └── demo.mp4                # Short demo video (or link to it)
├── LICENSE
└── README.md
```

---

## Getting started

**Prerequisites:** Arduino IDE with ESP32 board support, plus the `ModbusMaster` and `ModbusRTUSlave` libraries.

1. Wire the hardware according to `docs/wiring_diagram.png`.
2. Open `src/parameter_loader.ino` in the Arduino IDE.
3. Select **ESP32C3 Dev Module** as the board and upload.
4. Connect the device to a PC and open QModMaster, targeting slave ID 1 (bank 1).
5. Write parameter values to the register addresses you want to configure — each write is captured and stored automatically.
6. Disconnect USB, connect the RS-485 lines to the drive, and press the button once.

---

## Author

**Emircan İpek** — Electrical & Electronics Engineering, Haliç University
[LinkedIn](https://linkedin.com/in/emircan-ipek-b53159291)

## License

See [LICENSE](LICENSE).
