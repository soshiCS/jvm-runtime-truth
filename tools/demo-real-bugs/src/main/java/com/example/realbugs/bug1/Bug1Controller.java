package com.example.realbugs.bug1;

import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.Map;

/**
 * Bug 1 — @Transactional self-invocation bypass.
 * Endpoint: GET /bug1/process?orderId=ORDER-123
 */
@RestController
@RequestMapping("/bug1")
public class Bug1Controller {

    private final OrderService orderService;

    @Autowired
    public Bug1Controller(OrderService orderService) {
        this.orderService = orderService;
    }

    @GetMapping("/process")
    public Map<String, String> process(@RequestParam(defaultValue = "ORDER-001") String orderId) {
        // This call goes through the Spring proxy → processOrder() is @Transactional
        String result = orderService.processOrder(orderId);

        // Also call saveAuditEntry directly through the proxy (correct path)
        String directResult = orderService.saveAuditEntry(orderId, "DIRECT");

        return Map.of(
            "bug",         "Spring @Transactional self-invocation bypass",
            "buggy_call",  result,
            "direct_call", directResult
        );
    }
}
