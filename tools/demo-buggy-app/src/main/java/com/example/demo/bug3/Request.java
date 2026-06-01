package com.example.demo.bug3;

/**
 * Incoming request routed to a handler by type.
 * Type is one of: USER, ADMIN, GUEST.
 */
public record Request(String type, String userId, String data) {}
