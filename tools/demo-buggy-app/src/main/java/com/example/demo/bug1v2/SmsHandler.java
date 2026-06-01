package com.example.demo.bug1v2;

import com.example.demo.bug1.EventHandler;

public class SmsHandler implements EventHandler {
    @Override
    public void handle(String payload) {
        System.out.printf("[SMS] Sending SMS to: %s%n", payload);
    }
}
