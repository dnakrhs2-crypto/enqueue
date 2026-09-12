# 고정 FFmpeg SDK

`ffmpeg.lock.json`은 로컬 BtbN SDK 아카이브, include 헤더, lib 파일, bin의 모든 DLL·EXE와 PE 일반/지연 import를 고정한다. 폴더명에 남은 `latest`는 버전 선택에 쓰지 않는다. 감사·빌드·패키징은 다운로드하지 않으며, 정확한 버전과 SHA가 다르면 중단한다.

```powershell
python -B tools/recorder/audit_ffmpeg.py --sdk 'C:\Users\claude\SDKs\ffmpeg-lgpl\ffmpeg-n8.1-latest-win64-lgpl-shared-8.1' --expected-version 'n8.1.2-51-g7ba069f4f1-20260908' --report out/r32/ffmpeg-audit.json
```

기본 아카이브 경로는 SDK 부모 폴더의 lock에 기록된 파일명이다. 다른 위치라면 `--archive`를 지정한다. CMake 재검증에서도 원본 zip이 필요하다. lock 갱신은 명시적인 `--write-lock --archive <zip>` 유지보수 작업이며, 자동 재고정은 없다. 생성 시 zip 내부 바이트와 SDK 개별 파일을 대조한다. 이후 configure/package 검사는 lock의 개별 SHA를 사용한다.

CMake는 기존 `_rec_dlls` 복사 목록을 감사기에 전부 전달한다. 누락·추가·변경이 있으면 configure 실패다. 새 SDK나 전이 DLL을 추가할 때 목록만 늘려 우회하지 않고, 이 아카이브 잠금과 소스·고지 확인을 함께 갱신한다. NVIDIA 드라이버 DLL과 Windows/UCRT API 계약은 설치 전제이며 앱에 복사하지 않는다. 런타임 `LoadLibrary` 경로와 최저 OS 지원은 PE 정적 검사만으로 인증하지 않는다.

대응 소스는 저장소에 vendoring하지 않는다. [확보·검증·같은 릴리스에 묶는 절차](../../docs/validation/recorder/dependencies.md)와 [고지 목록](../licenses/THIRD-PARTY.md)을 따른다. 현재 lock의 `corresponding_source.status=UNVERIFIED`는 기술 감사 PASS와 별개다.
