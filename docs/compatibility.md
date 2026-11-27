---
title: Compatibility
layout: default
nav_order: 8
---

# Compatibility Matrix
{: .no_toc }

## Table of contents
{: .no_toc .text-delta }

1. TOC
{:toc}

---

## Why this page exists

SSC is a vendor-private codec: the app relies on **remote capability
information** negotiated over AVDTP, and the practical result (audio or
silence) depends on what the specific device actually advertises — not just on
what the app is configured to send. With every firmware update the advertised
SSC capability byte can change. **Only devices that have been physically
verified with this app are listed as "tested".** Everything else is
"expected / not verified".

How to read the SSC capability byte (`cap=0x3C` in the logs): per
`sachk/openssc` `pipewire/a2dp-codec-ssc.c`:
- high nibble (`0x30`) = supported rate/mode bits,
- `0x08` = bitrate-limit bit,
- `0x04` = 24-bit support,
- `0x02` = **UHQ2 / 96 kHz** (`SSC_CAP_UHQ2`) — when absent the app refuses
  96 kHz and falls back to 48 kHz to avoid silent playback.

## Tested on this repository

| Device | Address | Dongle / chipset | Tested (SSC) | Sample rate / ch | SSC cap | Result |
|--------|---------|------------------|--------------|------------------|---------|--------|
| Samsung Galaxy **Buds3 FE** | `78:C1:1D:A7:BC:EE` | TP-Link UB500 (RTL8761BU, `VID_2357&PID_0604`) | High 229 / Standard 192 / Mobile 128 kbps | 48 kHz / stereo | `0x3C` (no UHQ) | Audio confirmed; UHQ request automatically falls back to 48 kHz |
| Samsung Galaxy **Buds3 FE** | `78:C1:1D:A7:BC:EE` | TP-Link UB500 | AAC 256 / SBC | 48 kHz / stereo | — | Codec paths present; exercise for spot checks |

Additional verification (not a different device):
- WSL2 daemon (qemu-aarch64) and Qiling native daemon (`--ssc-native`) both
  stream 48 kHz SSC solidly with the Buds3 FE.
- UHQ 96 kHz (584/442/250 kbps) is validated **byte-exact against frozen
  goldens** and encodes correctly, but the Buds3 FE cannot decode it — it is
  only usable with a UHQ-capable sink.

## Expected but not yet verified

| Device class | Expected | Why we expect it |
|--------------|----------|------------------|
| Other **Galaxy Buds** / Samsung TWS (Buds proper, Buds2/2 Pro, Buds3/3 Pro, Buds Pro, Buds Live, Buds+ …) | SSC works; UHQ only on models that advertise the `0x02` bit | Samsung's own Scalable Codec IP runs on-device; the negotiated capability byte decides UHQ eligibility |
| Windows 11 (current builds) | Same as Windows 10 tested baseline | User-mode stack + WinUSB is OS-version independent |

None of the above have been physically tested with this app yet. If you test one,
open a PR adding a row (see *How to contribute a row* below) — keep the address
short or redacted for privacy.

## Platform / capture matrix

| Layer | Support | Notes |
|-------|---------|-------|
| Windows 10 x64 | ✅ used during development | — |
| Windows 11 x64 | ✅ WinUI 3 GUI (Mica) | requires WindowsAppSDK 1.3+ (repo pins 1.8.x) |
| WASAPI capture | 48 kHz stereo | loopback is **pre-volume-mix**; UHQ = 2x SRC from 48 kHz (not a true 96 kHz capture) |
| USB dongles | TP-Link UB500 verified | Intel/CSR dongles expected fine (no firmware upload); Realtek needs `rtl8761*` firmware in `%APPDATA%\A2DPWB` |

## Windows 11 x64 qualification

