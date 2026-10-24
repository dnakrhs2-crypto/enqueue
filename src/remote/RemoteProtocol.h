#pragma once
#include <juce_core/juce_core.h>
#include <functional>

namespace gocue::remote
{
juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields);
juce::String encode (const juce::var&);
juce::var ack (juce::int64 id, const juce::Result&);
juce::var bye (const juce::String& reason);

struct Request
{
    juce::int64 id = 0;
    juce::String type;
    juce::var data;
    juce::Result error = juce::Result::ok();
    juce::var operator[] (const char* field) const { return data[juce::Identifier (field)]; }
    static Request parse (const juce::String& json);
};

/** IDs increase within one socket. Replayed requests only repeat the acknowledgement. */
class Session
{
public:
    juce::var dispatch (const Request&, const std::function<juce::Result (const Request&)>&);
private:
    juce::int64 lastId = 0;
    juce::Result lastResult = juce::Result::ok();
};
}
