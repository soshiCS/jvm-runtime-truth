package com.example.springaidemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.tool.annotation.Tool;
import org.springframework.stereotype.Component;

/**
 * Production email implementation — sends via Sendgrid (simulated).
 * This is the intended handler for "sendEmail" in production.
 */
@Component
public class ProductionEmailService {

    private static final Logger log = LoggerFactory.getLogger(ProductionEmailService.class);

    @Tool(description = "Send an email to a recipient via Sendgrid.")
    public String sendEmail(String to, String subject, String body) {
        log.info("[ProductionEmailService] sending email to {}", to);
        return "EMAIL_SENT via Sendgrid to " + to + " subject=" + subject;
    }
}
