package com.example.realbugs.bug2;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.retry.annotation.Backoff;
import org.springframework.retry.annotation.Retryable;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

/**
 * Bug 2 implementation.
 *
 * charge() is @Retryable — Spring Retry will use a JDK proxy (interface-based).
 * refund() is @Transactional — Spring TX will use CGLIB proxy.
 *
 * When Spring Boot's spring.aop.proxy-target-class=true (default), @Transactional
 * gets CGLIB. But @Retryable (from Spring Retry, separate BeanPostProcessor) uses
 * JDK proxy for interface-implementing beans unless explicitly configured.
 *
 * The visible symptom: if any caller does `(PaymentServiceImpl) bean`, it gets
 * a ClassCastException because the injected bean is a JDK proxy, not the CGLIB
 * proxy they might expect.
 */
@Service
public class PaymentServiceImpl implements PaymentService {

    private static final Logger log = LoggerFactory.getLogger(PaymentServiceImpl.class);

    private int chargeAttempts = 0;

    // @Retryable → Spring Retry creates a JDK proxy for this interface method
    @Override
    @Retryable(maxAttempts = 3, backoff = @Backoff(delay = 10))
    public String charge(String customerId, double amount) {
        chargeAttempts++;
        log.info("[Bug2] charge({}, {}) attempt #{}", customerId, amount, chargeAttempts);
        if (chargeAttempts < 3) {
            throw new RuntimeException("Payment gateway timeout (simulated)");
        }
        chargeAttempts = 0;
        return "CHARGED: " + customerId + " $" + amount;
    }

    // @Transactional → Spring TX creates CGLIB proxy
    @Override
    @Transactional
    public String refund(String customerId, double amount) {
        log.info("[Bug2] refund({}, {})", customerId, amount);
        return "REFUNDED: " + customerId + " $" + amount;
    }
}
