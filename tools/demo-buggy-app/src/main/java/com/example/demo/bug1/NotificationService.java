package com.example.demo.bug1;

import org.springframework.stereotype.Service;

import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

/**
 * Routes notification events to the appropriate handler class via reflection.
 *
 * The handler registry maps event type strings to handler Class objects.
 * The dispatch uses Method.invoke so the registry drives all routing decisions
 * at runtime — the Java source code alone does not reveal which handler executes.
 *
 * BUG: "DELIVERY" is mapped to AuditHandler instead of DeliveryHandler.
 * Delivery events are silently audited but never delivered.
 * No exception is thrown — the symptom is missing deliveries, not a crash.
 */
@Service
public class NotificationService {

    // Handler registry: event type → handler class
    // Populated from external config in a real system; hardcoded here for the demo.
    private final Map<String, Class<? extends EventHandler>> registry = new HashMap<>();

    public NotificationService() {
        // BUG IS HERE: DELIVERY should map to DeliveryHandler, not AuditHandler.
        // In a real system this would come from a config file or database row.
        registry.put("DELIVERY", AuditHandler.class);   // ← BUG
        registry.put("ALERT",    DeliveryHandler.class);
        registry.put("AUDIT",    AuditHandler.class);
    }

    /**
     * Dispatches the event to the registered handler via reflection.
     * The caller cannot determine which concrete class was invoked from the call site alone.
     */
    public String dispatch(String eventType, String payload) throws Exception {
        Class<? extends EventHandler> handlerClass = registry.get(eventType);
        if (handlerClass == null) {
            throw new IllegalArgumentException("No handler registered for event type: " + eventType);
        }

        // Reflection dispatch: actual target class is invisible to static analysis
        Method handleMethod = handlerClass.getMethod("handle", String.class);
        EventHandler handler = handlerClass.getDeclaredConstructor().newInstance();
        handleMethod.invoke(handler, payload);   // ← causality hook fires here

        return "dispatched:" + eventType + ":" + handlerClass.getSimpleName();
    }
}
