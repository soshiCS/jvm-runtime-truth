package com.example.demo.bug2v2;

import org.aspectj.lang.ProceedingJoinPoint;
import org.aspectj.lang.annotation.Around;
import org.aspectj.lang.annotation.Aspect;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

@Aspect
@Component
@Order(2)
public class MetricsInterceptor {

    @Around("execution(* com.example.demo.bug2v2.OrderService2.*(..))")
    public Object record(ProceedingJoinPoint pjp) throws Throwable {
        long start = System.nanoTime();
        Object result = pjp.proceed();
        long elapsed = (System.nanoTime() - start) / 1_000_000;
        System.out.printf("[METRICS] %s completed in %d ms%n", pjp.getSignature().getName(), elapsed);
        return result;
    }
}
