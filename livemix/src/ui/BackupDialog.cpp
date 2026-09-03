#include "BackupDialog.h"

#include "WebDavBackup.h"
#include "Widgets.h"

namespace gocue::livemix
{

void BackupDialog::show (MixDocument& document, LiveMixSettings& settings, juce::Component* centreAround,
                         std::function<void (const juce::String&, bool)> status)
{
    if (settings.getBackupUrl().isEmpty() || settings.getBackupUser().isEmpty())
    {
        status (ko ("설정에서 온라인 백업 주소와 사용자를 먼저 넣으세요"), true);
        return;
    }

    const bool needPassword = ! settings.getBackupRememberPassword() || settings.getBackupPassword().isEmpty();
    auto* alert = new juce::AlertWindow (ko ("온라인 백업"),
                                         ko ("크리에이터 이름을 넣으면 세션이 시놀로지에 저장됩니다.\n") + settings.getBackupFolder() + "/"
                                             + juce::SystemStats::getComputerName() + ko ("/크리에이터이름_날짜시간.livemix"),
                                         juce::MessageBoxIconType::NoIcon, centreAround);
    alert->addTextEditor ("creator", settings.getBackupCreator(), ko ("크리에이터 이름"));

    if (needPassword)
        alert->addTextEditor ("password", "", ko ("비밀번호 (") + settings.getBackupUser() + ")", true);

    alert->addButton (ko ("백업"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<juce::Component> safeCentre (centreAround);

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([alert, &document, &settings, status, needPassword] (int result)
    {
        if (result != 1)
            return;

        const auto creator = alert->getTextEditorContents ("creator").trim();

        if (creator.isEmpty())
        {
            status (ko ("크리에이터 이름이 비어 있습니다"), true);
            return;
        }

        settings.setBackupCreator (creator);
        WebDavBackup::Target target;
        target.baseUrl = settings.getBackupUrl();
        target.folder = settings.getBackupFolder();
        target.user = settings.getBackupUser();
        target.password = needPassword ? alert->getTextEditorContents ("password") : settings.getBackupPassword();

        if (needPassword && settings.getBackupRememberPassword())
            settings.setBackupPassword (target.password);

        const auto remotePath = WebDavBackup::remotePathFor (target.folder, juce::SystemStats::getComputerName(), creator, juce::Time::getCurrentTime());
        status (ko ("백업 중... ") + remotePath, false);
        WebDavBackup::upload (target, document.getFile(), remotePath, [status] (bool ok, const juce::String& message) { status (message, ! ok); });
    }), true);
}

} // namespace gocue::livemix
