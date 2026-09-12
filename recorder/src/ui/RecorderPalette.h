#pragma once
#include <juce_graphics/juce_graphics.h>

namespace gocue::recorder::Palette
{
// Design 13: Premiere Dark. Recorder owns its colours independently of LiveMix.
inline const juce::Colour background      {0xff1e1e1e};
inline const juce::Colour bar             {0xff2d2d2d};
inline const juce::Colour card            {0xff262626};
inline const juce::Colour card2           {0xff2d2d2d};
inline const juce::Colour line            {0xff3a3a3a};
inline const juce::Colour text            {0xffe6e6e6};
inline const juce::Colour dimText         {0xff9a9a9a};
inline const juce::Colour accent          {0xff4a9df0};
inline const juce::Colour brand           {0xffe0443a};
inline const juce::Colour danger          {0xffe0443a};
inline const juce::Colour meterGreen      {0xff4ec27a};
inline const juce::Colour meterYellow     {0xffe2a93b};
inline const juce::Colour meterRed        {0xffe0443a};
inline const juce::Colour meterBg         {0xff232323};
inline const juce::Colour field           {0xff232323};
inline const juce::Colour selection       {0xff31445f};
inline const juce::Colour clipVideoTop    {0xff3b3560};
inline const juce::Colour clipVideoBottom {0xff332f5e};
inline const juce::Colour clipAudio       {0xff243f33};
inline const juce::Colour recording       {0xffe0443a};
inline const juce::Colour muteOn          {0xffe5342a};
inline const juce::Colour soloOn          {0xfff2c53d};
constexpr float cardRadius = 4.0f;
constexpr float controlRadius = 4.0f;
}
