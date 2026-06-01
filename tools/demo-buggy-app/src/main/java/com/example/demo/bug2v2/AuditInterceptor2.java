package com.example.demo.bug2v2;

import org.aspectj.lang.ProceedingJoinPoint;
import org.aspectj.lang.annotation.Around;
import org.aspectj.lang.annotation.Aspect;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

@Aspect
@Component
@Order(4)
public class AuditInterceptor2 {

    @Around("execution(* com.example.demo.bug2v2.OrderService2.*(..))")
    public Object timed(ProceedingJoinPoint pjp) throws Throwable {
        long t0 = System.currentTimeMillis();
        Object result = pjp.proceed();
        System.out.printf("[AUDIT-V2] %s(..) completed in %d ms -> %s%n",
                pjp.getSignature().getName(), System.currentTimeMillis() - t0, result);
        return result;
    }
}
