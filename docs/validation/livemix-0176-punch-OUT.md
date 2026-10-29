**LiveMix 0.17.6 합방 펀칭 수정 결과 (2026-10-29)**

작업 브랜치 `livemix-0176-punch`, 기준 `720a0b27d42f6c1b9032817f3f8d14dee4fe3922`. 커밋하지 않았다. `matrix_done.flag` 생성 시각은 10:01:07 KST, 대기 루프가 10:01:37에 확인했다. 그 전에는 빌드·시험을 실행하지 않았고, 모든 빌드·시험은 `rb.py`로 roomc에서 수행했다.

| 바꾼 파일 | 함수 및 동작 |
|---|---|
| `livemix/src/CollabTransport.h`, `.cpp` | `startHost`, `startGuest`, `Impl::HelloWarmup`, `sendV1Control`, `sendControl`, `send`, `receiveV1`, `receiveV2`, `receiveLoop`, `open`. v2와 legacy2의 초기 hello에 같은 TTL 처리를 사용한다. 소켓을 열 때 실제 기본 TTL을 읽고, 해당 hello 전송 동안만 TTL을 낮춘 뒤 즉시 원복한다. 방장 링크의 패킷을 받거나 제한 시간이 지나면 다음 hello부터 기본 TTL을 사용한다. 방장 punch·welcome, 일반 제어·오디오 패킷은 기본 TTL을 유지한다. |
| `livemix/src/CollabSession.h`, `.cpp` | `Options`, `joinRoom`, `startTransport`. 참가 응답의 `nativeState == "punch"`이고 `sameNetwork == false`일 때만 적용한다. 스냅샷 없는 옛 서버, 포트 열린 방, 같은 망은 기존 동작이다. 기본값은 `punchHelloTtl = 3`, `punchHelloMs = 3000`. |
| `livemix/src/CollabSession.h`, `.cpp` | `peerChanged`, `finishGuestLink`, `timerCallback`, `leave`. 방장 링크의 연결 이력을 참가 단위로 관리한다. 처음부터 실패하면 기존 문구 **방장 PC에 연결하지 못했습니다**, 연결 후 끊기면 기존 **연결 끊김**, 실제 `peerLeft`/bye는 **방장이 방을 닫았습니다**를 쓴다. legacy의 연결 후 재접속 동작은 유지한다. |
| `livemix/src/CollabSession.cpp` | `localInfo`. `appVersionString()`으로 빌드 버전 정보를 넣는다. 아래 사용처 확인에 따라 HTTP 호환 판단용 값은 바꾸지 않았다. |
| `tests/LiveMixCollabSessionTests.cpp` | `PunchGuest`, `LiveMixCollabPunchTests`, `LiveMixCollabPunchOptionsTests`. 실제 UDP TTL, 3초 복원, 방장 패킷 수신 후 복원, 정상 방·LAN·옛 서버, 실패·끊김·bye·재참가, Options 변경을 검사한다. |
| `tests/LiveMixCollabUdpPeer.h` | `UdpPeer`. Windows `IP_RECVTTL`과 `WSARecvMsg`로 수신한 패킷의 TTL을 읽는다. |

| roomc Release 시험 | 통과 | 실패 | 실행 시간 |
|---|---:|---:|---:|
| 수정 전, 새 `LiveMix Collab punch start` 시험 + 기준 커밋의 전송·세션 소스 | 68 | 10 | 26초 |
| 수정 후, 동일한 `LiveMix Collab punch start` | 78 | 0 | 32초 |
| 수정 후, `LiveMix Collab` 전체 (Options 변경 시험 포함) | 19,469 | 0 | 226초 |
| 수정 후, `LiveMix WebGuest` 전체 | 436 | 0 | 4초 |

