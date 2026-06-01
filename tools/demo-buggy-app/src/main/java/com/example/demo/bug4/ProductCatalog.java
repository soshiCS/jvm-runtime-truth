package com.example.demo.bug4;

import org.springframework.stereotype.Service;

import java.util.HashMap;
import java.util.Map;
import java.util.function.Function;

/**
 * Computes the final customer-facing price for a product using a per-category lambda.
 *
 * BUG: The "CLEARANCE" pricer lambda applies the 50% discount to basePrice instead
 * of salePrice. Clearance items already have a reduced salePrice; halving basePrice
 * instead gives a higher (wrong) price. No exception is thrown — the number is
 * simply too high.
 *
 * Example: basePrice=100, salePrice=60.
 *   Correct:  0.5 × 60  = 30.00
 *   Buggy:    0.5 × 100 = 50.00  ← what the system returns
 *
 * Causality reveals the invokedynamic dispatch from calculatePrice → hidden class
 * containing the CLEARANCE lambda body, making the exact computation visible.
 */
@Service
public class ProductCatalog {

    private final Map<String, Function<Product, Double>> pricers = new HashMap<>();

    public ProductCatalog() {
        pricers.put("REGULAR",   p -> p.salePrice());
        pricers.put("PREMIUM",   p -> p.salePrice() * 1.20);
        pricers.put("CLEARANCE", p -> p.basePrice() * 0.50);  // ← BUG: should be p.salePrice() * 0.50
        pricers.put("SEASONAL",  p -> p.salePrice() * 0.85);
    }

    /**
     * Dispatches to the category-specific lambda via invokedynamic.
     * The causality hook fires on the Function.apply() call — the hidden class
     * holding the lambda body is the actual dispatch target.
     */
    public double calculatePrice(Product product) {
        Function<Product, Double> pricer = pricers.get(product.category());
        if (pricer == null) {
            throw new IllegalArgumentException("No pricer for category: " + product.category());
        }
        double price = pricer.apply(product);  // ← invokedynamic dispatch; causality hook fires here
        System.out.printf("[CATALOG] %s category=%s basePrice=%.2f salePrice=%.2f finalPrice=%.2f%n",
                product.name(), product.category(), product.basePrice(), product.salePrice(), price);
        return price;
    }
}
