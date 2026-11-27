# Windows x64 설치 및 Bluetooth 드라이버 복원

## ZIP 설치

1. GitHub Actions의 **SSCOnWindows-win-x64** 아티팩트를 내려받습니다. 아티팩트 안의 `SSCOnWindows-0.2.0.zip`을 새 폴더에 풉니다. 함께 제공되는 SHA-256은 PowerShell의 `Get-FileHash .\SSCOnWindows-0.2.0.zip -Algorithm SHA256` 결과와 대조할 수 있습니다.
2. WinUI는 `winui\SSCOnWindows.exe`, wx GUI 및 CLI는 `wx\SSCOnWindows-0.2.0.exe`를 사용합니다. 각 실행 파일은 같은 폴더에 포함된 DLL과 리소스를 함께 사용합니다. Windows 표시 언어가 한국어이면 WinUI의 한국어 리소스를 사용합니다. 일부 기술 로그는 영어로 표시됩니다.
3. 장치 관리자에서 내장 **Qualcomm FastConnect 7800**과 USB 동글의 하드웨어 ID, 드라이버 공급자 및 버전을 기록합니다. **Alternative A2DP Driver**를 사용 중이면 현재 설정도 기록하고 Qualcomm 경로에서 재생되는지 확인합니다.
4. 시스템 정보(`msinfo32`)의 **보안 부팅 상태**가 켜짐인지 확인합니다. 앱을 실행하는 데 Secure Boot, 드라이버 서명 적용, 테스트 서명 설정을 변경할 필요가 없습니다.

## 지원 동글로 스트리밍

- 기존 WinUI 스트리밍 경로는 RTL8761BU 계열 `2357:0604` 또는 `0BDA:8771`을 대상으로 합니다. 전용 USB 동글에 정확히 대응하는 신뢰할 수 있는 서명된 WinUSB 패키지가 있을 때만 장치 관리자에서 그 동글의 드라이버를 변경합니다. 패키지가 없으면 기존 Bluetooth 드라이버를 유지합니다.
- 내장 Qualcomm 장치, Alternative A2DP Driver, 다른 USB 동글의 드라이버와 설정은 그대로 둡니다. 전용 동글이 WinUSB를 사용하면 Windows Bluetooth 설정에서 그 동글로 페어링할 수 없습니다.
- WinUI에서 **장치 검색 → 헤드셋 선택 → 연결**을 사용합니다. SSC 인코더와 Realtek 펌웨어가 필요한 경우 [기존 설치 절차](../setup.md)를 참고하세요. 테스트 ZIP에는 별도로 배포되는 펌웨어와 SSC 인코더 바이너리가 포함되지 않습니다.

## ipTIME A760 진단

- 장치 관리자에서 동글의 **속성 → 자세히 → 하드웨어 ID**로 들어가 `USB\VID_0BDA&PID_A760&REV_0200`인지 확인합니다.
- WinUI의 **문제 해결 → A760 HCI 진단**에서 USB 리비전과 드라이버 서비스 상태를 확인합니다. 기존 Windows Bluetooth 드라이버를 사용하면 PnP 정보까지만 표시합니다. HCI 진단만을 위해 드라이버를 교체할 필요는 없습니다.
- 검증용 장비에서 이미 WinUSB로 연결된 경우, 이 앱의 다른 인스턴스를 종료하고 A760 한 개만 연결합니다. 진단이 USB 설명자에서 VID/PID/리비전을 확인하고, 표준 HCI 버전 조회(`0x1001`) 결과가 패치 전 식별값과 일치하면 Realtek ROM 버전 조회(`0xFC6D`)를 보냅니다. HCI/LMP 값, ROM 버전, 실패한 명령의 HCI 상태·Win32 오류·응답 길이를 기록합니다.
- PowerShell에서 같은 진단을 파일로 저장할 수 있습니다.

  ```powershell
  Start-Process .\wx\SSCOnWindows-0.2.0.exe -ArgumentList '--diagnose-a760' -Wait -RedirectStandardOutput .\a760.txt
  Get-Content .\a760.txt
  ```

