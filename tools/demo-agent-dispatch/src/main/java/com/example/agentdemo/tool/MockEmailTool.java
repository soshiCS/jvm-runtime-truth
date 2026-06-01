package com.example.agentdemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

import java.util.Map;

/**
 * Test/mock email tool: silently discards all emails.
 *
 * Intended ONLY for test environments. Must never be active in production.
 *
 * @Order(Ordered.LOWEST_PRECEDENCE) — supposed to have lowest priority.
 *
 * THE BUG: ToolRegistry iterates the Spring-ordered List<AgentTool> and
 * calls HashMap.put() for each. SendgridEmailTool (Order=1) is first,
 * but MockEmailTool (Order=LOWEST) is last — and last-write-wins in HashMap.
 * MockEmailTool silently replaces SendgridEmailTool in the "send_email" slot.
 *
 * No exception is thrown. The agent thinks emails are being sent.
 * Emails are silently dropped.
 */
@Component
@Order(Integer.MAX_VALUE)
public class MockEmailTool implements AgentTool {

    private static final Logger log = LoggerFactory.getLogger(MockEmailTool.class);

    @Override
    public String toolName() { return "send_email"; }

    @Override
    public String description() {
        return "TEST ONLY: simulates email sending without actually sending. For CI/test.";
    }

    @Override
    public String execute(Map<String, Object> input) {
        String to = String.valueOf(input.getOrDefault("to", "(none)"));
        String subject = String.valueOf(input.getOrDefault("subject", "(no subject)"));
        log.warn("[MockEmailTool] *** MOCK: email NOT sent *** to={} subject={}", to, subject);
        return "MOCK_NOOP: email discarded (MockEmailTool active — no email sent to " + to + ")";
    }
}
