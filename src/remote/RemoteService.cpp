#include "remote/RemoteService.h"
#include "RemoteWebData.h"
#include "app/AppVersion.h"

namespace gocue::remote
{
namespace
{
std::vector<Server::Asset> assets()
{
    std::vector<Server::Asset> result;
    for (int i = 0; i < RemoteWebData::namedResourceListSize; ++i)
    {
        const juce::String name (RemoteWebData::originalFilenames[i]);
        juce::String path, mime;
        if (name == "index.html") { path = "/"; mime = "text/html; charset=utf-8"; }
        else if (name == "app.css") { path = "/app.css"; mime = "text/css; charset=utf-8"; }
        else if (name == "app.js") { path = "/app.js"; mime = "text/javascript; charset=utf-8"; }
        else if (name.endsWith (".svg")) { path = "/icons/" + name; mime = "image/svg+xml"; }
        else continue;
        int size = 0;
        const char* bytes = RemoteWebData::getNamedResource (RemoteWebData::namedResourceList[i], size);
        result.push_back ({ path.toStdString(), mime.toStdString(), bytes, static_cast<size_t> (size) });
    }
    return result;
}
}
Service::Service (AppSettings& s, ProjectDocument& d, Callbacks cb, NetworkDiscovery discovery)
    : settings (s), document (d), callbacks (std::move (cb)), discoverNetworks (std::move (discovery)), model (d)
{
    accessKey = settings.getRemoteKey();
    if (accessKey.isEmpty()) { accessKey = randomKey(); settings.setRemoteKey (accessKey); }
    serverId = settings.getRemoteInstallId();
    if (serverId.isEmpty()) { serverId = juce::Uuid().toString(); settings.setRemoteInstallId (serverId); }
    loopback = juce::SystemStats::getEnvironmentVariable ("ENQUEUE_REMOTE_LOOPBACK", {}) == "1";
    refreshNetworks();
    server = std::make_unique<Server> (assets(), Server::Callbacks {
        [this] (uint64_t id) { enqueue ({ EventKind::opened, id, juce::Time::getMillisecondCounterHiRes(), {} }); },
        [this] (uint64_t id) { enqueue ({ EventKind::closed, id, juce::Time::getMillisecondCounterHiRes(), {} }); },
        [this] (uint64_t id, const std::string& text)
        {
            const double time = juce::Time::getMillisecondCounterHiRes();
            enqueue ({ EventKind::request, id, time, Request::parse (juce::String::fromUTF8 (text.data(), static_cast<int> (text.size()))) });
        } });
}
Service::~Service() { stop ("off"); }
void Service::restoreEnabled (bool safeMode) { if (! safeMode && settings.getRemoteEnabled()) setEnabled (true); }
void Service::start (int preferredPort)
{
    { std::lock_guard<std::mutex> lock (queueMutex); accepting = true; }
    if (accessKey.isNotEmpty()) port = server->start (accessKey.toStdString(), loopback, preferredPort);
    lastStateMs = -1e9;
    sentGeneration = 0;
    sendSynchronousChangeMessage();
}
void Service::stop (const char* reason)
{
    // No new input -> invalidate queued delivery -> release this socket's holds -> close -> destroy.
    { std::lock_guard<std::mutex> lock (queueMutex); accepting = false; incoming.clear(); }
    cancelPendingUpdate();
    for (const auto& device : connected) if (callbacks.release) callbacks.release (device.first);
    connected.clear();
    if (server != nullptr) server->stop (reason);
    port = 0;
}
void Service::setEnabled (bool value)
{
    if (value == enabled && (! value || port != 0)) return;
    enabled = value;
    settings.setRemoteEnabled (enabled);
    if (enabled) { refreshNetworks(); start(); }
    else { stop ("off"); sendSynchronousChangeMessage(); }
}
void Service::renewKey()
{
    const juce::String replacement (randomKey());
    if (replacement.isEmpty()) return;
    const int previousPort = port == 0 ? 49820 : port;
    stop ("newkey");
    accessKey = replacement;
    settings.setRemoteKey (accessKey);
    if (enabled) start (previousPort);
    else sendSynchronousChangeMessage();
}
void Service::refreshNetworks()
{
    // The explicit loopback test mode keeps its synthetic address; production candidates are RFC 1918 only.
    auto next = discoverNetworks ? discoverNetworks() : loopback ? std::vector<NetworkAddress> { { "loopback", "Loopback", "127.0.0.1", true } } : networkAddresses();
    const auto preference = settings.getRemoteAdapter().toStdString();
    const auto previousIp = juce::isPositiveAndBelow (selected, static_cast<int> (addresses.size()))
        ? addresses[static_cast<size_t> (selected)].ip : std::string();
    int choice = next.empty() ? -1 : 0;
    for (size_t i = 0; i < next.size(); ++i) if (next[i].adapter == preference) { choice = static_cast<int> (i); break; }
    for (size_t i = 0; i < next.size(); ++i)
        if (next[i].adapter == preference && next[i].ip == previousIp) { choice = static_cast<int> (i); break; }
    bool different = next.size() != addresses.size() || choice != selected;
    for (size_t i = 0; ! different && i < next.size(); ++i)
        different = next[i].adapter != addresses[i].adapter || next[i].ip != addresses[i].ip || next[i].name != addresses[i].name || next[i].recommended != addresses[i].recommended;
    addresses = std::move (next);
    selected = choice;
    if (different) sendSynchronousChangeMessage();
}
void Service::selectNetwork (int index)
{
    if (! juce::isPositiveAndBelow (index, static_cast<int> (addresses.size()))) return;
    selected = index;
    settings.setRemoteAdapter (addresses[static_cast<size_t> (index)].adapter);
    sendSynchronousChangeMessage();
}
juce::String Service::address() const
{
    if (! enabled || port == 0 || selected < 0) return {};
    return "http://" + juce::String (addresses[static_cast<size_t> (selected)].ip) + ":" + juce::String (port);
}
juce::String Service::link() const { const auto url = address(); return url.isEmpty() ? juce::String() : url + "/#k=" + accessKey; }
void Service::enqueue (Event event)
{
    std::lock_guard<std::mutex> lock (queueMutex);
    if (! accepting) return;
    incoming.push_back (std::move (event));
    triggerAsyncUpdate();
}
void Service::send (uint64_t id, const juce::var& value) { server->send (id, encode (value).toStdString()); }
void Service::broadcast (const juce::var& value)
{
    const auto json = encode (value).toStdString();
    for (const auto& device : connected) server->send (device.first, json);
}
juce::var Service::stateMessage()
{
    auto value = model.state (snapshot, callbacks.showMode());
    value.getDynamicObject()->setProperty ("seq", ++sequence);
    value.getDynamicObject()->setProperty ("now", static_cast<juce::int64> (juce::Time::getMillisecondCounterHiRes()));
    return value;
}
void Service::handleAsyncUpdate()
{
    std::deque<Event> events;
    { std::lock_guard<std::mutex> lock (queueMutex); events.swap (incoming); }
    for (const auto& event : events)
    {
        if (event.kind == EventKind::opened)
        {
            connected.emplace (event.connection, Device { event.connection, {}, juce::Time::getCurrentTime(), {} });
            send (event.connection, object ({ { "t", "hello" }, { "proto", 1 }, { "server", serverId },
                { "now", static_cast<juce::int64> (juce::Time::getMillisecondCounterHiRes()) },
                { "pc", juce::SystemStats::getComputerName() }, { "project", document.getDisplayName() }, { "app", appVersionString() } }));
            send (event.connection, model.lists (callbacks.showMode()));
            send (event.connection, stateMessage());
            sendSynchronousChangeMessage();
        }
        else if (event.kind == EventKind::closed)
        {
            if (callbacks.release) callbacks.release (event.connection);
            connected.erase (event.connection);
            sendSynchronousChangeMessage();
        }
        else if (auto device = connected.find (event.connection); device != connected.end())
        {
            auto response = device->second.session.dispatch (event.request, [&] (const Request& request)
            {
                if (request.type == "hi")
                {
                    device->second.name = request["device"].toString();
                    sendSynchronousChangeMessage();
                    return juce::Result::ok();
                }
                if (request.type == "ping") return juce::Result::ok();
                return callbacks.execute (event.connection, request, event.timeMs);
            });
            send (event.connection, response);
        }
    }
}
void Service::publish (const PlaybackSnapshot& value)
{
    snapshot = value;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastNetworkMs >= 1000) { lastNetworkMs = now; refreshNetworks(); }
    if (connected.empty()) return;
    const auto lists = model.lists (callbacks.showMode());
    const auto generation = static_cast<juce::int64> (lists["gen"]);
    if (generation != sentGeneration) { sentGeneration = generation; broadcast (lists); }
    const auto state = model.state (snapshot, callbacks.showMode());
    const auto json = encode (state);
    const bool busy = snapshot.running != 0 || ! snapshot.waits.empty();
    if ((busy && now - lastStateMs >= 100.0) || (! busy && (json != lastState || now - lastStateMs >= 1000.0)))
    {
        lastStateMs = now;
        lastState = json;
        state.getDynamicObject()->setProperty ("seq", ++sequence);
        state.getDynamicObject()->setProperty ("now", static_cast<juce::int64> (now));
        broadcast (state);
    }
}
}
