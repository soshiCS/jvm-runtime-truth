package com.example.demo.bug1v2;

import com.example.demo.bug1.EventHandler;

public class PushHandler implements EventHandler {
    @Override
    public void handle(String payload) {
        System.out.printf("[PUSH] Sending push notification to: %s%n", payload);
    }
}