수정 전 실패 10개는 초기 TTL 관련 6개, v2의 최초 연결 실패 상태·문구 2개, 연결 후 끊김 문구 1개, 빌드 버전 정보 1개다. 실제 첫 hello TTL은 v2·legacy2 모두 128이었다. 수정 후에는 첫 TTL 3, 방장 punch를 받은 뒤 TTL 128, 무응답 시 3초 후 TTL 128을 모두 확인했다. Options를 TTL 5·600ms로 바꾼 두 전송 경로의 시험도 통과했다. 포트 열린 방·같은 망·스냅샷 없는 응답, 방장 punch·welcome은 처음부터 기본 TTL을 사용한다. 전체 Collab 시험에는 일반 LiveMix 합방, 3인, 웹 손님, 나가기·재참가, 기존 legacy 재접속이 포함된다.

시험 로그:

- `out/livemix0176-punch/before-livemix-collab-punch-start.log`
- `out/livemix0176-punch/after-livemix-collab-punch-start.log`
- `out/livemix0176-punch/after-livemix-collab.log`
- `out/livemix0176-punch/after-livemix-webguest.log`
- 같은 폴더의 `before-configure.log`, `before-build.log`, `after-build.log`, `validation_driver.py.txt`.

ICMP 확인: Windows 문서는 TTL 만료를 `recvfrom`의 `WSAENETRESET`으로 설명한다. 현재 `receiveLoop`의 `bytes <= 0` 분기는 상태·수신 시각·warmup을 바꾸지 않고 다음 수신으로 진행하며, 그 동작을 유지했다. ICMP Time Exceeded 자체를 이번 루프백 시험에서 주입하지는 않았다. [Microsoft recvfrom 문서](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-recvfrom), [IP_TTL·IP_RECVTTL 문서](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ip-socket-options).

버전 판단 근거:

1. `localInfo().appVersion`은 `writePeerInfo`/`readPeerInfo`를 통한 UDP hello/welcome 및 legacy 명단의 `appVersion`, `PersonSnapshot`으로 전달된다. 전송 협상은 패킷 버전·capabilities·offer·wireRate로 판단하며 이 문자열을 버전 비교에 쓰지 않는다. 현재 `CollabWindow`는 이 버전 문자열을 화면에 직접 그리지 않는다. v2 서버 명단은 서버의 회원 정보가 원본이다.
2. 서버 `server/ytdown/livemix_collab.py::_supports`는 별도로 받은 `appVersion >= 0.17.0`을 호환 판단에 사용한다. 그러나 `CollabSession.cpp::clientInfo`는 `me.appVersion`을 복사하지 않는다. 서버에 전송하는 값은 `CollabRoom.h::ClientInfo.appVersion`의 독립적인 기본값 `0.17.0`이다. 이 경로와 값은 유지했고, HTTP join 요청을 검사하는 시험으로 확인했다.
3. 따라서 요청한 `localInfo` 하드코딩만 기존 `appVersionString()`으로 교체했다. `gocue_add_app_version`이 타깃의 `JUCE_VERSION`으로 생성하며, LiveMix와 Probe의 타깃 버전은 CMake `LIVEMIX_VERSION`이다. 현재 두 타깃 값은 `0.17.5`이고 버전 번호 자체를 올리지는 않았다. 통합 시험 실행 파일 EnqueueTests는 자체 프로젝트 버전 `0.17.4`를 사용하므로 수정 전 버전 시험의 기대값은 `0.17.4`였다.

`LiveMix`, `EnqueueTests`, `LiveMixCollabProbe` Release 빌드가 성공했다. Probe 폴더는 `rb.py --fetch build/vs2022/LiveMixCollabProbe_artefacts/Release`로 이 PC에 가져왔다.

- 실행 파일: `build/vs2022/LiveMixCollabProbe_artefacts/Release/LiveMixCollabProbe.exe`
- 크기: 13,316,096 bytes
- SHA-256: `701D811CD558F388B460F75F7A5DAF4CAF702F9D8CC553C888E1FABD7810F51F` (roomc와 이 PC 일치)

실제 세 PC·공유기 재측정은 아직 수행하지 않았다. 사무실 방장·강남 펀칭 방장·보통 방의 재측정에 위 Probe를 사용할 수 있다.
