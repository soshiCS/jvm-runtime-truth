package com.example.demo.bug2v2;

import org.aspectj.lang.ProceedingJoinPoint;
import org.aspectj.lang.annotation.Around;
import org.aspectj.lang.annotation.Aspect;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

@Aspect
@Component
@Order(3)
public class TransactionInterceptor {

    @Around("execution(* com.example.demo.bug2v2.OrderService2.*(..))")
    public Object transact(ProceedingJoinPoint pjp) throws Throwable {
        System.out.printf("[TX] Begin transaction for %s%n", pjp.getSignature().getName());
        try {
            Object result = pjp.proceed();
            System.out.printf("[TX] Commit%n");
            return result;
        } catch (Throwable t) {
            System.out.printf("[TX] Rollback: %s%n", t.getMessage());
            throw t;
        }
    }
}
