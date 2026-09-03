#include "model/SafeFileWrite.h"

#include <cstring>

namespace gocue::SafeFileWrite
{

juce::Result writeTextVerified (const juce::File& file, const juce::String& text, const Verifier& verify)
{
    const auto dir = file.getParentDirectory();

    if (! dir.exists())
    {
        const auto created = dir.createDirectory();

        if (created.failed())
            return created;
    }

    juce::TemporaryFile temp (file);
    const juce::CharPointer_UTF8 utf8 = text.toUTF8();
    const auto bytes = (size_t) utf8.sizeInBytes() - 1;

    {
        juce::FileOutputStream stream (temp.getFile());

        if (stream.failedToOpen())
            return juce::Result::fail ("Could not write " + temp.getFile().getFullPathName() + ": " + stream.getStatus().getErrorMessage());

        if (! stream.write (utf8.getAddress(), bytes))
            return juce::Result::fail ("Could not write " + temp.getFile().getFullPathName() + ": " + stream.getStatus().getErrorMessage());

        stream.flush();

        if (stream.getStatus().failed())
            return juce::Result::fail ("Could not write " + temp.getFile().getFullPathName() + ": " + stream.getStatus().getErrorMessage());
    }

    juce::MemoryBlock written;

    if (! temp.getFile().loadFileAsData (written) || written.getSize() != bytes || std::memcmp (written.getData(), utf8.getAddress(), bytes) != 0)
        return juce::Result::fail ("Could not write " + file.getFullPathName() + ": the file on disk does not match what was written ("
                                   + juce::String ((juce::int64) written.getSize()) + " of " + juce::String ((juce::int64) bytes) + " bytes)");

    if (verify)
    {
        const auto checked = verify (temp.getFile().loadFileAsString());

        if (checked.failed())
            return juce::Result::fail ("Could not write " + file.getFullPathName() + ": " + checked.getErrorMessage());
    }

    if (! temp.overwriteTargetFileWithTemporary())   // ReplaceFile / rename, retried a few times (an antivirus scan holds the file briefly)
        return juce::Result::fail ("Could not replace " + file.getFullPathName());

    return juce::Result::ok();
}

} // namespace gocue::SafeFileWrite
