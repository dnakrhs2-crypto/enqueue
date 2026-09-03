#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace gocue::SafeFileWrite
{

/** Checks the text read back from the temporary file before it replaces the target (e.g. "does it parse?"). */
using Verifier = std::function<juce::Result (const juce::String& readBack)>;

/** Writes 'text' (UTF-8) to a uniquely named sibling, checks the stream status and the bytes on disk, runs the
    verifier, then swaps the file in with retries: a full disk, a dropped share or a crash mid-write leaves the
    previous file intact. Creates the parent directory when missing. */
juce::Result writeTextVerified (const juce::File& file, const juce::String& text, const Verifier& verify = nullptr);

} // namespace gocue::SafeFileWrite