| Target | Code review result | Windows or hardware check |
|--------|--------------------|---------------------------|
| Windows 11 x64 | CI targets x64 with MSVC and WinUI. Release ZIP stages the Windows App SDK and x64 Visual C++ DLLs. | Extract on a clean Windows 11 x64 machine and launch both GUIs. Exercise Settings → Language → 한국어 in wxWidgets and launch WinUI with Korean as the Windows display language. |
| Realtek RTL8761 family | BTstack registers its known Realtek IDs. The application also maps OEM IDs such as TP-Link `2357:0604` to RTL8761BU. WinUI can infer RTL8761BU for a single known adapter in WinUSB mode while preserving explicit settings. The app metadata names RTL8761CU, but the pinned BTstack USB firmware table has no `0BDA:876C` entry. The selected firmware and configuration must match the controller. | Confirm WinUSB enumeration, firmware download, HCI readiness, scan, pairing and reconnect on each dongle revision. Test with only one WinUSB Bluetooth dongle connected because the UI's chip setting does not select a physical device. |
| Realtek `0BDA:A760` | A760 is detected separately from streaming. Raw HCI version diagnostics are gated on `REV_0200` with existing WinUSB; there is no verified app firmware mapping for this ID. | Record the full hardware IDs and HCI local version (including LMP subversion and HCI revision). Match those to authoritative vendor firmware before adding a mapping. Check signed WinUSB availability, cold-start firmware download and restoration to the original driver. |
| Galaxy Buds Pro SM-R190 | SSC detection uses the remote AVDTP vendor/codec IDs `0x00000075 / 0x0103` and the reported mode bits. There is no SM-R190-specific override. | Capture the advertised SSC capabilities and selected codec; confirm audible 48 kHz stereo, reconnect and AAC/SBC fallback. Try 96 kHz only if the UHQ capability is advertised. Record the earbuds' firmware version. |
| Qualcomm FastConnect 7800 and Alternative A2DP Driver | The BTstack route uses a dedicated USB adapter. The guided driver flow performs a read-only status check and presents Device Manager instructions. | Record the Qualcomm and Alternative A2DP Driver state before setup. Verify Bluetooth pairing and audio through that original route before installing WinUSB, during SSC use and after restoring the USB dongle. |
| Secure Boot and recovery | WinUI leaves package selection and rollback to Device Manager. | With Secure Boot and existing signing policies enabled, install an accepted signed package for the dedicated dongle only. Confirm driver state after install, reboot and rollback. Compare the final device service and driver provider/version against the baseline. |

The golden tests validate SSC encoder output. They do not run Windows Plug and
Play, exercise Bluetooth hardware, or verify that an SM-R190 decodes the stream.

## How to contribute a row

1. Build & run against your sink.
2. Capture the diagnostic block (`CAP:` / `SSC:` / `BTstack:` lines + the
   `cap=0xXX` capability byte) into the issue or PR.
3. State the exact model + firmware generation (e.g. "Buds2 Pro"),
   dongle/chipset, and codec/bitrate/rate combos you confirmed.
4. Mark clearly whether it produced **audio**, **silence**, or **garbled** —
   all three are useful datapoints for the gate logic.

Keep the device address private (use `AA:BB:…:FF` or a short prefix).

## Known limitations (affect compatibility)

- Windows **default output volume** does not change the captured/game-audio
  level (loopback is pre-mix). Use the app's AVRCP volume slider.
- Without the remote `0x02` (UHQ2) capability bit, the app **falls back to
  48 kHz** — the sink accepts the 0x0E config but mutes 96 kHz audio, so the
  gate is deliberate.
- SSC bitrates are mode-gated; the app snaps to the valid set (447 kbps → 442
  on UHQ / 328 on basic) to avoid malformed frames.
- The native Qiling daemon is ~5x slower than WSL2; acceptable at 48 kHz,
  marginal at 96 kHz UHQ.
### ipTIME BT60XR / BT60XR-A (`0BDA:A760`, `REV_0200`)

ipTIME 기술팀은 BT60XR-A를 **RTL8761B** 기반으로 확인했다. 같은 USB ID와 리비전의 공개 Linux 실측에서는 ROM이 `HCI 0x0A / HCI rev 0x000B / LMP subversion 0x8761 / ROM version 1`을 보고했다. Linux의 `btrtl`은 이 HCI/LMP 조합과 USB 전송 유형을 RTL8761BU 펌웨어 항목에 매핑한다. 공개 실측의 Linux 기본 펌웨어와 제조사 패치는 서로 다른 펌웨어 버전을 보고했다. 동일한 칩 계열에서도 제품별 펌웨어와 설정을 별도로 확인해야 한다. [ipTIME 기술팀 답변](https://iptime.com/support/qna/900295), [Linux 실측과 제조사 패치 비교](https://iptime.com/support/qna/898551), [Linux btrtl 칩 매핑](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c)

현재 BTstack의 `0BDA:8771` 초기화에서 재사용 후보는 HCI 버전 조회, Realtek ROM 리비전 조회, epatch 분석 및 분할 전송 순서다. 번들 BTstack에는 A760의 펌웨어 항목이 없다. Windows WinUSB에서 이 리비전의 초기화 전후 응답, 정확한 ROM 패치/설정 조합, 콜드 부팅, 재연결 및 A2DP 재생은 확인되지 않았다. 앱은 A760을 `8771`에 매핑하지 않으며 BTstack 전송에서도 모든 A760 리비전을 제외한다. 진단은 정확히 `REV_0200`을 확인한 뒤 표준 HCI 버전 조회만 수행한다. [번들 BTstack Realtek 구현](https://github.com/bluekitchen/btstack/blob/5bc5cbdbeec33be1fdbd0d50e04c0f6deab99d2d/chipset/realtek/btstack_chipset_realtek.c)
