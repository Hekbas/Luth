#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/events/Event.h"

#include <memory>
#include <queue>
#include <unordered_map>
#include <functional>

namespace Luth
{
    using EventPtr = std::unique_ptr<Event>;
    using EventHandler = std::function<void(Event&)>;
    using EventTypeID = size_t;

    class EventBus
    {
    public:
        template<typename T>
        static EventTypeID GetEventTypeID() {
            static EventTypeID typeID = m_NextTypeID++;
            return typeID;
        }

        // Queue an event for later processing
        template<typename T, typename... Args>
        void Enqueue(Args&&... args) {
            static_assert(std::is_base_of_v<Event, T>,
                "T must inherit from Event");

            m_EventQueue.emplace(
                std::make_unique<T>(std::forward<Args>(args)...),
                GetEventTypeID<T>()
            );
        }

        // Subscribe to specific event type
        template<typename T>
        void Subscribe(EventHandler handler) {
            const EventTypeID typeID = GetEventTypeID<T>();
            m_Subscribers[typeID].emplace_back(std::move(handler));
        }

        // Process all queued events
        void ProcessEvents()
        {
            while (!m_EventQueue.empty())
            {
                auto& [event, typeID] = m_EventQueue.front();
                DispatchEvent(*event, typeID);
                m_EventQueue.pop();
            }
        }

    private:
        void DispatchEvent(Event& event, EventTypeID typeID)
        {
            if (auto it = m_Subscribers.find(typeID); it != m_Subscribers.end())
            {
                for (auto& handler : it->second) {
                    if (event.m_Handled) break;
                    handler(event);
                }
            }
        }

        static inline EventTypeID m_NextTypeID = 0;
        std::queue<std::pair<EventPtr, EventTypeID>> m_EventQueue;
        std::unordered_map<EventTypeID, std::vector<EventHandler>> m_Subscribers;
    };
}
