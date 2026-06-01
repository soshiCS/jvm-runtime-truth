package com.example.demo.bug1;

/**
 * Writes a structured audit record to the audit log.
 * Expects a structured identifier (e.g. user ID or order ID), NOT a delivery address.
 */
public class AuditHandler implements EventHandler {
    @Override
    public void handle(String payload) {
        // This handler silently accepts any payload and logs it as an audit entry.
        // When misdispatched for DELIVERY events, the delivery is silently dropped
        // and only an audit record is written — no error, no delivery.
        System.out.println("[AUDIT] Recorded event payload: " + payload);
    }
}
