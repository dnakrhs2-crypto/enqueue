# fix-014-seam OUT

파일 수정·Recorder/RecorderTests/RecorderProbe Release 빌드·헤드리스 검증 완료. 실행기 OUT 환경 경로가 없어 이 파일을 사용했다. 커밋은 사용자에게 남겼다.

원인: 기준 VideoPlaybackEngine.cpp:621–641에서 직렬 다음 클립 프리페치가 매 진행 요청마다 IDR 프리픽스를 취소하고, :699–705에서 준비 큐를 덮어썼다. :1073의 seek 세대 변경 때 직전 프레임 폐기와 후속 검정 Present가 스크럽 검정 38/42회를 만들었다. 동일 세대 일반 재생은 기준도 직전 화면을 유지해 이번 검정 제출 0개였으며, 프레임 지연이 재현됐다.

수정 위치:

- `recorder/src/playback/VideoPlaybackEngine.cpp:419,638`: 경계 250ms 전 독립 IDR 프리롤, 첫 2프레임과 warm DPB 승계, 세대/계획/파일 취소.
- 같은 파일 `:607,629`: 현재 2+다음 2의 유한 준비 큐 유지.
- `recorder/src/playback/VideoPlaybackEngine.h:105`, cpp `:1225,1251,1263`: 마지막 제출 유지, 진짜 gap/epoch 폐기 시 클리어, 첫 프레임 전 빈 제출 방지, 유지 화면의 새 seek receipt 위조 방지.
- `recorder/tests/CutSeamChecks.h`, `PlaybackTests.cpp`: 기존 suite에 회귀 8건과 선택적 실제 미디어 3건 추가.

측정: 동일 파일/두 파일/구간 삭제 당기기에서 80ms 주입 경계 ±10프레임의 정확 프레임 부재 **16/15/16 → 0/0/0**, 검정 **0/0/0 → 0/0/0**, 지연 **100.969/85.032/100.533 → 0.640/1.098/0.888ms**. 스크럽 검정 **38 → 0**(직전 그림 유지 38회). 실제 H.264/WAV도 세 경우 각각 21/21 정확, 검정·PTS/순서 오류·PCM oracle 오차·underrun 모두 0. 기존 3ms MicroFade 유지.

실제 정지 커서→첫 프레임/PCM 준비는 전 **339.824/261.433/276.598ms** → 후 **302.727/249.936/263.413ms**, 평균 **292.618 → 272.026ms**. 별도 인덱싱/물리 DXGI/ASIO 시간은 제외하므로 GUI 약 450ms와 동일한 측정은 아니다. 측정한 준비 경로는 2초 이내이고 악화되지 않았다.

검증: 기준 기존 바이너리 48 suite/554건 → 최종 **48 suite/563건, 단독 전체 두 번 통과**. 증가분은 신규 seam 8건과 HEAD에 이미 있던 EditJournalTests.cpp:168의 1건이다. 해당 파일은 수정하지 않았다. 실제 미디어 옵션 playback-engine **43건 통과**. 3개 타깃 최종 빌드 성공(`-m:1 -nr:false`). 원본 복사·복사본 lock 삭제 후 검증했고, 캡처보드·ASIO 실장치/실제 HWND는 열지 않았다. 원본 11개 파일과 복사본 해시 일치, 릴리스 초안 해시 보존.

다른 세션 담당 파일 수정: **없음**. ProductIdentity/버전/릴리스 노트/site 변경 없음. 커밋·push·release.py 실행 없음.

남은 위험: **전체 suite와 실제 미디어 suite를 동시에 실행한 중간 1회가 `0xC0000005`로 종료됐다. 모듈/원인은 미확인**이며 이후 단독 전체 통과로 해결됐다고 주장하지 않는다. 실패 로그를 보존했다. 250ms를 넘는 준비/GPU 지연에서는 화면 유지·프레임 건너뜀 가능. 실제 DXGI/광학·DAC, 대표의 특정 GUI 실행, 4K 장시간 포화는 미검증이다.

전체 원인·수정·21행 표·측정 정의: [cut-seam.md](cut-seam.md). 원시 기록: [measurements.json](cut-seam-traces/measurements.json). 빌드/테스트/실패 로그: `build/seam-validation`.
