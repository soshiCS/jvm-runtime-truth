package com.example.lc4jdemo.tool;

import dev.langchain4j.agent.tool.Tool;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

@Component
public class DatabaseService {

    private static final Logger log = LoggerFactory.getLogger(DatabaseService.class);

    @Tool("Execute a read-only SQL query.")
    public String queryDatabase(String query) {
        log.info("[DatabaseService] executing: {}", query);
        return "DB_RESULT: [{\"id\":1,\"name\":\"Alice\"}] for: " + query;
    }
}
