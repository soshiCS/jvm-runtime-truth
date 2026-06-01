package com.example.demo.bug1;

/**
 * Common interface for all notification handlers.
 * Implementations are selected at runtime via the handler registry.
 */
public interface EventHandler {
    void handle(String payload) throws Exception;
}
