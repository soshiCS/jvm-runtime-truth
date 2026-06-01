package com.example.springaidemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.tool.annotation.Tool;
import org.springframework.stereotype.Component;

@Component
public class DatabaseService {

    private static final Logger log = LoggerFactory.getLogger(DatabaseService.class);

    @Tool(description = "Execute a read-only SQL query against the application database.")
    public String queryDatabase(String query) {
        log.info("[DatabaseService] executing: {}", query);
        return "DB_RESULT: [{\"id\":1,\"name\":\"Alice\"}] for: " + query;
    }
}
