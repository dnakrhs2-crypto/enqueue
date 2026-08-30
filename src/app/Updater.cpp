#include "app/Updater.h"

#include <juce_events/juce_events.h>

#if GOCUE_HAS_WINSPARKLE
 #include <winsparkle.h>
#endif

#ifndef GOCUE_APPCAST_URL
 #define GOCUE_APPCAST_URL ""
#endif

#ifndef GOCUE_EDDSA_PUBLIC_KEY
 #define GOCUE_EDDSA_PUBLIC_KEY ""
#endif

namespace gocue
{

namespace
{
    Updater::Callbacks storedCallbacks;
    bool initialised = false;

    bool isConfigured()
    {
        return juce::String (GOCUE_APPCAST_URL).isNotEmpty() && juce::String (GOCUE_EDDSA_PUBLIC_KEY).isNotEmpty();
    }

   #if GOCUE_HAS_WINSPARKLE
    int __cdecl canShutdownThunk()
    {
        return (! storedCallbacks.canShutdown || storedCallbacks.canShutdown()) ? 1 : 0;
    }

    void __cdecl shutdownRequestThunk()
    {
        if (storedCallbacks.requestShutdown)
            storedCallbacks.requestShutdown();
    }
   #endif
}

bool Updater::isAvailable()
{
    return GOCUE_HAS_WINSPARKLE != 0 && isConfigured();
}

juce::String Updater::getAppcastUrl()
{
    return GOCUE_APPCAST_URL;
}

void Updater::initialise (const juce::String& companyName, const juce::String& appName,
                          const juce::String& version, Callbacks callbacks)
{
   #if GOCUE_HAS_WINSPARKLE
    if (initialised || ! isConfigured())
        return;

    storedCallbacks = std::move (callbacks);

    win_sparkle_set_appcast_url (GOCUE_APPCAST_URL);
    win_sparkle_set_app_details (companyName.toWideCharPointer(), appName.toWideCharPointer(), version.toWideCharPointer());
    win_sparkle_set_eddsa_public_key (GOCUE_EDDSA_PUBLIC_KEY);
    win_sparkle_set_can_shutdown_callback (canShutdownThunk);
    win_sparkle_set_shutdown_request_callback (shutdownRequestThunk);
    win_sparkle_set_automatic_check_for_updates (1);   // silent background check; UI only when an update exists
    win_sparkle_init();
    initialised = true;
   #else
    juce::ignoreUnused (companyName, appName, version, callbacks);
   #endif
}

void Updater::checkForUpdatesWithUI()
{
   #if GOCUE_HAS_WINSPARKLE
    if (initialised)
        win_sparkle_check_update_with_ui();
   #endif
}

void Updater::shutdown()
{
   #if GOCUE_HAS_WINSPARKLE
    if (initialised)
    {
        win_sparkle_cleanup();
        initialised = false;
    }
   #endif

    storedCallbacks = {};
}

} // namespace gocue
