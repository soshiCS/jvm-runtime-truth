package com.example.proxydemo;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

@RestController
public class OrderController {

    private final OrderService orderService;

    public OrderController(OrderService orderService) {
        this.orderService = orderService;
    }

    /**
     * Shows both call paths side by side.
     * The bug is visible: buggy_path shows both tx names are identical.
     */
    @GetMapping("/compare")
    public Map<String, Object> compare() {
        CallRecord buggy   = orderService.processOrder("ORDER-001");
        CallRecord correct = orderService.processOrderCorrect("ORDER-001");

        Map<String, Object> result = new LinkedHashMap<>();

        result.put("buggy_path", pathMap(buggy,
            "saveAuditEntry ran in processOrder's transaction — REQUIRES_NEW ignored",
            "saveAuditEntry should have its own transaction — REQUIRES_NEW honored"));

        result.put("correct_path", pathMap(correct,
            "saveAuditEntry ran in processOrderCorrect's transaction — BUG PRESENT",
            "saveAuditEntry has its own transaction — REQUIRES_NEW honored"));

        result.put("verdict", buggy.proxyBypassed()
            ? "BUG CONFIRMED: self-invocation bypasses Spring proxy"
            : "No bypass detected");

        return result;
    }

    private static Map<String, Object> pathMap(CallRecord r,
                                                String bugDesc,
                                                String okDesc) {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put("call_expression",      r.callExpression());
        m.put("outer_tx",             r.callerTxName());
        m.put("saveAuditEntry_tx",    r.saveAuditEntryTxName());
        m.put("proxy_bypassed",       r.proxyBypassed());
        m.put("conclusion",           r.proxyBypassed() ? bugDesc : okDesc);
        return m;
    }
}
