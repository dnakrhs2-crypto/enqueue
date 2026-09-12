# Recorder 라운드 32 — 의존성·대응 소스

2026-09-09, 이 트리의 고정 SDK를 읽고 검사했다. 기술 감사는 PASS, 정확한 대응 소스·회사 계약·전체 제3자 고지는 **미확인 → 스파이크 8**이다. 저장소에 FFmpeg 소스나 바이너리를 vendoring하지 않았다. 지정 SDK에는 LICENSE.txt와 doc/가 있고 README 파일은 없었다.

| 검사 | 결과 |
|---|---|
| 버전 | `n8.1.2-51-g7ba069f4f1-20260908` |
| FFmpeg 커밋 식별자 | `7ba069f4f1` — 실행 파일 문자열 확인. 전체 40자리 커밋·다운로드한 소스와의 대응은 미확인 |
| 아카이브 SHA-256 | `b74c95a1976622f93f9c3ce73551683f4167646b155d9bfe9f2e132e5503e7eb` |
| 파일 | 헤더 144개, lib 파일 28개, DLL 7개, EXE 3개. 개별 SHA·크기는 lock에 기록 |
| PE import | 자체 PE 파서로 일반 import와 delay import 분석. 전이 누락 DLL 0개 |
| unpinned latest | 0. 폴더명을 버전으로 사용하지 않고, 다운로드 자동 갱신 없음 |
| MinGW runtime DLL | 별도 libgcc/libstdc++/libwinpthread/libgomp/libssp DLL 없음. 정적 포함 코드가 없다는 뜻은 아님 |
| 런타임 메타데이터 | `ffmpeg -version`, `-L`, 7개 DLL의 version/configuration/license API 검사 |
| 컴파일러 문자열 | `gcc 15.2.0 (crosstool-NG 1.28.0.23_185f348)` |
| 구성 | shared + disable-static + version3, GPL/nonfree 활성화 없음, x264/x265/fdk-aac 비활성화 |

DLL 전체 목록은 avcodec-62, avdevice-62, avfilter-11, avformat-62, avutil-60, swresample-6, swscale-9다. 실제 일반/지연 import 이름, Windows API/UCRT 의존성, 구성 옵션 원문과 API 반환값은 [lock](../../../recorder/third_party/ffmpeg.lock.json)에 있다. native AAC/NVENC 선택은 회사 계약 승인이나 장치 실행 성공의 근거가 아니다.

## URL과 SHA의 확인 범위

아래 SHA는 **로컬에 확보한 바이트**에 대한 값이다. 원격 서버에서 같은 바이트를 내려받았다는 검증과 구별한다. 네트워크의 Python 다운로드는 WinError 10013/10051로 막혔고, 웹 도구에서도 해당 FFmpeg 커밋의 API 응답을 얻지 못했다. 정확한 소스·BtbN 작업 메타데이터·toolchain archive SHA를 추정하지 않았다.

