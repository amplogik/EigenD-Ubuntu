/*
 Copyright 2026 EigenD contributors

 This file is part of EigenD.

 EigenD is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
*/

// Linux-only JUCE internals that libpijuce builds but does not export
// (JUCE is compiled with -fvisibility=hidden and these lack JUCE_API).
// The CLAP host in libejuce needs them to embed plugin GUIs.

#ifndef __PIJUCE_LINUX_GLUE__
#define __PIJUCE_LINUX_GLUE__

#include "juce.h"
#include <functional>

namespace pijuce
{
    // An XEmbedComponent hosting a foreign X11 window; delete it as a Component.
    JUCE_API juce::Component *xembed_create(bool wants_keyboard_focus, bool allow_foreign_resize);
    JUCE_API unsigned long xembed_host_window(juce::Component *xembed);
    JUCE_API void xembed_remove_client(juce::Component *xembed);

    // juce::LinuxEventLoop fd callbacks on the message thread
    JUCE_API void register_fd(int fd, std::function<void(int)> callback, short poll_mask);
    JUCE_API void unregister_fd(int fd);
}

#endif
