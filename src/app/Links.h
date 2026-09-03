#pragma once

namespace gocue::Links
{
    /** Beta feedback room (KakaoTalk open chat). Empty until the room exists: the help-menu item stays disabled. */
    inline constexpr const char* feedbackChat = "https://open.kakao.com/o/pST4IRLi";

    /** Desktop shortcut target offered by the installer: our redirect page, which forwards to the Coupang Partners link
        (the page can be repointed without touching installed shortcuts). */
    inline constexpr const char* coupangShortcut = "https://dnakrhs2-crypto.github.io/gocue/coupang/";
}
