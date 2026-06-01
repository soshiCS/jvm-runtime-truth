package com.example.demo.bug1;

/** Sends a delivery confirmation to the provided recipient address. */
public class DeliveryHandler implements EventHandler {
    @Override
    public void handle(String payload) {
        System.out.println("[DELIVERY] Sending confirmation to: " + payload);
    }
}
