package com.example.demo.bug4;

/**
 * A catalog product with separate base and sale prices.
 * Clearance items: correct price = 50% off salePrice.
 */
public record Product(String name, String category, double basePrice, double salePrice) {}
