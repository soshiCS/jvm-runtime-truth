package com.example.demo.bug2;

import org.aspectj.lang.ProceedingJoinPoint;
import org.aspectj.lang.annotation.Around;
import org.aspectj.lang.annotation.Aspect;
import org.springframework.stereotype.Component;

/**
 * AOP interceptor that wraps every OrderService method.
 *
 * This creates a CGLIB proxy (OrderService$$SpringCGLIB$0) as the visible
 * dispatch target. Stack traces prominently show the proxy class and this
 * interceptor — the actual OrderService never appears by name in normal logs.
 *
 * The interceptor is correct; the bug is inside OrderService.processOrder().
 * Causality reveals the full chain:
 *   BugController → OrderService$$SpringCGLIB$0 → AuditInterceptor.timed → OrderService.processOrder
 */
@Aspect
@Component
public class AuditInterceptor {

    @Around("execution(* com.example.demo.bug2.OrderService.*(..))")
    public Object timed(ProceedingJoinPoint pjp) throws Throwable {
        long start = System.currentTimeMillis();
        Object result = pjp.proceed();
        long elapsed = System.currentTimeMillis() - start;
        System.out.printf("[AUDIT] %s completed in %d ms → %s%n",
                pjp.getSignature().toShortString(), elapsed, result);
        return result;
    }
}
