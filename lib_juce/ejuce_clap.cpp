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

/*
 * CLAP plugin hosting for EigenD, as a juce::AudioPluginFormat.
 *
 * Scope of this first pass: scanning, audio processing, MIDI/MPE input.
 * Not yet: plugin editors (clap.gui), parameters (clap.params), state
 * (clap.state), transport, latency/tail, MIDI output.
 *
 * Threading: CLAP distinguishes the main thread from the audio thread. The
 * JUCE message thread is our main thread; request_callback() is marshalled
 * onto it with MessageManager::callAsync.
 */

#include "ejuce_clap.h"
#include "pijuce_linux_glue.h"
#include <picross/pic_log.h>

#if JUCE_LINUX

#include <clap/clap.h>
#include <dlfcn.h>
#include <poll.h>
#include <set>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace
{
    // One loaded .clap file. The spec requires exactly one init()/deinit() pair
    // per load, so a module is shared by every instance created from that file
    // and released when the last one goes away.
    struct clap_module_t
    {
        void *handle_ = nullptr;
        const clap_plugin_entry_t *entry_ = nullptr;
        const clap_plugin_factory_t *factory_ = nullptr;

        ~clap_module_t()
        {
            if(entry_) entry_->deinit();
            if(handle_) dlclose(handle_);
        }

        uint32_t count() const
        {
            return factory_->get_plugin_count(factory_);
        }

        const clap_plugin_descriptor_t *descriptor(uint32_t i) const
        {
            return factory_->get_plugin_descriptor(factory_,i);
        }

        static std::shared_ptr<clap_module_t> load(const juce::String &path, juce::String &err)
        {
            static std::mutex lock;
            static std::map<juce::String,std::weak_ptr<clap_module_t>> cache;

            std::lock_guard<std::mutex> guard(lock);

            if(auto existing = cache[path].lock())
            {
                return existing;
            }

            auto m = std::make_shared<clap_module_t>();

            m->handle_ = dlopen(path.toRawUTF8(),RTLD_LOCAL|RTLD_NOW);
            if(!m->handle_)
            {
                err = "dlopen failed: " + juce::String(dlerror());
                return {};
            }

            auto entry = (const clap_plugin_entry_t *)dlsym(m->handle_,"clap_entry");
            if(!entry)
            {
                err = "no clap_entry symbol";
                return {};
            }

            if(!clap_version_is_compatible(entry->clap_version))
            {
                err = "incompatible CLAP version";
                return {};
            }

            // only a successful init() may be paired with deinit()
            if(!entry->init(path.toRawUTF8()))
            {
                err = "clap_entry init failed";
                return {};
            }
            m->entry_ = entry;

            m->factory_ = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
            if(!m->factory_)
            {
                err = "no plugin factory";
                return {};
            }

            cache[path] = m;
            return m;
        }
    };

    // JUCE identifies a plugin within a file by an int; derive it from the CLAP id.
    int clap_unique_id(const char *clap_id)
    {
        return juce::String::fromUTF8(clap_id).hashCode();
    }

    bool has_feature(const clap_plugin_descriptor_t *d, const char *feature)
    {
        if(!d->features) return false;

        for(const char *const *f = d->features; *f; ++f)
        {
            if(!strcmp(*f,feature)) return true;
        }

        return false;
    }

    // The CLAP plugin and the host struct it talks to. Shared so that a
    // queued main-thread callback can tell whether the plugin still exists.
    struct clap_handle_t: std::enable_shared_from_this<clap_handle_t>
    {
        std::shared_ptr<clap_module_t> module_;
        clap_host_t host_;
        const clap_plugin_t *plugin_ = nullptr;
        std::atomic<bool> callback_pending_ { false };

        // GUI support (main thread only): the open editor, plus the timers and
        // file descriptors a plugin's X11 GUI asks the host to run for it.
        juce::Component::SafePointer<juce::AudioProcessorEditor> editor_;
        double editor_scale_ = 1.0;
        struct timer_t: juce::Timer
        {
            clap_handle_t *h; clap_id id;
            timer_t(clap_handle_t *h_, clap_id id_): h(h_), id(id_) {}
            void timerCallback() override
            {
                static int logged = 0;
                if(logged < 5) { ++logged; pic::logmsg() << "clap host: timer " << id << " fired"; }
                auto ext = h->extension<clap_plugin_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
                if(ext && ext->on_timer) ext->on_timer(h->plugin_,id);
            }
        };
        std::map<clap_id,std::unique_ptr<timer_t>> timers_;
        clap_id next_timer_ = 1;
        std::set<int> fds_;

        clap_handle_t()
        {
            host_.clap_version = CLAP_VERSION;
            host_.host_data = this;
            host_.name = "EigenD";
            host_.vendor = "EigenD Community";
            host_.url = "https://github.com/TheTechnobear/EigenD";
            host_.version = "3.0";
            host_.get_extension = get_extension;
            host_.request_restart = request_restart;
            host_.request_process = request_process;
            host_.request_callback = request_callback;
        }

        ~clap_handle_t()
        {
            timers_.clear();
            for(int fd: fds_) pijuce::unregister_fd(fd);
            fds_.clear();
            if(plugin_) plugin_->destroy(plugin_);
        }

        template<class T> const T *extension(const char *id) const
        {
            return (const T *)plugin_->get_extension(plugin_,id);
        }

        static clap_handle_t *self(const clap_host_t *h)
        {
            return (clap_handle_t *)h->host_data;
        }

        // --- host extensions --------------------------------------------

        static void gui_resize_hints_changed(const clap_host_t *) {}
        static bool gui_request_resize(const clap_host_t *h, uint32_t w, uint32_t hh)
        {
            std::weak_ptr<clap_handle_t> weak = self(h)->weak_from_this();
            juce::MessageManager::callAsync([weak,w,hh]()
            {
                if(auto s = weak.lock())
                    if(auto *e = s->editor_.getComponent())
                        e->setSize((int)std::lround(w/s->editor_scale_),(int)std::lround(hh/s->editor_scale_));
            });
            return true;
        }
        static bool gui_request_show(const clap_host_t *) { return false; }
        static bool gui_request_hide(const clap_host_t *) { return false; }
        static void gui_closed(const clap_host_t *, bool) {}

        static bool timer_register(const clap_host_t *h, uint32_t period_ms, clap_id *id)
        {
            clap_handle_t *s = self(h);
            clap_id i = s->next_timer_++;
            pic::logmsg() << "clap host: register_timer " << period_ms << "ms -> id " << i;
            auto t = std::make_unique<timer_t>(s,i);
            t->startTimer((int)std::max<uint32_t>(period_ms,1));
            s->timers_[i] = std::move(t);
            if(id) *id = i;
            return true;
        }
        static bool timer_unregister(const clap_host_t *h, clap_id id)
        {
            return self(h)->timers_.erase(id) > 0;
        }

        static short poll_mask(clap_posix_fd_flags_t f)
        {
            short m = 0;
            if(f & CLAP_POSIX_FD_READ) m |= POLLIN;
            if(f & CLAP_POSIX_FD_WRITE) m |= POLLOUT;
            if(f & CLAP_POSIX_FD_ERROR) m |= POLLERR;
            return m ? m : POLLIN;
        }
        static bool fd_register(const clap_host_t *h, int fd, clap_posix_fd_flags_t flags)
        {
            clap_handle_t *s = self(h);
            pic::logmsg() << "clap host: register_fd " << fd << " flags " << flags;
            std::weak_ptr<clap_handle_t> weak = s->weak_from_this();
            pijuce::register_fd(fd,[weak,flags](int f)
            {
                static int logged = 0;
                if(logged < 5) { ++logged; pic::logmsg() << "clap host: fd " << f << " ready"; }
                if(auto s = weak.lock())
                {
                    auto ext = s->extension<clap_plugin_posix_fd_support_t>(CLAP_EXT_POSIX_FD_SUPPORT);
                    if(ext && ext->on_fd) ext->on_fd(s->plugin_,f,flags);
                }
            },poll_mask(flags));
            s->fds_.insert(fd);
            return true;
        }
        static bool fd_modify(const clap_host_t *h, int fd, clap_posix_fd_flags_t flags)
        {
            pijuce::unregister_fd(fd);
            self(h)->fds_.erase(fd);
            return fd_register(h,fd,flags);
        }
        static bool fd_unregister(const clap_host_t *h, int fd)
        {
            pijuce::unregister_fd(fd);
            return self(h)->fds_.erase(fd) > 0;
        }

        static const void *get_extension(const clap_host_t *, const char *id)
        {
            pic::logmsg() << "clap host: plugin asked for extension " << id;
            static const clap_host_gui_t gui = { gui_resize_hints_changed, gui_request_resize,
                                                 gui_request_show, gui_request_hide, gui_closed };
            static const clap_host_timer_support_t timer = { timer_register, timer_unregister };
            static const clap_host_posix_fd_support_t fd = { fd_register, fd_modify, fd_unregister };

            if(!strcmp(id,CLAP_EXT_GUI)) return &gui;
            if(!strcmp(id,CLAP_EXT_TIMER_SUPPORT)) return &timer;
            if(!strcmp(id,CLAP_EXT_POSIX_FD_SUPPORT)) return &fd;
            return nullptr;
        }

        static void request_restart(const clap_host_t *)
        {
            // not supported yet: would need deactivate/activate on the main thread
        }

        static void request_process(const clap_host_t *)
        {
            // EigenD processes continuously while the plugin is active
        }

        static void request_callback(const clap_host_t *h)
        {
            clap_handle_t *s = self(h);

            if(s->callback_pending_.exchange(true))
            {
                return;
            }

            std::weak_ptr<clap_handle_t> weak = s->weak_from_this();

            juce::MessageManager::callAsync([weak]()
            {
                if(auto s = weak.lock())
                {
                    s->callback_pending_ = false;
                    if(s->plugin_) s->plugin_->on_main_thread(s->plugin_);
                }
            });
        }
    };

    struct port_t
    {
        juce::String name;
        uint32_t channels;
    };

    // Embeds the plugin's own X11 GUI in a JUCE window via XEmbed.
    class clap_editor_t: public juce::AudioProcessorEditor
    {
        public:
            clap_editor_t(juce::AudioProcessor &p, std::shared_ptr<clap_handle_t> h, double zoom):
                juce::AudioProcessorEditor(&p), handle_(h), embed_(pijuce::xembed_create(true,false))
            {
                addAndMakeVisible(*embed_);
                gui_ = handle_->extension<clap_plugin_gui_t>(CLAP_EXT_GUI);
                const clap_plugin_t *pl = handle_->plugin_;

                uint32_t w = 400, hh = 300;
                if(gui_ && gui_->create(pl,CLAP_WINDOW_API_X11,false))
                {
                    created_ = true;

                    // X11 is a physical-pixel API: tell the plugin the display
                    // scale. A plugin that accepts it reports physical sizes
                    // (convert to our logical units); one that refuses scales
                    // itself and its size is used as-is.
                    double display = 1.0;
                    if(auto *d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                        display = d->scale;
                    const double want = display*zoom;
                    if(gui_->set_scale && (want != 1.0) && gui_->set_scale(pl,want))
                        scale_ = display;   // sizes now in physical pixels

                    gui_->get_size(pl,&w,&hh);
                }
                pic::logmsg() << "clap gui: create " << (created_?"ok":"FAILED") << " size " << w << "x" << hh
                              << " zoom " << zoom << " px/unit " << scale_;
                setSize((int)std::lround(w/scale_),(int)std::lround(hh/scale_));
                setResizable(created_ && gui_->can_resize && gui_->can_resize(pl),false);

                if(created_)
                {
                    clap_window_t win;
                    win.api = CLAP_WINDOW_API_X11;
                    win.x11 = pijuce::xembed_host_window(embed_.get());
                    bool parented = gui_->set_parent(pl,&win);
                    bool shown = parented && gui_->show(pl);
                    pic::logmsg() << "clap gui: host window " << (unsigned long)win.x11
                                  << " set_parent " << (parented?"ok":"FAILED")
                                  << " show " << (shown?"ok":"FAILED");
                    if(parented)
                    {
                        handle_->editor_ = this;
                        handle_->editor_scale_ = scale_;
                    }
                }
            }

            ~clap_editor_t() override
            {
                if(created_)
                {
                    const clap_plugin_t *pl = handle_->plugin_;
                    gui_->hide(pl);
                    gui_->destroy(pl);
                }
                pijuce::xembed_remove_client(embed_.get());
                handle_->editor_ = nullptr;
            }

            void resized() override
            {
                embed_->setBounds(getLocalBounds());
                if(created_ && gui_->can_resize && gui_->can_resize(handle_->plugin_))
                {
                    uint32_t w = (uint32_t)std::lround(getWidth()*scale_), hh = (uint32_t)std::lround(getHeight()*scale_);
                    if(gui_->adjust_size) gui_->adjust_size(handle_->plugin_,&w,&hh);
                    gui_->set_size(handle_->plugin_,w,hh);
                }
            }

            void paint(juce::Graphics &g) override { g.fillAll(juce::Colours::black); }

        private:
            std::shared_ptr<clap_handle_t> handle_;
            std::unique_ptr<juce::Component> embed_;
            const clap_plugin_gui_t *gui_ = nullptr;
            bool created_ = false;
            double scale_ = 1.0;  // physical pixels per JUCE unit when the plugin accepted set_scale
    };

    class clap_instance_t: public juce::AudioPluginInstance
    {
        public:
            static std::unique_ptr<clap_instance_t> create(std::shared_ptr<clap_module_t> module, const clap_plugin_descriptor_t *desc, const juce::String &path, juce::String &err)
            {
                auto h = std::make_shared<clap_handle_t>();
                h->module_ = module;
                h->plugin_ = module->factory_->create_plugin(module->factory_,&h->host_,desc->id);

                if(!h->plugin_)
                {
                    err = "create_plugin failed for " + juce::String::fromUTF8(desc->id);
                    return {};
                }

                if(!h->plugin_->init(h->plugin_))
                {
                    err = "plugin init failed for " + juce::String::fromUTF8(desc->id);
                    h->plugin_->destroy(h->plugin_);
                    h->plugin_ = nullptr;
                    return {};
                }

                std::vector<port_t> inputs = audio_ports(*h,true);
                std::vector<port_t> outputs = audio_ports(*h,false);

                BusesProperties buses;
                add_buses(buses,inputs,true);
                add_buses(buses,outputs,false);

                return std::unique_ptr<clap_instance_t>(new clap_instance_t(buses,h,desc,path,inputs,outputs));
            }

            ~clap_instance_t() override
            {
                releaseResources();
            }

            void fillInPluginDescription(juce::PluginDescription &d) const override
            {
                d.name = juce::String::fromUTF8(desc_->name);
                d.descriptiveName = juce::String::fromUTF8(desc_->description ? desc_->description : desc_->name);
                d.pluginFormatName = "CLAP";
                d.manufacturerName = juce::String::fromUTF8(desc_->vendor ? desc_->vendor : "");
                d.version = juce::String::fromUTF8(desc_->version ? desc_->version : "");
                d.fileOrIdentifier = path_;
                d.lastFileModTime = juce::File(path_).getLastModificationTime();
                d.lastInfoUpdateTime = juce::Time::getCurrentTime();
                d.uniqueId = d.deprecatedUid = clap_unique_id(desc_->id);
                d.isInstrument = has_feature(desc_,CLAP_PLUGIN_FEATURE_INSTRUMENT);
                d.category = d.isInstrument ? "Instrument" : (has_feature(desc_,CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) ? "Effect" : "");
                d.numInputChannels = getTotalNumInputChannels();
                d.numOutputChannels = getTotalNumOutputChannels();
                d.hasSharedContainer = handle_->module_->count() > 1;
                d.hasARAExtension = false;
            }

            const juce::String getName() const override
            {
                return juce::String::fromUTF8(desc_->name);
            }

            void prepareToPlay(double sample_rate, int block_size) override
            {
                if(activated_ && sample_rate == sample_rate_ && block_size == block_size_)
                {
                    return;
                }

                releaseResources();

                setRateAndBufferSizeDetails(sample_rate,block_size);
                sample_rate_ = sample_rate;
                block_size_ = std::max(1,block_size);

                // inputs are copied to scratch so a plugin never sees its
                // input and output aliased (CLAP only allows that on request)
                input_scratch_.setSize(std::max(1,getTotalNumInputChannels()),block_size_);
                events_.clear();
                events_.reserve(max_events_);

                const clap_plugin_t *p = handle_->plugin_;
                activated_ = p->activate(p,sample_rate_,1,block_size_);
                steady_time_ = 0;

                if(!activated_)
                {
                    juce::Logger::writeToLog("CLAP: activate failed for " + getName());
                }
            }

            void releaseResources() override
            {
                const clap_plugin_t *p = handle_->plugin_;

                // stop_processing belongs on the audio thread; by the time a
                // host calls releaseResources() it has stopped calling
                // processBlock(), so this is the last point we can do it.
                if(processing_)
                {
                    p->stop_processing(p);
                    processing_ = false;
                }

                if(activated_)
                {
                    p->deactivate(p);
                    activated_ = false;
                }
            }

            void processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi) override
            {
                const clap_plugin_t *p = handle_->plugin_;
                const int frames = std::min(buffer.getNumSamples(),block_size_);

                if(!activated_ || frames <= 0)
                {
                    buffer.clear();
                    midi.clear();
                    return;
                }

                if(!processing_)
                {
                    processing_ = p->start_processing(p);

                    if(!processing_)
                    {
                        buffer.clear();
                        midi.clear();
                        return;
                    }
                }

                // audio: inputs from scratch, outputs straight into the buffer
                int channel = 0;
                for(size_t i=0; i<inputs_.size(); ++i)
                {
                    for(uint32_t c=0; c<inputs_[i].channels; ++c, ++channel)
                    {
                        input_scratch_.copyFrom(channel,0,buffer,channel,0,frames);
                        input_ptrs_[channel] = input_scratch_.getWritePointer(channel);
                    }
                }

                channel = 0;
                for(size_t i=0; i<outputs_.size(); ++i)
                {
                    for(uint32_t c=0; c<outputs_[i].channels; ++c, ++channel)
                    {
                        output_ptrs_[channel] = buffer.getWritePointer(channel);
                    }
                }

                // events: MIDI (incl. MPE) passed through when the plugin takes
                // MIDI, else note on/off translated to CLAP note events
                events_.clear();
                for(const auto meta: midi)
                {
                    if(meta.numBytes > 3 || meta.numBytes < 1) continue;
                    if(events_.size() == events_.capacity()) break;

                    const uint32_t time = (uint32_t)juce::jlimit(0,frames-1,meta.samplePosition);
                    event_t e;

                    if(note_dialect_ == CLAP_NOTE_DIALECT_MIDI)
                    {
                        e.midi.header = header(sizeof(clap_event_midi_t),time,CLAP_EVENT_MIDI);
                        e.midi.port_index = note_port_;
                        e.midi.data[0] = meta.data[0];
                        e.midi.data[1] = meta.numBytes > 1 ? meta.data[1] : 0;
                        e.midi.data[2] = meta.numBytes > 2 ? meta.data[2] : 0;
                    }
                    else if(note_dialect_ == CLAP_NOTE_DIALECT_CLAP)
                    {
                        const juce::MidiMessage m = meta.getMessage();
                        const bool on = m.isNoteOn();

                        if(!on && !m.isNoteOff()) continue;

                        e.note.header = header(sizeof(clap_event_note_t),time,on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
                        e.note.note_id = -1;
                        e.note.port_index = note_port_;
                        e.note.channel = (int16_t)(m.getChannel()-1);
                        e.note.key = (int16_t)m.getNoteNumber();
                        e.note.velocity = m.getFloatVelocity();
                    }
                    else
                    {
                        continue;
                    }

                    events_.push_back(e);
                }

                clap_input_events_t in_events { this, events_size, events_get };
                clap_output_events_t out_events { this, events_push };

                clap_audio_buffer_t in_buffers[max_ports_];
                clap_audio_buffer_t out_buffers[max_ports_];
                fill_buffers(in_buffers,inputs_,input_ptrs_.data());
                fill_buffers(out_buffers,outputs_,output_ptrs_.data());

                clap_process_t process;
                process.steady_time = steady_time_;
                process.frames_count = (uint32_t)frames;
                process.transport = nullptr;
                process.audio_inputs = in_buffers;
                process.audio_outputs = out_buffers;
                process.audio_inputs_count = (uint32_t)inputs_.size();
                process.audio_outputs_count = (uint32_t)outputs_.size();
                process.in_events = &in_events;
                process.out_events = &out_events;

                if(p->process(p,&process) == CLAP_PROCESS_ERROR)
                {
                    buffer.clear();
                }

                steady_time_ += frames;
                midi.clear();
            }

            using juce::AudioPluginInstance::processBlock;

            double getTailLengthSeconds() const override { return 0.0; }
            bool acceptsMidi() const override { return note_dialect_ != 0; }
            bool producesMidi() const override { return false; }

            juce::AudioProcessorEditor *createEditor() override
            {
                bool ok = hasEditor();
                pic::logmsg() << "clap gui: createEditor -> " << (ok?"embedding":"no gui, generic editor");
                return ok ? new clap_editor_t(*this,handle_,gui_zoom_) : nullptr;
            }

            void set_gui_zoom(double z) { gui_zoom_ = z; }
            bool hasEditor() const override
            {
                auto gui = handle_->extension<clap_plugin_gui_t>(CLAP_EXT_GUI);
                bool x11 = gui && gui->is_api_supported(handle_->plugin_,CLAP_WINDOW_API_X11,false);
                pic::logmsg() << "clap gui: ext " << (gui?"present":"MISSING") << " x11 " << (x11?"yes":"no");
                return x11;
            }

            int getNumPrograms() override { return 1; }
            int getCurrentProgram() override { return 0; }
            void setCurrentProgram(int) override {}
            const juce::String getProgramName(int) override { return {}; }
            void changeProgramName(int, const juce::String &) override {}

            void getStateInformation(juce::MemoryBlock &) override {}
            void setStateInformation(const void *, int) override {}

        private:
            // CLAP events are variable-sized structs; this holds any we send
            union event_t
            {
                clap_event_header_t header;
                clap_event_midi_t midi;
                clap_event_note_t note;
            };

            static constexpr size_t max_events_ = 1024;
            static constexpr size_t max_ports_ = 16;

            clap_instance_t(const BusesProperties &buses, std::shared_ptr<clap_handle_t> h, const clap_plugin_descriptor_t *desc, const juce::String &path,
                            const std::vector<port_t> &inputs, const std::vector<port_t> &outputs):
                juce::AudioPluginInstance(buses), handle_(h), desc_(desc), path_(path), inputs_(inputs), outputs_(outputs)
            {
                input_ptrs_.resize(std::max(1,getTotalNumInputChannels()));
                output_ptrs_.resize(std::max(1,getTotalNumOutputChannels()));
                choose_note_port();
            }

            static std::vector<port_t> audio_ports(clap_handle_t &h, bool is_input)
            {
                std::vector<port_t> ports;
                auto ext = h.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
                if(!ext) return ports;

                const uint32_t n = std::min<uint32_t>(ext->count(h.plugin_,is_input),max_ports_);

                for(uint32_t i=0; i<n; ++i)
                {
                    clap_audio_port_info_t info;
                    if(!ext->get(h.plugin_,i,is_input,&info)) continue;
                    ports.push_back({ juce::String::fromUTF8(info.name), info.channel_count });
                }

                return ports;
            }

            static void add_buses(BusesProperties &buses, const std::vector<port_t> &ports, bool is_input)
            {
                for(const port_t &p: ports)
                {
                    if(p.channels == 0) continue;

                    juce::AudioChannelSet set = p.channels <= 2 ? juce::AudioChannelSet::canonicalChannelSet((int)p.channels)
                                                                : juce::AudioChannelSet::discreteChannels((int)p.channels);
                    buses.addBus(is_input,p.name,set,true);
                }
            }

            void choose_note_port()
            {
                auto ext = handle_->extension<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS);
                if(!ext || ext->count(handle_->plugin_,true) == 0) return;

                clap_note_port_info_t info;
                if(!ext->get(handle_->plugin_,0,true,&info)) return;

                note_port_ = 0;

                // prefer raw MIDI: it carries MPE (per-channel pitchbend/pressure) untouched
                if(info.supported_dialects & (CLAP_NOTE_DIALECT_MIDI|CLAP_NOTE_DIALECT_MIDI_MPE))
                {
                    note_dialect_ = CLAP_NOTE_DIALECT_MIDI;
                }
                else if(info.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
                {
                    note_dialect_ = CLAP_NOTE_DIALECT_CLAP;
                }
            }

            static void fill_buffers(clap_audio_buffer_t *buffers, const std::vector<port_t> &ports, float **ptrs)
            {
                for(size_t i=0; i<ports.size(); ++i)
                {
                    buffers[i].data32 = ports[i].channels ? ptrs : nullptr;
                    buffers[i].data64 = nullptr;
                    buffers[i].channel_count = ports[i].channels;
                    buffers[i].latency = 0;
                    buffers[i].constant_mask = 0;
                    ptrs += ports[i].channels;
                }
            }

            static clap_event_header_t header(uint32_t size, uint32_t time, uint16_t type)
            {
                clap_event_header_t h;
                h.size = size;
                h.time = time;
                h.space_id = CLAP_CORE_EVENT_SPACE_ID;
                h.type = type;
                h.flags = 0;
                return h;
            }

            static uint32_t events_size(const clap_input_events_t *list)
            {
                return (uint32_t)((clap_instance_t *)list->ctx)->events_.size();
            }

            static const clap_event_header_t *events_get(const clap_input_events_t *list, uint32_t index)
            {
                return &((clap_instance_t *)list->ctx)->events_[index].header;
            }

            static bool events_push(const clap_output_events_t *, const clap_event_header_t *)
            {
                return true; // MIDI output not supported yet; discard
            }

            std::shared_ptr<clap_handle_t> handle_;
            double gui_zoom_ = 1.0;
            const clap_plugin_descriptor_t *desc_;
            juce::String path_;
            std::vector<port_t> inputs_, outputs_;
            std::vector<float *> input_ptrs_, output_ptrs_;
            juce::AudioBuffer<float> input_scratch_;
            std::vector<event_t> events_;
            uint32_t note_dialect_ = 0;
            uint16_t note_port_ = 0;
            bool activated_ = false;
            bool processing_ = false;
            double sample_rate_ = 0;
            int block_size_ = 0;
            int64_t steady_time_ = 0;
    };

    class clap_format_t: public juce::AudioPluginFormat
    {
        public:
            juce::String getName() const override { return "CLAP"; }

            void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription> &results, const juce::String &file) override
            {
                juce::String err;
                auto module = clap_module_t::load(file,err);

                if(!module)
                {
                    juce::Logger::writeToLog("CLAP: " + file + ": " + err);
                    return;
                }

                // instantiate each plugin to learn its channel counts
                for(uint32_t i=0; i<module->count(); ++i)
                {
                    const clap_plugin_descriptor_t *desc = module->descriptor(i);
                    if(!desc || !clap_version_is_compatible(desc->clap_version)) continue;

                    auto instance = clap_instance_t::create(module,desc,file,err);
                    if(!instance)
                    {
                        juce::Logger::writeToLog("CLAP: " + file + ": " + err);
                        continue;
                    }

                    auto d = std::make_unique<juce::PluginDescription>();
                    instance->fillInPluginDescription(*d);
                    results.add(d.release());
                }
            }

            bool fileMightContainThisPluginType(const juce::String &file) override
            {
                const juce::File f(file);
                return f.existsAsFile() && f.hasFileExtension(".clap");
            }

            juce::String getNameOfPluginFromIdentifier(const juce::String &file) override
            {
                return juce::File(file).getFileNameWithoutExtension();
            }

            bool pluginNeedsRescanning(const juce::PluginDescription &d) override
            {
                return juce::File(d.fileOrIdentifier).getLastModificationTime() != d.lastFileModTime;
            }

            bool doesPluginStillExist(const juce::PluginDescription &d) override
            {
                return juce::File(d.fileOrIdentifier).existsAsFile();
            }

            bool canScanForPlugins() const override { return true; }
            bool isTrivialToScan() const override { return false; }

            juce::StringArray searchPathsForPlugins(const juce::FileSearchPath &path, bool recursive, bool) override
            {
                juce::StringArray found;

                for(int i=0; i<path.getNumPaths(); ++i)
                {
                    for(const juce::File &f: path[i].findChildFiles(juce::File::findFiles,recursive,"*.clap"))
                    {
                        found.addIfNotAlreadyThere(f.getFullPathName());
                    }
                }

                return found;
            }

            // CLAP spec, Linux: $CLAP_PATH, ~/.clap, /usr/lib/clap
            juce::FileSearchPath getDefaultLocationsToSearch() override
            {
                juce::FileSearchPath path;

                for(const juce::String &p: juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("CLAP_PATH",{}),":",""))
                {
                    if(p.isNotEmpty()) path.addIfNotAlreadyThere(juce::File(p));
                }

                path.addIfNotAlreadyThere(juce::File("~/.clap"));
                path.addIfNotAlreadyThere(juce::File("/usr/lib/clap"));
                path.removeNonExistentPaths();
                return path;
            }

            bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription &) const override { return false; }

        protected:
            void createPluginInstance(const juce::PluginDescription &d, double sample_rate, int block_size, PluginCreationCallback callback) override
            {
                juce::String err;
                auto module = clap_module_t::load(d.fileOrIdentifier,err);

                if(!module)
                {
                    callback(nullptr,err);
                    return;
                }

                // a .clap can hold several plugins: match on the id hash, then name
                const clap_plugin_descriptor_t *match = nullptr;
                for(uint32_t i=0; i<module->count() && !match; ++i)
                {
                    const clap_plugin_descriptor_t *desc = module->descriptor(i);
                    if(desc && clap_unique_id(desc->id) == d.uniqueId) match = desc;
                }
                for(uint32_t i=0; i<module->count() && !match; ++i)
                {
                    const clap_plugin_descriptor_t *desc = module->descriptor(i);
                    if(desc && d.name == juce::String::fromUTF8(desc->name)) match = desc;
                }

                if(!match)
                {
                    callback(nullptr,"plugin " + d.name + " not found in " + d.fileOrIdentifier);
                    return;
                }

                auto instance = clap_instance_t::create(module,match,d.fileOrIdentifier,err);
                if(instance)
                {
                    instance->setRateAndBufferSizeDetails(sample_rate,block_size);
                }

                callback(std::move(instance),err);
            }
    };
}

void ejuce::add_clap_format(juce::AudioPluginFormatManager &manager)
{
    manager.addFormat(new clap_format_t());
}

bool ejuce::set_plugin_gui_zoom(juce::AudioPluginInstance *p, double zoom)
{
    if(auto *c = dynamic_cast<clap_instance_t *>(p))
    {
        c->set_gui_zoom(zoom);
        return true;
    }
    return false;
}

#else

void ejuce::add_clap_format(juce::AudioPluginFormatManager &)
{
}

bool ejuce::set_plugin_gui_zoom(juce::AudioPluginInstance *, double)
{
    return false;
}

#endif
