/*
 Copyright 2026 EigenD contributors

 This file is part of EigenD.

 EigenD is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
*/

// need JUCE's internal XWindowSystem / X11Symbols for the display sync below
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_extra/juce_gui_extra.h>
#include "pijuce_linux_glue.h"

namespace pijuce
{
    juce::Component *xembed_create(bool wants_keyboard_focus, bool allow_foreign_resize)
    {
        return new juce::XEmbedComponent(wants_keyboard_focus,allow_foreign_resize);
    }

    unsigned long xembed_host_window(juce::Component *c)
    {
        auto *x = dynamic_cast<juce::XEmbedComponent *>(c);
        if(!x) return 0;
        unsigned long id = x->getHostWindowID();

        // The host window was created on *our* X connection and may still be
        // sitting in Xlib's output buffer. A plugin creates its window inside
        // it on its *own* connection; if that request reaches the server
        // first, the parent doesn't exist yet and the plugin gets BadWindow
        // (silently). Make sure the server has it before handing it out.
        if(auto *d = juce::XWindowSystem::getInstance()->getDisplay())
        {
            juce::X11Symbols::getInstance()->xSync(d,0);
        }
        return id;
    }

    void xembed_remove_client(juce::Component *c)
    {
        if(auto *x = dynamic_cast<juce::XEmbedComponent *>(c)) x->removeClient();
    }

    void register_fd(int fd, std::function<void(int)> callback, short poll_mask)
    {
        juce::LinuxEventLoop::registerFdCallback(fd,std::move(callback),poll_mask);
    }

    void unregister_fd(int fd)
    {
        juce::LinuxEventLoop::unregisterFdCallback(fd);
    }
}
