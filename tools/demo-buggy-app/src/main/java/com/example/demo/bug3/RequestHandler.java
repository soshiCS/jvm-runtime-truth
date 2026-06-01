package com.example.demo.bug3;

/**
 * Common interface for all request type handlers.
 * The router selects an implementation at runtime based on Request.type().
 */
public interface RequestHandler {
    String handle(Request request);
}
