package com.example.proxydemo;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Propagation;
import org.springframework.transaction.annotation.Transactional;
import org.springframework.transaction.support.TransactionSynchronizationManager;

/**
 * Demonstrates the Spring @Transactional self-invocation bypass.
 *
 * processOrder() calls this.saveAuditEntry() — the call stays on the raw
 * object, never touching the Spring proxy. The @Transactional(REQUIRES_NEW)
 * annotation on saveAuditEntry is silently ignored.
 *
 * processOrderCorrect() calls self.saveAuditEntry() — 'self' is the Spring
 * proxy injected by the container. The REQUIRES_NEW interceptor fires.
 */
@Service
public class OrderService {

    private static final Logger log = LoggerFactory.getLogger(OrderService.class);

    // Self-injection: Spring injects the proxy, not the raw object.
    // Used by processOrderCorrect() to route the call through the proxy.
    @Autowired
    private OrderService self;

    // -----------------------------------------------------------------------
    // BUGGY: 'this' reference bypasses the Spring proxy.
    // saveAuditEntry() never gets its own REQUIRES_NEW transaction.
    @Transactional
    public CallRecord processOrder(String orderId) {
        log.info("[processOrder] starting — tx={}", txName());
        String auditTx = this.saveAuditEntry(orderId);   // <-- BUG
        log.info("[processOrder] done     — tx={}", txName());
        return new CallRecord("processOrder", txName(), auditTx, "this.saveAuditEntry()");
    }

    // -----------------------------------------------------------------------
    // CORRECT: 'self' is the Spring-managed proxy. The interceptor fires.
    @Transactional
    public CallRecord processOrderCorrect(String orderId) {
        log.info("[processOrderCorrect] starting — tx={}", txName());
        String auditTx = self.saveAuditEntry(orderId);   // <-- CORRECT
        log.info("[processOrderCorrect] done     — tx={}", txName());
        return new CallRecord("processOrderCorrect", txName(), auditTx, "self.saveAuditEntry()");
    }

    // -----------------------------------------------------------------------
    // Meant to run in a NEW transaction (REQUIRES_NEW).
    // Only works when called through the Spring proxy.
    @Transactional(propagation = Propagation.REQUIRES_NEW)
    public String saveAuditEntry(String orderId) {
        String tx = txName();
        log.info("[saveAuditEntry] tx={}", tx);
        return tx;
    }

    // -----------------------------------------------------------------------
    private static String txName() {
        String n = TransactionSynchronizationManager.getCurrentTransactionName();
        return n != null ? n : "(none)";
    }
}
