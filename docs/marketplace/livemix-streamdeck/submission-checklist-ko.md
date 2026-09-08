# LiveMix 1.0.0 Marketplace 제출 체크리스트

이 폴더는 고객이 제출할 파일과 문구입니다. 계정 생성·약관 동의·제출·게시가 실행된 상태는 아닙니다. 플러그인 UUID는 `com.gomtwigim.livemix`, manifest 버전은 `1.0.0.0`입니다.

## 제출 전 준비

- [ ] Claude가 [VALIDATION.md](../../../streamdeck/VALIDATION.md)의 1.0.0 결과와 패키지를 검토합니다. 고객의 0.9.1 + LiveMix 0.6.0 Mobile 성공 기록과 새 1.0.0 검증을 구분합니다.
- [ ] 아래 HTML 5개를 Chromium으로 정확한 캔버스 크기, 배율 1로 PNG 출력합니다. 배경 포함, 여백 없음. 자세한 산출물·내보내기는 [README.md](README.md)를 따릅니다.
- [ ] 새 아이콘의 1.0.0 패키지를 실제 Stream Deck 앱/Mobile에서 설치·업그레이드하고 ko/en 키 글자, 그룹 구분, 미연결 표시를 확인합니다.
- [ ] Stream Deck +에서 회전·짧게 누르기·누른 채 회전·터치 동작을 검증합니다. **실제 시연 영상**에 키/다이얼 조작과 LiveMix 변화, 재연결을 담고 영어 자막을 붙입니다. 1920×1080 MP4, 250 MB 미만; 내부 목표 50 MB 이하. 하드웨어 의존 제품의 시연 영상 요건에 대비합니다. [심사 안내](https://docs.elgato.com/maker-console/review-process/)
- [ ] LiveMix 0.6.0 이상 설치 파일이 [앱 릴리스](https://github.com/dnakrhs2-crypto/livemix/releases)에서 공개 다운로드되는지 확인합니다.
- [ ] Claude가 사이트의 설치·지원 내용을 공개한 뒤 [지원 URL](https://곰튀김.com/livemix/#streamdeck-support)이 설치법, 최소 버전, 오프라인/채널 없음 안내, 로그 위치와 실제 문의 방법을 제공하는지 확인합니다. 영어 문의를 맡을 담당자/연락 경로를 확정합니다.
- [ ] 제작자 이름 **Gomtwigim**이 실제 Maker 조직 이름과 일치하는지, 제품 이름 **LiveMix**와 UUID를 사용할 수 있는지 확인합니다. 태그라인은 요청된 **Microphones and FX at your fingertips**를 사용합니다.

## Maker Console에서 제출

1. [Maker Console](https://maker.elgato.com/)에 고객 계정으로 로그인합니다. 처음이면 Maker 조직을 만들고 이름·프로필·연락처를 입력합니다. 조직은 제품에 표시되는 제작자입니다. [시작 안내](https://docs.elgato.com/maker-console/getting-started/)
2. 조직 Settings에서 **Maker Agreement**를 읽고 권한 있는 고객이 동의합니다. 이 문서는 고객을 대신해 약관에 동의하지 않습니다. 약관은 조직 Settings에서 확인할 수 있습니다. [제품 가이드](https://docs.elgato.com/guidelines/products/)
3. Home → **Create product**에서 제품 종류 **Stream Deck plugin**을 선택합니다.
4. Files 단계에 **`streamdeck/dist/com.gomtwigim.livemix.streamDeckPlugin`**을 올립니다. HTML, 소스 ZIP, LiveMix 설치 파일을 올리는 칸이 아닙니다. 업로드 후 UUID와 1.0.0.0 버전을 확인합니다.
5. Details에 이름 **LiveMix**, [listing-en.md](listing-en.md)의 Description 세 문단을 입력합니다. 제품 종류를 이름 뒤에 덧붙이지 않습니다. 카테고리는 **Audio**를 선택합니다. Console에 보이는 관련 태그 중 LiveMix / microphone / audio / streaming / effects를 적용하고 무관한 태그는 넣지 않습니다. 가격은 **Free / 무료**로 설정합니다. 이름·수익화 방식은 생성 후 Console에서 직접 변경할 수 없으므로 여기서 확인합니다.
6. 요구사항을 Windows 10/11 x64, LiveMix 0.6.0+, Stream Deck 7.1+로 맞춥니다. 키는 하드웨어와 Mobile, 다이얼은 Stream Deck +입니다. Additional links / Support에 **https://곰튀김.com/livemix/#streamdeck-support**와 앱 다운로드 링크를 넣습니다. 한국어 설명란이 제공되면 [listing-ko.md](listing-ko.md)를 추가합니다.
7. Media에 **app-icon.png 288×288**, **thumbnail.png 1920×960**, **gallery-01-microphones.png / gallery-02-groups.png / gallery-03-send.png 각 1920×960**을 올립니다. 갤러리는 최소 3개, 최대 10개입니다. 실제 시연 MP4와 1920×960 영상 썸네일도 준비해 Console의 영상 입력 또는 심사팀이 지정한 경로로 제공합니다. 모든 캡션은 영어로 유지합니다.
8. Release notes에 [release-notes-en.md](release-notes-en.md)의 1.0.0 내용을 붙입니다. 심사자 메모/첨부란에는 [review-notes-en.md](review-notes-en.md)를 제공합니다. 별도 입력란이 없으면 제출 후 심사 이메일에 제품 ID와 함께 전달합니다. LiveMix 본체는 한국어이며 ASIO 없이도 제어 시험이 가능하다는 설명을 포함합니다.
9. 최종 검토에서 **Automatically publish after being approved**를 반드시 **해제**합니다. 기본 자동 게시 대신 승인 뒤 고객이 수동으로 게시하도록 합니다.
10. 파일·문구·미디어 미리보기를 확인한 뒤 **Submit for review**를 누릅니다. 제품 ID, 제출 버전, 제출 날짜를 고객이 기록합니다. 위 Files → Details → Media / Release notes 순서는 [공식 제출 안내](https://docs.elgato.com/maker-console/submitting-products/)를 기준으로 합니다.

## 심사 이후

11. 고객 계정 이메일과 스팸함을 확인합니다. 심사 소통은 **maker@elgato.com**과 이메일로 진행되며 보통 4–10 영업일을 잡습니다. 승인 날짜를 보장하는 일정은 아닙니다. 자동 게시 해제 상태에서는 승인 후 **Approved**로 남도록 확인합니다. [심사 상태와 자동 게시 옵션](https://docs.elgato.com/maker-console/review-process/)
12. Marketplace가 처리한 패키지를 내려받을 수 있게 되면 실제 PC에서 설치·실행·PI·0.9.1 업그레이드·키·다이얼을 다시 검증합니다. DRM 처리본의 해시가 직접 배포본과 같다고 가정하지 않습니다.
13. 수정 요청이 오면 문구/자산을 고친 버전 또는 수정본을 제출하고 다시 검증합니다. 이미 공개한 버전 파일을 다른 내용으로 덮어쓰지 않습니다.
14. 의존 앱·지원 사이트·처리본 검증이 완료되면 고객이 승인된 버전을 수동으로 게시합니다. 실제 Marketplace URL이 생긴 뒤 Claude가 사이트에 연결합니다.

직접 배포용 플러그인을 같은 GitHub 저장소에 공개할 때는 앱 릴리스의 latest/appcast를 바꾸지 않는 별도 플러그인 릴리스를 사용하도록 Claude에게 전달합니다. 이 작업에서는 사이트·GitHub 릴리스·앱 설치 파일을 변경하지 않았습니다.
