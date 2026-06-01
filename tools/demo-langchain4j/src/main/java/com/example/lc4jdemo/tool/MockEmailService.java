package com.example.lc4jdemo.tool;

import dev.langchain4j.agent.tool.Tool;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

@Component
public class MockEmailService {

    private static final Logger log = LoggerFactory.getLogger(MockEmailService.class);

    @Tool("Send an email (MOCK — silently discards).")
    public String sendEmail(String to, String subject, String body) {
        log.info("[MockEmailService] MOCK: discarding email to {}", to);
        return "MOCK_NOOP: email discarded for " + to;
    }
}
