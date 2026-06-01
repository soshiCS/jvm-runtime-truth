package com.example.lc4jdemo.tool;

import dev.langchain4j.agent.tool.Tool;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

@Component
public class ProductionEmailService {

    private static final Logger log = LoggerFactory.getLogger(ProductionEmailService.class);

    @Tool("Send an email to a recipient via Sendgrid.")
    public String sendEmail(String to, String subject, String body) {
        log.info("[ProductionEmailService] sending email to {}", to);
        return "EMAIL_SENT via Sendgrid to " + to + " subject=" + subject;
    }
}
