/*
 Copyright 2026 Kim McCann

 This file is part of EigenD.

 EigenD is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 EigenD is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with EigenD.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __EJUCE_CLAP__
#define __EJUCE_CLAP__

#include "ejuce_exports.h"
#include "juce.h"

namespace ejuce
{
    // JUCE has no CLAP hosting (not even in JUCE 9), so EigenD provides its own
    // juce::AudioPluginFormat for CLAP. Call this next to addDefaultFormats().
    // Currently Linux only; a no-op on other platforms.
    EJUCE_DECLSPEC_FUNC(void) add_clap_format(juce::AudioPluginFormatManager &manager);
}

#endif
