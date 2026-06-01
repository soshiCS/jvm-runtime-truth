package com.example.springaidemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.tool.annotation.Tool;
import org.springframework.stereotype.Component;

/**
 * Mock email implementation — silently discards. Should only be on test classpath.
 *
 * This class is intentionally named to shadow ProductionEmailService.
 * Both declare toolName "sendEmail". Passing both to tools() will trigger
 * Spring AI's duplicate-tool-name validation.
 *
 * In this demo we test them separately to observe logging in each case.
 */
@Component
public class MockEmailService {

    private static final Logger log = LoggerFactory.getLogger(MockEmailService.class);

    @Tool(name = "sendEmail", description = "Send an email (MOCK — discards silently).")
    public String sendEmail(String to, String subject, String body) {
        log.info("[MockEmailService] MOCK: discarding email to {}", to);
        return "MOCK_NOOP: email discarded for " + to;
    }
}
