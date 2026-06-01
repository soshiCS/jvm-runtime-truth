package com.example.realbugs.bug2;

/**
 * Bug 2 — proxy type mismatch: @Retryable gets JDK proxy, @Transactional gets CGLIB.
 *
 * Source: https://github.com/spring-projects/spring-framework/issues/35286
 *
 * PaymentService implements this interface. Because it implements an interface,
 * Spring Retry creates a JDK dynamic proxy (jdk.proxy1.$Proxy<N>).
 * Meanwhile, @Transactional (even on the same bean) creates a CGLIB proxy
 * (PaymentServiceImpl$$SpringCGLIB$$0).
 *
 * The inconsistency becomes visible when:
 *   (a) Code casts the Spring-injected bean to PaymentServiceImpl (ClassCastException)
 *   (b) Code checks proxy class name to understand wrapping
 *   (c) Runtime Truth's generated_class records show two different proxy types
 */
public interface PaymentService {
    String charge(String customerId, double amount);
    String refund(String customerId, double amount);
}