- Windows 11에서 사용자 제공 실측으로 WinUSB 전환, HCI `0x1001` 응답, 한국어 WinUI 실행이 확인됐습니다. 이번 빌드의 `0xFC6D` 결과는 **ROM `0x01`인지** 확인하고 다른 값이나 오류가 나오면 진단 로그를 보관하세요.
- 후보 파일은 Python 3에서 오프라인으로 검사할 수 있습니다. 아래 명령의 ROM에는 새 HCI 진단의 실제 값을 넣습니다. 배포본에 펌웨어 파일은 포함되지 않습니다.

  ```powershell
  py -3 .\tools\a760_firmware.py --rom 0x01 --firmware .\rtl8761bu_fw.bin --config .\rtl8761bu_config.bin
  ```

  `rtl8761bu_rom_candidate`는 파일 구조와 ROM 선택 기준의 일치 여부입니다. `windows_winusb_load_approved`는 검증된 조합이 확보될 때까지 `false`입니다. [펌웨어 비교](../compatibility.md)

### A760 단계별 실물 검증

| 단계 | 현재 상태와 확인할 항목 |
|---|---|
| USB/HCI | 사용자 실측으로 확인. 콜드 재연결 후 기존 값과 새 ROM 조회 결과를 기록합니다. |
| 휘발성 초기화 | 대기. 동일 SHA-256의 펌웨어와 config 조합에 대한 Windows 검증이 선행돼야 합니다. 이후 사용자 확인을 받아 RAM에만 로드하고 첫 실패 단계, 각 블록의 HCI 상태와 마지막 HCI 버전을 기록합니다. |
| 장치 검색 | 대기. 초기화 완료와 HCI 준비 상태를 확인한 뒤 BR/EDR Inquiry로 SM-R190을 찾습니다. |
| 페어링 | 대기. 검색 확인 후 페어링과 재연결, 저장한 링크 키의 동작을 확인합니다. |
| SSC 스트리밍 | 대기. 먼저 기존 지원 동글로 SM-R190을 테스트할 수 있습니다. WinUI의 44.1/48 kHz 선택 후 원격 SSC capability, 협상 바이트, 실제 캡처·인코더 주파수, 소리와 재연결을 확인합니다. |
| 복원 및 보안 | 복원 후 기존 Realtek 드라이버 버전, Qualcomm FastConnect 7800, Alternative A2DP Driver 재생과 Secure Boot 상태를 확인합니다. |

## Windows Bluetooth 드라이버 복원

1. SSC 스트리밍을 중지하고 앱을 종료합니다. 장치 관리자에서 기록해 둔 **USB 동글의 하드웨어 ID와 장치 인스턴스**를 확인합니다.
2. 해당 USB 동글의 **속성 → 드라이버 → 드라이버 롤백**을 사용합니다. 롤백이 비활성화돼 있으면 **드라이버 업데이트 → 내 컴퓨터에서 드라이버 찾아보기 → 컴퓨터의 사용 가능한 드라이버 목록에서 직접 선택**에서 기존 서명된 Bluetooth 드라이버를 선택하거나 제조사의 공식 설치 프로그램을 사용합니다. A760의 경우 ipTIME의 BT60XR/BT60XR-A 공식 드라이버를 사용할 수 있습니다. [ipTIME 설치 안내](https://iptime.com/support/qna/900664)
3. 동글을 다시 연결하고 Windows가 재부팅을 요청하면 재부팅합니다. 장치 관리자에서 원래 공급자·드라이버와 경고 표시를 확인합니다. Windows Bluetooth 설정에서 다시 페어링하고 오디오를 재생합니다.
4. 내장 Qualcomm의 기존 연결, Alternative A2DP Driver를 통해 선택한 오디오 설정과 재생, `msinfo32`의 Secure Boot 상태를 각각 확인합니다. 다른 장치의 드라이버를 삭제하거나 시스템 전체의 Bluetooth 드라이버 패키지를 제거할 필요는 없습니다.

## 검증 범위

Windows CI의 컴파일, 단위 테스트, ZIP 검사가 통과하더라도 새 ROM 진단, 창 크기·DPI별 한국어 레이아웃, SM-R190 오디오, Qualcomm 및 Alternative A2DP Driver 동작, Secure Boot 상태에서의 드라이버 복원은 실물 Windows PC에서 확인해야 합니다.
