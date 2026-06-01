package com.example.demo.bug2v2;

import com.example.demo.bug2.Order;
import org.springframework.stereotype.Service;

@Service
public class OrderService2 {

    private static final double DOMESTIC_TAX_RATE      = 0.08;
    private static final double INTERNATIONAL_TAX_RATE = 0.15;

    public double processOrder(Order order) {
        double taxRate = "INTERNATIONAL".equals(order.type())
                ? DOMESTIC_TAX_RATE
                : INTERNATIONAL_TAX_RATE;

        double total = order.amount() * (1 + taxRate);
        System.out.printf("[ORDER-V2] %s type=%s amount=%.2f taxRate=%.2f total=%.2f%n",
                order.orderId(), order.type(), order.amount(), taxRate, total);
        return total;
    }
}
