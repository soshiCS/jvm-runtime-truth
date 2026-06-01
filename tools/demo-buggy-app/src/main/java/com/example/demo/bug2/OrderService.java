package com.example.demo.bug2;

import org.springframework.stereotype.Service;

/**
 * Calculates the final order total including tax.
 *
 * BUG: INTERNATIONAL orders should use INTERNATIONAL_TAX_RATE (0.15),
 * but the branch condition is inverted — they receive DOMESTIC_TAX_RATE (0.08).
 * The AuditInterceptor wraps every call and logs correctly, making the proxy
 * chain prominent in every stack trace while the mis-rate is invisible.
 *
 * Normal log output looks fine: "Order processed: ORD-001 total=108.00" —
 * amount×1.08 is plausible. The symptom (wrong tax) only appears when
 * comparing the total against the expected INTERNATIONAL rate.
 */
@Service
public class OrderService {

    private static final double DOMESTIC_TAX_RATE      = 0.08;
    private static final double INTERNATIONAL_TAX_RATE = 0.15;

    /**
     * Returns the final order total after applying the correct tax rate.
     * Hook fires on the invokevirtual dispatch that reaches this method
     * through the CGLIB proxy → AuditInterceptor chain.
     */
    public double processOrder(Order order) {
        // BUG IS HERE: condition is reversed.
        // "INTERNATIONAL".equals(order.type()) is true for international orders
        // but the branch assigns DOMESTIC rate when it should assign INTERNATIONAL.
        double taxRate = "INTERNATIONAL".equals(order.type())
                ? DOMESTIC_TAX_RATE          // ← BUG: should be INTERNATIONAL_TAX_RATE
                : INTERNATIONAL_TAX_RATE;    // ← BUG: should be DOMESTIC_TAX_RATE

        double total = order.amount() * (1.0 + taxRate);
        System.out.printf("[ORDER] %s type=%s amount=%.2f taxRate=%.2f total=%.2f%n",
                order.orderId(), order.type(), order.amount(), taxRate, total);
        return total;
    }
}
