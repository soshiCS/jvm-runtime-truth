package com.example.agentdemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

import java.util.Map;

/** Unambiguous tool: only one implementation of "query_database". */
@Component
public class DatabaseQueryTool implements AgentTool {

    private static final Logger log = LoggerFactory.getLogger(DatabaseQueryTool.class);

    @Override
    public String toolName() { return "query_database"; }

    @Override
    public String description() { return "Execute a read-only SQL query against the application database."; }

    @Override
    public String execute(Map<String, Object> input) {
        String query = String.valueOf(input.getOrDefault("query", "SELECT 1"));
        log.info("[DatabaseQueryTool] executing: {}", query);
        return "DB_RESULT: [{\"row\":1,\"value\":\"example\"}] for query: " + query;
    }
}
