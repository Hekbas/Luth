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

    enum class BusType {
        MainThread,
        RenderThread,
        COUNT
    };

    class EventBus
    {
    public:
        // Queue an event for later processing
        template<typename T, typename... Args>
        static void Enqueue(BusType bus, Args&&... args) {
            static_assert(std::is_base_of_v<Event, T>,
                "T must inherit from Event");
            GetBus(bus).Enqueue<T>(std::forward<Args>(args)...);
        }

        // Subscribe to specific event type
        template<typename T>
        static void Subscribe(BusType bus, EventHandler handler) {
            GetBus(bus).Subscribe<T>(std::move(handler));
        }

        // Process all queued events
        static void ProcessEvents(BusType bus) {
            GetBus(bus).ProcessEvents();
        }

        class BusInstance {
        public:
            template<typename T, typename... Args>
            void Enqueue(Args&&... args) {
                m_EventQueue.emplace(
                    std::make_unique<T>(std::forward<Args>(args)...),
                    GetEventTypeID<T>()
                );
            }

            template<typename T>
            void Subscribe(EventHandler handler) {
                const auto typeID = GetEventTypeID<T>();
                m_Subscribers[typeID].emplace_back(std::move(handler));
            }

            void ProcessEvents() {
                while (!m_EventQueue.empty()) {
                    auto& [event, typeID] = m_EventQueue.front();
                    DispatchEvent(*event, typeID);
                    m_EventQueue.pop();
                }
            }

            template<typename T>
            static EventTypeID GetEventTypeID() {
                static EventTypeID typeID = NextEventTypeID();
                return typeID;
            }

        private:
            void DispatchEvent(Event& event, EventTypeID typeID) {
                if (auto it = m_Subscribers.find(typeID); it != m_Subscribers.end()) {
                    for (auto& handler : it->second) {
                        if (event.m_Handled) break;
                        handler(event);
                    }
                }
            }

            static EventTypeID NextEventTypeID() {
                static EventTypeID counter = 0;
                return counter++;
            }

            std::queue<std::pair<EventPtr, EventTypeID>> m_EventQueue;
            std::unordered_map<EventTypeID, std::vector<EventHandler>> m_Subscribers;
        };

        static BusInstance& GetBus(BusType bus) {
            static std::array<BusInstance, (size_t)BusType::COUNT> buses;
            return buses[(size_t)bus];
        }

        /*static EventTypeID GetEventTypeID() {
            static EventTypeID m_NextTypeID = 0;
            return m_NextTypeID++;
        }*/
    };
}
