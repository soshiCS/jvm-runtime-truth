package com.example.demo.bug2;

/**
 * Represents a customer order.
 * Type is either "DOMESTIC" or "INTERNATIONAL" — tax rates differ.
 */
public record Order(String orderId, String customerId, double amount, String type) {}
