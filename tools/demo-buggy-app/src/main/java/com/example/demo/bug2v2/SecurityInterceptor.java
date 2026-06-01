package com.example.demo.bug2v2;

import org.aspectj.lang.ProceedingJoinPoint;
import org.aspectj.lang.annotation.Around;
import org.aspectj.lang.annotation.Aspect;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

@Aspect
@Component
@Order(1)
public class SecurityInterceptor {

    @Around("execution(* com.example.demo.bug2v2.OrderService2.*(..))")
    public Object checkAccess(ProceedingJoinPoint pjp) throws Throwable {
        System.out.printf("[SECURITY] Access check for %s%n", pjp.getSignature().getName());
        Object result = pjp.proceed();
        System.out.printf("[SECURITY] Access granted%n");
        return result;
    }
}
