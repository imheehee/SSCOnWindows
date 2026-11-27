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

사용자 제공 Windows 11 실측에서 Realtek 드라이버 `20.11.3038.3001`을 쓰던 A760을 WinUSB로 전환해 한국어 WinUI와 HCI `0x1001`을 확인했다. 패치 전 식별은 `HCI 0x0A / rev 0x000B / LMP 0x0A / subver 0x8761 / manufacturer 0x005D`다. Linux에서 보고된 ROM은 `0x01`이며 기존 BTstack RTL8761BU 흐름과 같은 `0xFC6D` 명령으로 Windows에서 다시 측정할 수 있다. 이 식별값은 RTL8761B USB 계열의 근거다. [ipTIME 칩셋 답변](https://iptime.com/support/qna/900295), [Linux 측정](https://iptime.com/support/qna/898506), [Linux 칩 매핑과 epatch 코드](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btrtl.c)

| 펌웨어 후보 | 비교 근거 | Windows WinUSB 로드 승인 |
|---|---|---|
| Linux `rtl8761bu_fw.bin` | Debian `firmware-realtek 20261019-1`에서 오프라인 확인. SHA-256 `1d7a9597349ad89344fa16c1913d3e39e9a12e966e417ca16871bc79bbe59edb`, 44,484바이트, `project_id=14`, 버전 `0xDFC6D922`, ROM 1 → 칩 ID 2 패치 30,204바이트. Linux 게시물에 초기화 타임아웃과 재열거가 한 차례 보고됐다. | 대기 |
| ipTIME/Realtek OEM 패치 | 공개 게시물 기준 29,215바이트, `project_id=14`, SHA-256 `f0437ab72b37d3ac4e3b6e9e5c22c74a0f5d21d9d276b728d7ed5461311a0ce2`. 같은 해시의 패치가 Realtek `20.11.3038.3001`에 포함됐다는 보고가 있다. Linux에서 버전 `0x2AFB4BE5`와 HCI/LMP `0x0E`를 보고했다. OEM 원본 바이트와 Windows 쪽 config는 이번 작업에서 직접 대조하지 않았다. | 대기 |
| Linux `rtl8761bu_config.bin` | 같은 Debian 배포에서 확인한 SHA-256 `6c28a3f07c6a30ed208c4b64862a23f02b7d93543ea980edd24df16bab45095f`. 6바이트 헤더, 설정 레코드 0개. 게시물의 두 Linux 실행 모두 6바이트 config를 사용했다. A760의 OEM 설정이 이 파일과 같다는 근거는 없다. | 대기 |

[Debian 원본 패키지](https://deb.debian.org/debian/pool/non-free-firmware/f/firmware-nonfree/firmware-realtek_20261019-1_all.deb) · [OEM 이미지와 Linux 비교 실측](https://iptime.com/support/qna/898555)

A760에는 기존 코드의 HCI 식별과 ROM 조회 프로토콜을 적용했다. epatch의 프로젝트 ID와 ROM별 패치 선택은 오프라인 검사기로 확인할 수 있다. BTstack의 USB 대상 지정, 펌웨어·config 파일 승인, 블록 상태 확인과 최종 버전 재조회는 실제 Windows 검증이 필요한 초기화 단계다. A760은 BTstack 전송 대상에서 제외하며, 검사기의 해시 일치나 형식 검사는 업로드 권한을 부여하지 않는다. [번들 BTstack Realtek 코드](https://github.com/bluekitchen/btstack/blob/5bc5cbdbeec33be1fdbd0d50e04c0f6deab99d2d/chipset/realtek/btstack_chipset_realtek.c)

### Galaxy Buds Pro SM-R190 / Buds4 Pro

SM-R190용 SSC 기본 협상은 헤드셋이 광고한 44.1/48 kHz 비트를 사용한다. 44.1 kHz 기본 설정은 `0x28`, 48 kHz legacy 설정은 `0x18`이며, 기존 HiFi 가능 헤드셋의 48 kHz 설정 `0x0C`는 유지한다. legacy 모드는 최대 192 kbps로 제한한다. 캡처를 협상한 주파수로 초기화해 인코더와 동일한 PCM 속도를 사용한다. SM-R190의 실제 capability와 재생은 검증 대기다. [SSC 모드와 A2DP 설정값](https://github.com/WillyBilly06/samsung-ssc-encoder#operating-modes)

삼성은 Buds4 Pro의 SSC UHQ를 최대 24비트/96 kHz로 안내한다. 이 앱에서 Buds4 Pro의 HiFi/UHQ는 원격 vendor codec ID, SEP별 capability, 실제 SET_CONFIGURATION 응답, PCM·인코더 경로와 가청 재생을 검증한 후 별도 지원 대상으로 판정한다. 기존 SSC UHQ 경로는 장치가 광고한 기능으로 제한하며 제품명에 따른 강제 활성화는 사용하지 않는다. [Samsung Buds4 Pro 제품 사양](https://www.samsung.com/us/audio-sound/galaxy-buds4-pro/)
