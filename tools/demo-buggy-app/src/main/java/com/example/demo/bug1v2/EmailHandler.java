package com.example.demo.bug1v2;

import com.example.demo.bug1.EventHandler;

public class EmailHandler implements EventHandler {
    @Override
    public void handle(String payload) {
        System.out.printf("[EMAIL] Sending email to: %s%n", payload);
    }
}
