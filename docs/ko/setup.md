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

## ipTIME A760 실험용 RAM 초기화

1. Windows 11 장치 관리자에서 이미 WinUSB로 연결된 `USB\VID_0BDA&PID_A760&REV_0200` 한 대를 확인합니다. 기존 장치 검색·스트리밍을 끝내고 다른 SSCOnWindows 인스턴스를 종료하세요. 동글을 물리적으로 뺐다가 다시 연결하면 콜드 상태에서 시험할 수 있습니다.
2. WinUI에서 **문제 해결 → A760 펌웨어 다운로드 및 검증**을 누릅니다. 공식 linux-firmware의 고정 커밋에서 두 파일을 HTTPS로 가져와 크기·SHA-256·epatch·config를 검사하고, 연결된 장치의 HCI, ROM 버전, Realtek 보안 프로젝트 키 ID를 확인합니다. 네트워크에서 가져오기 어렵다면 [공식 원본 커밋](https://gitlab.com/kernel-firmware/linux-firmware/-/tree/a2dcb924f0897368b0b4f305cbf9808cdda90ce0/rtl_bt)에서 받은 `rtl8761bu_fw.bin`과 `rtl8761bu_config.bin`을 한 폴더에 두고 **로컬 A760 펌웨어 검증**을 누르세요.
3. 검증을 통과하면 **실험용 RAM 초기화 시작 → RAM 로드 승인**을 누릅니다. 앱은 전용 USB 핸들을 열고 `REV_0200`, 패치 전 HCI 식별값, ROM `0x01`, 키 ID `0`을 다시 확인한 후 Project 14 / Chip ID 2 패치를 RAM에만 보냅니다. 블록의 오류·누락 또는 버전 불일치에서 즉시 중단하며 자동 재시도나 다른 장치로의 전환을 하지 않습니다.
4. 초기화 성공 조건은 마지막 블록 후의 HCI 펌웨어 버전 `0xDFC6D922`, BTstack의 HCI Reset 이후 동일한 버전, 그리고 `HCI_STATE_WORKING`입니다. **A760 로그 저장**으로 두 SHA-256, 전후 HCI/LMP, ROM·키 ID 상태, 120개 블록의 인덱스·응답과 앱 단계 기록을 텍스트로 보관합니다. 실패하면 로그를 보관하고 앱을 닫은 후 동글을 뺐다가 다시 연결하세요.
5. **장치 검색**을 누르고 페어링 모드의 Galaxy Buds Pro **SM-R190**을 선택하세요. 코덱 **SSC**, 주파수 **48 kHz**로 연결해 소리가 나는지 확인합니다. 연결을 끊은 뒤 **44.1 kHz**로 다시 시험하고, 앱에 표시된 실제 스트림·캡처 주파수와 코덱을 기록하세요. 헤드셋이 한 주파수만 광고하면 그 주파수로 협상합니다.
6. 실험 종료는 스트리밍을 끊고 앱을 닫은 다음 USB 전원을 끊습니다. 실험 선택과 링크 키는 세션 중에만 사용합니다. Qualcomm FastConnect 7800, Alternative A2DP Driver와 Secure Boot는 그대로 두세요.

ROM 진단만 다시 하려면 **A760 HCI 진단**을 사용하거나 다음과 같이 CLI 출력을 저장합니다.

```powershell
Start-Process .\wx\SSCOnWindows-0.2.0.exe -ArgumentList '--diagnose-a760' -Wait -RedirectStandardOutput .\a760.txt
```

사용자 제공 Windows 11 실측에서 WinUSB, HCI `0x1001`, ROM `0xFC6D → 0x01`까지 확인됐습니다. 이번 앱에서 실행하는 RAM 전송·리셋·검색·페어링·가청 재생은 실물 검증 단계입니다. [펌웨어 비교](../compatibility.md)

## 삼성 SSC 인코더 설치 및 연결 전 테스트

WinUI에서 **SSC 인코더 설치 및 설정**을 열고 실행 방식을 선택하세요. **인코더 파일 선택 및 설치**에서 직접 확보한 `libScalable_Encoder.so`를 선택하면 설치 콘솔이 열립니다. 콘솔의 설치 질문에 답한 뒤 창을 닫고, **실행 환경·의존성·포트 진단 → 연결 전 SSC 인코더 자체 테스트** 순으로 실행합니다. 자체 테스트는 선택된 방식으로 실제 PCM 프레임을 보내 SSC 데이터가 돌아오는지 확인하고 데몬을 종료합니다. 스트리밍에서는 앱이 데몬을 시작하고 세션이 끝나면 종료합니다.

인코더는 소유한 Galaxy의 시스템/vendor 이미지에서 적법하게 확보한 **ARM64 ELF `libScalable_Encoder.so`**를 사용합니다. 이미지에 따라 `vendor/lib64` 또는 추출한 Bluetooth APEX 내부에서 찾습니다. 테스트 ZIP에는 삼성 인코더가 들어 있지 않습니다.

### WSL2 + QEMU

- `wsl -l -v`에서 Ubuntu가 버전 **2**로 표시되는지 확인합니다. 필요한 경우 Windows PowerShell에서 `wsl --install -d Ubuntu`를 실행하고 Ubuntu 초기 설정을 완료합니다. 기본 배포판이 다르면 WinUI를 실행하는 PowerShell에서 `$env:SSC_WSL_DISTRO = 'Ubuntu'`를 설정합니다.
- 설치 콘솔은 Ubuntu의 `gcc`, `qemu-user`, `libc6-arm64-cross`가 없을 때 apt 설치를 제안합니다. 바이너리는 `~/ssc/blob/libScalable_Encoder.so`에, 데몬·헬퍼·shim은 `~/ssc/openssc/build_blob/`에 설치됩니다. 구버전 데몬이 이미 설치된 경우 이 빌드의 설치 메뉴에서 다시 빌드합니다.
- 데몬은 WSL 루프백의 TCP `20248`에만 바인딩합니다. Windows에서 연결하지 못하면 WSL의 localhost 전달과 이전 SSC 데몬이 포트를 점유하는지 확인하세요. 방화벽이나 드라이버 서명 설정을 변경할 필요는 없습니다.

### Windows + Qiling

- [Python 설치 프로그램](https://www.python.org/downloads/windows/)으로 Python 3.14와 `py` 런처를 준비합니다. 설치 콘솔이 `py -3.14 -m pip install --user qiling`을 제안합니다.
- 설치 콘솔에서 신뢰할 수 있는 ARM64 rootfs 폴더를 지정합니다. 기존 저장소 체크아웃의 `tools\ssc_daemon\rootfs`에 `helper\ssc_blob_helper`, `shims\libc.so`, `lib\ld-linux-aarch64.so.1`, `lib\aarch64-linux-gnu` 등의 런타임이 있습니다. 설치 프로그램은 이 폴더의 `helper`, `shims`, `lib`만 복사하고, 직접 선택한 인코더만 `%LOCALAPPDATA%\SSCOnWindows\encoder\rootfs\blob\libScalable_Encoder.so`에 배치합니다. 데몬은 `%LOCALAPPDATA%\SSCOnWindows\encoder\sscblobd.py`입니다.
- 콘솔 설치를 직접 실행할 경우, 압축 해제 폴더에서 다음 명령을 사용합니다. Python/Qiling 또는 WSL 설치와 스크립트 실행은 현재 Windows 정책이 허용하는 범위에서 진행하세요.

  ```powershell
  .\tools\ssc_setup.ps1 -Engine WSL2 -Blob 'C:\SSC\libScalable_Encoder.so'
  .\tools\ssc_setup.ps1 -Engine Qiling -Blob 'C:\SSC\libScalable_Encoder.so' -Rootfs 'C:\SSCOnWindows\tools\ssc_daemon\rootfs'
  ```

### `error.encoder_init` 또는 인코더 없는 연결

`error.encoder_init`은 A2DP 코덱을 설정한 다음 선택한 인코더의 초기화 함수가 실패했다는 뜻입니다. SSC에서는 이전 빌드가 WSL/네이티브 프로세스 시작, 의존성, TCP 연결 실패를 한 상태로 표시했습니다. 주어진 로그만으로 어느 단계였는지 판별할 수 없습니다. 이번 빌드에서는 파일, 환경, 포트, 실제 프레임 생성 실패를 구분합니다. **SSC 준비 없이 Bluetooth 오디오를 시험하려면 코덱을 AAC 또는 SBC로 고른 뒤 연결**하세요. 이 선택은 SSC 데몬을 시작하지 않습니다.

## Windows Bluetooth 드라이버 복원

1. SSC 스트리밍을 중지하고 앱을 종료합니다. 장치 관리자에서 기록해 둔 **USB 동글의 하드웨어 ID와 장치 인스턴스**를 확인합니다.
2. 해당 USB 동글의 **속성 → 드라이버 → 드라이버 롤백**을 사용합니다. 롤백이 비활성화돼 있으면 **드라이버 업데이트 → 내 컴퓨터에서 드라이버 찾아보기 → 컴퓨터의 사용 가능한 드라이버 목록에서 직접 선택**에서 기존 서명된 Bluetooth 드라이버를 선택하거나 제조사의 공식 설치 프로그램을 사용합니다. A760의 경우 ipTIME의 BT60XR/BT60XR-A 공식 드라이버를 사용할 수 있습니다. [ipTIME 설치 안내](https://iptime.com/support/qna/900664)
3. 동글을 다시 연결하고 Windows가 재부팅을 요청하면 재부팅합니다. 장치 관리자에서 원래 공급자·드라이버와 경고 표시를 확인합니다. Windows Bluetooth 설정에서 다시 페어링하고 오디오를 재생합니다.
4. 내장 Qualcomm의 기존 연결, Alternative A2DP Driver를 통해 선택한 오디오 설정과 재생, `msinfo32`의 Secure Boot 상태를 각각 확인합니다. 다른 장치의 드라이버를 삭제하거나 시스템 전체의 Bluetooth 드라이버 패키지를 제거할 필요는 없습니다.

## 검증 범위

Windows CI의 컴파일, 단위 테스트, ZIP 검사가 통과하더라도 A760 RAM 패치와 BTstack 초기화, 검색·페어링, SM-R190의 44.1/48 kHz 오디오, Qualcomm 및 Alternative A2DP Driver 동작, Secure Boot 상태에서의 드라이버 복원을 실물 Windows PC에서 확인해야 합니다.
