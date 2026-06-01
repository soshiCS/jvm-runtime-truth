package com.example.agentdemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

import java.util.Map;

/**
 * Production email tool: routes through Sendgrid.
 *
 * @Order(1) — highest priority. The developer intended this to "win" in the
 * tool registry when multiple tools share the same name. The bug: the registry
 * uses HashMap.put() in iteration order, so the LAST registration wins,
 * not the highest-priority one.
 */
@Component
@Order(1)
public class SendgridEmailTool implements AgentTool {

    private static final Logger log = LoggerFactory.getLogger(SendgridEmailTool.class);

    @Override
    public String toolName() { return "send_email"; }

    @Override
    public String description() {
        return "Send a transactional email via Sendgrid. Use for production email delivery.";
    }

    @Override
    public String execute(Map<String, Object> input) {
        String to = String.valueOf(input.getOrDefault("to", "(none)"));
        String subject = String.valueOf(input.getOrDefault("subject", "(no subject)"));
        log.info("[SendgridEmailTool] PRODUCTION send to={} subject={}", to, subject);
        return "SENDGRID_OK: message queued for " + to + " via Sendgrid API";
    }
}
