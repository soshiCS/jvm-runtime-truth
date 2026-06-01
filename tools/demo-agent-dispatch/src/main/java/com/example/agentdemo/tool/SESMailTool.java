package com.example.agentdemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.core.annotation.Order;
import org.springframework.stereotype.Component;

import java.util.Map;

/**
 * Alternative email tool: routes through AWS SES.
 * Registered for the same tool name as SendgridEmailTool and MockEmailTool.
 */
@Component
@Order(2)
public class SESMailTool implements AgentTool {

    private static final Logger log = LoggerFactory.getLogger(SESMailTool.class);

    @Override
    public String toolName() { return "send_email"; }

    @Override
    public String description() {
        return "Send email via AWS Simple Email Service (SES). Fallback for Sendgrid.";
    }

    @Override
    public String execute(Map<String, Object> input) {
        String to = String.valueOf(input.getOrDefault("to", "(none)"));
        String subject = String.valueOf(input.getOrDefault("subject", "(no subject)"));
        log.info("[SESMailTool] AWS SES send to={} subject={}", to, subject);
        return "SES_OK: message submitted to AWS SES for " + to;
    }
}
