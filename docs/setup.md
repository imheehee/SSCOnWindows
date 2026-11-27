---
title: Setup
layout: default
nav_order: 2
---

# Setup
{: .no_toc }

## Table of contents
{: .no_toc .text-delta }

1. TOC
{:toc}

---

## Prerequisites

- **Windows 10/11** (x64)
- A **dedicated USB Bluetooth adapter** (separate from your built-in Bluetooth)
- For SSC streaming: **WSL2** with a Linux distro (default path) or **Python 3.14 + Qiling** (native path)

## Step 1: Switch the dongle to WinUSB

SSC On Windows uses a dedicated USB Bluetooth adapter via WinUSB.

1. Stop streaming. In Device Manager, check **Details → Hardware Ids** for the
   dedicated USB dongle. Record its current driver provider, version and device
   instance before making changes.
2. Use **Update driver** for that device and select a trusted, signed WinUSB
   package that explicitly matches its hardware ID. If Windows rejects the
   package or none is available, stop and leave Secure Boot, signature
   enforcement and certificate trust settings as configured.
3. In WinUI **Troubleshooting**, refresh and confirm the driver service is
   **WinUSB**. WinUI recognizes a single TP-Link UB500 (`2357:0604`) or
   Realtek RTL8761BU (`0BDA:8771`) for this check. For other dongles, check
   the service in Device Manager. Restart the app after changing the driver so it
   can initialize the controller with the selected firmware.

To restore Windows Bluetooth, stop streaming and use **Properties → Driver →
Roll Back Driver** on the same device. If unavailable, use **Update driver** to
select the original signed vendor or Windows Bluetooth package. Refresh and
verify the original driver provider/service and Windows Bluetooth pairing.
Check that built-in Bluetooth and any Alternative A2DP Driver configuration
still work. Keep those drivers and settings in place through the test.

For SSC with WSL2, run `setup.ps1` from the checkout. The application uses the
default WSL distro and the WSL user's `~/ssc` directory. When you install the
daemon in another distro, set `SSC_WSL_DISTRO` in the PowerShell session that
launches the app.

## Step 2: Find Your Headphone's Bluetooth Address

You need the Bluetooth MAC address of your headphones/speakers.

**From Windows Settings:**
1. Open **Settings > Bluetooth & devices**
2. Click your audio device
3. Click **Properties** -- the address is shown as `AA:BB:CC:DD:EE:FF`

**From the GUI:** paired Bluetooth audio devices appear in the device list after a scan.

**From the CLI:**
```
SSCOnWindows-0.2.0.exe --cli -l
```

## Step 3: Run

**GUI:**
```
SSCOnWindows.exe
```

**CLI:**
```
SSCOnWindows-0.2.0.exe --cli -d AA:BB:CC:DD:EE:FF
```

See [Usage](usage) for full details.

---

## Firmware (Realtek Adapters)

Realtek-based adapters (e.g. TP-Link UB500, RTL8761BU dongles) require proprietary
firmware. **Intel and CSR adapters do not need this step.**

### Manual Download

1. Download the firmware and config `.bin` files for your chipset from
   [linux-firmware/rtl_bt](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtl_bt)
   (e.g. `rtl8761bu_fw.bin`, `rtl8761bu_config.bin`)
2. Place both in the config folder `%APPDATA%\A2DPWB`

{: .note }
These firmware files are proprietary Realtek binaries distributed via the linux-firmware project. They are not included in this repository. See [WHENCE](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/WHENCE) for redistribution terms.

---

## SSC Encoder Daemon

The SSC encoder is the Samsung aarch64 blob, so it runs behind a local TCP daemon
(`:20248`). Two backends are supported:

| Backend | How | Notes |
|:--------|:----|:------|
| WSL2 (default) | Blob + helper under `qemu-aarch64` inside WSL2 | Fastest (~1.2 ms RTT); needed for UHQ |
| Qiling native (`--ssc-native`) | `tools/ssc_daemon/sscblobd.py` under `py -3.14` | No WSL2; ~5x slower, 96 kHz marginal |

The Windows side starts the daemon automatically. For the WSL2 path, a distro must
be installed and running.

---

## Recommended Adapters

| Chipset | Example Products | Notes |
|:--------|:-----------------|:------|
| Realtek | TP-Link UB500, RTL8761BU | Tested; requires firmware download |
| Intel | Intel AX200/AX210 | No firmware needed |
| CSR | Generic CSR8510 dongles | No firmware needed |

USB Bluetooth 5.0+ adapters are recommended.
