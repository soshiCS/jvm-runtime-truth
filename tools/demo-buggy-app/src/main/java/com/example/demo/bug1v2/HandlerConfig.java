package com.example.demo.bug1v2;

import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.stereotype.Component;

import java.util.HashMap;
import java.util.Map;

/**
 * Binds notification handler class names from application.properties.
 * Keys are lowercase event type names; values are fully-qualified class names.
 *
 * Example property:
 *   notification.handlers.delivery=com.example.demo.bug1.DeliveryHandler
 */
@Component
@ConfigurationProperties(prefix = "notification")
public class HandlerConfig {

    private Map<String, String> handlers = new HashMap<>();

    public Map<String, String> getHandlers() {
        return handlers;
    }

    public void setHandlers(Map<String, String> handlers) {
        this.handlers = handlers;
    }
}
