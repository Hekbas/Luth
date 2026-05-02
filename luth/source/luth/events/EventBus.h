#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/events/Event.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <queue>
#include <thread>
#include <unordered_map>
#include <functional>
#include <mutex>

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

    // Opaque handle returned by Subscribe; pass to Unsubscribe to revoke. The handle
    // carries the typeID so Unsubscribe is O(subscribers-of-T) instead of scanning
    // every type's subscriber list. Default-constructed handle is invalid (no-op
    // Unsubscribe), letting callers use it as a "not yet subscribed" sentinel.
    struct SubscriptionHandle {
        EventTypeID type{static_cast<EventTypeID>(-1)};
        u64 id{0};
        bool IsValid() const { return id != 0; }
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

        // Subscribe to specific event type. Discard the return value if the
        // subscription is process-lifetime (e.g., App.cpp's window/file-drop hooks).
        // Capture the handle when the subscriber's lifetime is shorter than the bus
        // (panels, plugins) — call Unsubscribe in their teardown to avoid dangling
        // handler captures.
        template<typename T>
        static SubscriptionHandle Subscribe(BusType bus, EventHandler handler) {
            return GetBus(bus).Subscribe<T>(std::move(handler));
        }

        // Revoke a subscription. Safe to call with an invalid (default-constructed)
        // handle — no-op. Safe across buses; the handle's typeID + id resolve uniquely.
        static void Unsubscribe(BusType bus, SubscriptionHandle handle) {
            GetBus(bus).Unsubscribe(handle);
        }

        // Process all queued events
        static void ProcessEvents(BusType bus) {
            GetBus(bus).ProcessEvents();
        }

        class BusInstance {
        public:
            template<typename T, typename... Args>
            void Enqueue(Args&&... args) {
                std::lock_guard<std::mutex> lock(m_QueueLock);
                m_EventQueue.emplace(
                    std::make_unique<T>(std::forward<Args>(args)...),
                    GetEventTypeID<T>()
                );
            }

            template<typename T>
            SubscriptionHandle Subscribe(EventHandler handler) {
                const auto typeID = GetEventTypeID<T>();
                const u64 id = ++m_NextSubId;   // pre-increment so 0 stays "invalid"
                m_Subscribers[typeID].push_back({ id, std::move(handler) });
                return { typeID, id };
            }

            void Unsubscribe(SubscriptionHandle handle) {
                if (!handle.IsValid()) return;
                auto it = m_Subscribers.find(handle.type);
                if (it == m_Subscribers.end()) return;
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                              [id = handle.id](const SubEntry& e) { return e.first == id; }),
                          vec.end());
            }

            // Drain the queue and dispatch all events. Single-thread-only by design —
            // first call captures the calling thread; subsequent calls assert match.
            //
            // Reentrancy: if a handler calls Enqueue<T> on the same bus, the new event
            // goes into m_EventQueue, NOT the processingQueue local. It will fire on
            // the NEXT ProcessEvents() call (i.e., next frame in the typical setup).
            // This is intentional — synchronous re-dispatch would risk handler chains
            // and unbounded recursion. Document at call sites that signal handlers
            // wanting same-frame follow-on work should write to panel state instead.
            void ProcessEvents() {
            #ifdef LUTH_BUILD_DEBUG
                static const std::thread::id s_DispatchThread = std::this_thread::get_id();
                LH_CORE_ASSERT(std::this_thread::get_id() == s_DispatchThread,
                    "EventBus::ProcessEvents called from inconsistent thread");
            #endif

                std::queue<std::pair<EventPtr, EventTypeID>> processingQueue;
                {
                    std::lock_guard<std::mutex> lock(m_QueueLock);
                    processingQueue.swap(m_EventQueue);
                }

                while (!processingQueue.empty()) {
                    auto& [event, typeID] = processingQueue.front();
                    DispatchEvent(*event, typeID);
                    processingQueue.pop();
                }
            }

            template<typename T>
            static EventTypeID GetEventTypeID() {
                static EventTypeID typeID = NextEventTypeID();
                return typeID;
            }

        private:
            // A throwing handler must not abort the dispatch loop — surviving handlers
            // and queued events would be silently lost otherwise. Catch, log, continue.
            // m_Handled propagation is preserved across exceptions: a thrown handler
            // is treated as not having consumed the event.
            void DispatchEvent(Event& event, EventTypeID typeID) {
                auto it = m_Subscribers.find(typeID);
                if (it == m_Subscribers.end()) return;
                for (auto& [id, handler] : it->second) {
                    (void)id;
                    if (event.m_Handled) break;
                    try {
                        handler(event);
                    } catch (const std::exception& e) {
                        LH_CORE_ERROR("EventBus handler for type {} threw: {}", typeID, e.what());
                    } catch (...) {
                        LH_CORE_ERROR("EventBus handler for type {} threw a non-std exception", typeID);
                    }
                }
            }

            static EventTypeID NextEventTypeID() {
                static EventTypeID counter = 0;
                return counter++;
            }

            using SubEntry = std::pair<u64, EventHandler>;

            std::queue<std::pair<EventPtr, EventTypeID>> m_EventQueue;
            std::unordered_map<EventTypeID, std::vector<SubEntry>> m_Subscribers;
            std::mutex m_QueueLock;
            u64 m_NextSubId = 0;
        };

        static BusInstance& GetBus(BusType bus) {
            static std::array<BusInstance, (size_t)BusType::COUNT> buses;
            return buses[(size_t)bus];
        }
    };
}
