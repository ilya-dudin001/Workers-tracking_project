#pragma once

#include "observer/agent_registry.h"
#include "observer/focus_router.h"
#include "observer/name_store.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

namespace monitor::observer {

class ObserverWindow {
public:
    ObserverWindow(HINSTANCE instance, AgentRegistry& registry,
                   ProfileSink& profile_sink, NameStore& names);
    ~ObserverWindow();
    ObserverWindow(const ObserverWindow&) = delete;
    ObserverWindow& operator=(const ObserverWindow&) = delete;

    [[nodiscard]] int run(int show_command);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
