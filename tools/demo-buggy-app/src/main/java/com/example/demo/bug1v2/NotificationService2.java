package com.example.demo.bug1v2;

import com.example.demo.bug1.EventHandler;
import org.springframework.stereotype.Service;

import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

/**
 * Routes notification events to handler classes loaded from configuration.
 *
 * Handler class names are bound from application.properties at startup via
 * HandlerConfig. Each event type string maps to a fully-qualified class name
 * that is instantiated and invoked reflectively at dispatch time.
 *
 * The dispatch mechanism is identical to NotificationService: the concrete
 * handler class that executes is determined entirely at runtime by the config
 * values — static analysis of this file reveals only that reflection is used,
 * not which class handles each event type.
 */
@Service
public class NotificationService2 {

    private final Map<String, Class<? extends EventHandler>> registry = new HashMap<>();

    @SuppressWarnings("unchecked")
    public NotificationService2(HandlerConfig config) throws ClassNotFoundException {
        for (Map.Entry<String, String> entry : config.getHandlers().entrySet()) {
            String eventType = entry.getKey().toUpperCase();
            Class<?> cls = Class.forName(entry.getValue());
            registry.put(eventType, (Class<? extends EventHandler>) cls);
        }
    }

    public String dispatch(String eventType, String payload) throws Exception {
        Class<? extends EventHandler> handlerClass = registry.get(eventType.toUpperCase());
        if (handlerClass == null) {
            throw new IllegalArgumentException("No handler configured for event type: " + eventType);
        }

        Method handleMethod = handlerClass.getMethod("handle", String.class);
        EventHandler handler = handlerClass.getDeclaredConstructor().newInstance();
        handleMethod.invoke(handler, payload);

        return "dispatched:" + eventType + ":" + handlerClass.getSimpleName();
    }
}