| 대상 / 다운로드 또는 조회 URL | SHA-256 / 상태 |
|---|---|
| 기존 로컬 `ffmpeg-n8.1-win64-lgpl-shared.zip`; 원본의 날짜 고정 다운로드 URL은 확인 필요 | `b74c95a1976622f93f9c3ce73551683f4167646b155d9bfe9f2e132e5503e7eb` |
| FFmpeg 소스 다운로드 후보: `https://github.com/FFmpeg/FFmpeg/archive/7ba069f4f1.tar.gz` | 미확인. 다운로드 성공·전체 커밋 확인 후 SHA 기록. 대응 소스 완료로 간주하지 않음 |
| BtbN 빌드 조회: [releases](https://github.com/BtbN/FFmpeg-Builds/releases), [Actions](https://github.com/BtbN/FFmpeg-Builds/actions) | 정확한 2026-09-08 빌드 job/run·레시피 커밋·artifact URL·SHA 미확인 |
| 레시피 다운로드 형식: `https://github.com/BtbN/FFmpeg-Builds/archive/<정확한-40자리-커밋>.tar.gz` | 다운로드 자리. 아직 사용 가능한 고정 URL/SHA 아님 |
| 제3자 소스·patch·툴체인 | 위 레시피·job 로그의 각 URL을 full commit/버전 및 SHA로 고정해야 함. 목록 미확보 |
| [LGPL v3 공식 본문](https://www.gnu.org/licenses/lgpl-3.0.txt); 로컬 SDK LICENSE.txt 원문을 동봉 | 로컬 본문 `da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768` |
| [GPL v3 공식 본문](https://www.gnu.org/licenses/gpl-3.0.txt); 로컬 Git `mingw64/share/licenses/xz/COPYING.GPLv3`의 표준 본문을 동봉 | 로컬 본문 `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |
| [WinSparkle 배포 저장소](https://github.com/vslavik/winsparkle); 로컬 0.9.4 SDK 고지 | COPYING `599d22b139b44f8f17140d09d16a4dd1fd6c864616135210b9308c7810d49caf`; COPYING.expat `31b15de82aa19a845156169a17a5488bf597e561b2c318d159ed583139b25e87` |

## 대응 소스를 릴리스 산출물로 확보하는 절차

1. 배포 담당은 위 zip 원본을 보존하고, 정확한 버전·개별 SHA와 대응하는 BtbN 빌드 job/run, workflow revision, head SHA, 빌드 로그, 컨테이너 digest를 확보한다. upstream의 현재 `master`나 날짜가 비슷한 tag로 대신하지 않는다. [BtbN 안내](https://github.com/BtbN/FFmpeg-Builds)는 `latest`가 이동하고 일일 빌드가 보존 기간 후 삭제될 수 있음을 설명하므로, 확보한 원본을 직접 릴리스 산출물로 보관한다.
2. FFmpeg의 `7ba069f4f1`을 전체 커밋으로 해소한 실제 소스를 받는다. 그 소스만으로 전체 BtbN 바이너리의 대응 소스가 완성됐다고 하지 않는다. 해당 job의 `scripts.d`, `patches`, `variants`, `addins`, build/download/generate/makeimage 스크립트와 하위 모듈을 **그 빌드의 커밋**으로 함께 받는다.
3. 모든 활성화 옵션 및 내부 전이 라이브러리를 빌드 로그·pkg-config 결과와 대조한다. [제3자 확인 목록](../../../recorder/licenses/THIRD-PARTY.md)은 현재 configure 재고다. 실제 다운로드 URL, 전체 VCS revision 또는 버전, 소스 아카이브 SHA, 적용한 patch·순서, 원저작권·라이선스 본문을 구성요소마다 보관한다. Windows DLL 외부에 드러나지 않는 정적 의존성도 포함한다.
4. GCC/crosstool-NG 문자열 외에 실제 MinGW-w64, binutils, C/C++ runtime, OpenMP 등 사용한 도구의 버전·소스·설정·예외 고지와 컨테이너 digest를 확보한다. 재현 명령은 해당 job 로그 그대로 기록한다. 지금 확인되지 않은 빌드 레시피나 도구 버전을 새로 만들어 넣지 않는다.
5. 별도 릴리스 staging에 `source-manifest.json`, FFmpeg·제3자 소스, patch, 레시피, toolchain 자료, README.rebuild, 고지, SBOM을 둔다. `Recorder-sources-0.1.0.zip`으로 묶어 개별 파일과 최종 zip SHA를 기록한다. 저장소에는 이 문서·고지·lock만 두고 소스 묶음은 릴리스 산출물로 관리한다.
6. 재빌드 로그와 DLL ABI/동적 교체 결과를 배포 담당이 검토한 뒤 lock의 `corresponding_source`에 최종 zip SHA, BtbN full commit, toolchain digest와 CONFIRMED 상태를 기록한다. 이 문서의 미확인 URL/SHA 표와 `licenses/release-gates.json`에 실제 검토 근거를 보충한다. 구조·해시 검사만으로 소스 완전성이나 계약을 승인하지 않는다.
7. `--source-bundle <zip>`을 사용해 **동일 릴리스의 설치기 옆**에 소스를 묶는다. ProductIdentity의 공개 repo/base URL/tag/source stem을 확정하면 앱 정보·사이트가 같은 릴리스의 소스로 연결될 수 있다. 현재 `.invalid`/UNCONFIRMED 값은 다운로드 버튼으로 노출하지 않으며 Recorder 게시 경로는 차단돼 있다. [FFmpeg 공식 배포 안내](https://ffmpeg.org/legal.html)에 따라 정확히 대응하는 소스, 공유 링크, 고지를 점검한다.

`source-manifest.json`의 schema_version은 1이다. `ffmpeg_commit`은 7ba069f4f1으로 시작하는 full commit, `btbn_recipe_commit`은 40자리 커밋, `toolchain_image_digest`는 `sha256:...`, `toolchain_versions`는 버전 자료다. `components`에는 구성요소별 name/version/source_paths/license_paths를 기록한다. `rebuild_instructions`와 `sbom`은 zip 안의 파일 경로다. `files`는 manifest 자신을 제외한 모든 파일의 `{path, size, sha256}` 목록이다. validator는 경로 탈출·중복·누락·바이트 불일치를 거부한다. 이것은 담당자의 소스 대응 검토를 대체하지 않는다.

## /MT 앱과 MinGW DLL 경계

앱/Recorder 타깃은 MSVC `/MT`이며 FFmpeg는 MinGW 공유 DLL이다. C API의 숫자·명시적 byte buffer·FFmpeg opaque handle만 경계를 통과시킨다. FFmpeg가 할당한 메모리는 해당 `av_free`, `av_frame_free`, `av_packet_free`, `avcodec_free_context` 등으로 반환한다. 앱의 `new/delete`, `malloc/free`로 해제하지 않는다. 반대로 앱 메모리를 DLL의 free에 주지 않는다. `FILE*`, CRT 파일 디스크립터, STL 객체, C++ 예외를 경계로 전달하지 않는다. callback의 수명·스레드·오류 변환도 호출자가 명시한다.

설치 폴더에 평문 DLL을 두어 ABI 호환 DLL 교체를 허용한다. 감사는 공급되는 원본 묶음의 무결성을 검사하며, 앱 시작 시 사용자가 교체한 DLL을 원본 SHA로 거부하는 기능은 넣지 않았다. 바꾼 DLL로 실행·할당/해제·종료를 검증하는 실험은 최종 본선/클린 PC에서 수행해야 한다. 이 라운드는 제한된 app 파일 외의 미디어 코드를 수정하거나 경계 전반을 실물 검증하지 않았다.

## 실행 증거

`python -B tools/recorder/audit_ffmpeg.py --sdk <고정 SDK> --expected-version n8.1.2-51-g7ba069f4f1-20260908 --report out/r32/ffmpeg-audit.json`: 종료코드 0. 원본 zip 대조를 포함한 최초 lock 생성도 0. CMake configure는 기존 `build/`를 쓰지 않는 `out/r33-build`에서 0. 감사 PASS는 U-11/U-12 해소나 출시 승인이 아니다.
