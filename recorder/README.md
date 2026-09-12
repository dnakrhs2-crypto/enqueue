# Tally (탤리)

Windows x64용 웹캠/ASIO 녹화·편집 앱의 개발 트리다. 제품 이름은 Tally(탤리, 2026-09-13 확정)이고 공개 저장소는 dnakrhs2-crypto/recorder다. 지원 최소 OS는 아직 확정되지 않았다. 병행 라운드의 미디어 기능·종료 경로는 최종 본선에서 병합·재검증해야 한다.

배포 준비는 [의존성·대응 소스](../docs/validation/recorder/dependencies.md), [설치·업데이트 검증](../docs/validation/recorder/install-update.md)을 따른다. FFmpeg LGPL v3-or-later 공유 DLL 7개와 헤더/아카이브를 [lock](third_party/ffmpeg.lock.json)으로 고정한다. 고지는 [licenses](licenses/NOTICE.txt)에 있다. JUCE/ASIO/AVC/AAC 계약은 **배포 담당·CEO 확인** 항목이다.

```powershell
python -B -m unittest discover -s tools/recorder/tests
python -B tools/release.py --app recorder --preset local --package-only
```

패키징 전에 CMake와 WinSparkle 도구를 준비하고 `GOCUE_EDDSA_PRIVATE_KEY_FILE`에 저장소 밖 실제 키 경로를 지정한다. `out/recorder-package-build`에서 모든 타깃·테스트를 실행하고, `out/release/recorder` 아래 고유한 후보 폴더에 설치기·manifest·서명·사이트 미리보기를 쓴다. 테스트 실패는 중단 사유다. 기존 `build/`·다른 앱의 site/appcast/latest·GitHub·tag를 변경하지 않는다. 이 모드는 게시하지 않으며, 현재 Recorder의 공개 게시 요청은 차단된다.

라운드 28은 `RecorderUpdater::Callbacks.canShutdown`에 실제 종료 조건을 제공하고 앱 수명·메뉴를 연결해야 한다. 이 라운드에는 callback 훅과 버전/FFmpeg/LGPL/동일 릴리스 소스를 표시하는 정보창 API만 있다. callback 기본 허용을 녹화·더빙·복구·export·미저장 상태 안전성 완료로 해석하지 않는다.

프로젝트는 설치 폴더 밖 사용자 로컬 폴더에 보관한다. 업데이트·제거는 프로젝트·설정 폴더를 지우지 않는다. 정전·강제 종료 뒤에는 앱이 제공하는 복구 상태와 실제 손실 보고를 확인하고 원본을 보존한다. 클린 PC 설치/업데이트·동적 DLL 교체·계약·정확한 대응 소스 묶음은 아직 미확인이다.
