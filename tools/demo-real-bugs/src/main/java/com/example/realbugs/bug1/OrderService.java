package com.example.realbugs.bug1;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Propagation;
import org.springframework.transaction.annotation.Transactional;
import org.springframework.transaction.support.TransactionSynchronizationManager;

/**
 * Bug 1 — @Transactional self-invocation bypass.
 *
 * Source: https://github.com/spring-projects/spring-framework/issues/27534
 *
 * processOrder() calls this.saveAuditEntry() via 'this' reference.
 * saveAuditEntry() is @Transactional(REQUIRES_NEW), so it should run in a
 * separate transaction. But because the call goes to 'this' (the raw bean),
 * not through the Spring CGLIB proxy, the @Transactional interceptor is never
 * invoked for saveAuditEntry().
 *
 * Expected: saveAuditEntry() starts a new transaction.
 * Actual:   saveAuditEntry() runs inside processOrder()'s transaction (or none).
 *
 * Runtime Truth detects this because:
 *   callsite_target for saveAuditEntry() shows:
 *     target = OrderService.saveAuditEntry   (direct — the raw class)
 *   NOT:
 *     target = OrderService$$SpringCGLIB$$0.saveAuditEntry  (through proxy)
 */
@Service
public class OrderService {

    private static final Logger log = LoggerFactory.getLogger(OrderService.class);

    // -----------------------------------------------------------------------
    // Public entry point — called via HTTP
    @Transactional
    public String processOrder(String orderId) {
        log.info("[Bug1] processOrder({}) — tx active: {}", orderId,
                 TransactionSynchronizationManager.isActualTransactionActive());

        // THE BUG: calling via 'this' bypasses the proxy.
        // saveAuditEntry() should get its own REQUIRES_NEW transaction, but won't.
        this.saveAuditEntry(orderId, "PROCESSED");

        return txStatus("processOrder");
    }

    // -----------------------------------------------------------------------
    // Should start a REQUIRES_NEW transaction but won't when called internally.
    @Transactional(propagation = Propagation.REQUIRES_NEW)
    public String saveAuditEntry(String orderId, String status) {
        boolean hasTx = TransactionSynchronizationManager.isActualTransactionActive();
        String txName = TransactionSynchronizationManager.getCurrentTransactionName();
        log.info("[Bug1] saveAuditEntry({}, {}) — tx active: {}, tx name: {}",
                 orderId, status, hasTx, txName);
        return txStatus("saveAuditEntry");
    }

    // -----------------------------------------------------------------------
    // Correct version — calls through the proxy via self-injection.
    // Included for comparison; not exercised by the buggy endpoint.
    @Transactional
    public String processOrderCorrect(OrderService proxy, String orderId) {
        log.info("[Bug1] processOrderCorrect({}) — tx active: {}", orderId,
                 TransactionSynchronizationManager.isActualTransactionActive());
        // This call DOES go through the proxy → REQUIRES_NEW is honoured.
        proxy.saveAuditEntry(orderId, "PROCESSED_CORRECT");
        return txStatus("processOrderCorrect");
    }

    // -----------------------------------------------------------------------
    private String txStatus(String method) {
        boolean active = TransactionSynchronizationManager.isActualTransactionActive();
        String name = TransactionSynchronizationManager.getCurrentTransactionName();
        return method + ": tx=" + active + " name=" + name;
    }
}
