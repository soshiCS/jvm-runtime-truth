package com.example.agentdemo.agent;

import com.example.agentdemo.registry.ToolRegistry;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

import java.util.*;

/**
 * Simulates an LLM agent executing a task by selecting and invoking tools.
 *
 * In a real framework (Spring AI, LangChain4j) this class would call an LLM
 * to select tool names and parameters. Here the selection is deterministic for
 * reproducibility, but the dispatch path is identical.
 *
 * Task "onboard_user" selects: send_email, query_database, schedule_meeting.
 * All three are dispatched via ToolRegistry.dispatch() → reflection invoke.
 */
@Service
public class AgentService {

    private static final Logger log = LoggerFactory.getLogger(AgentService.class);

    private final ToolRegistry registry;

    public AgentService(ToolRegistry registry) {
        this.registry = registry;
    }

    public AgentTaskResult runTask(String task) {
        log.info("[AgentService] running task: {}", task);

        List<ToolRegistry.DispatchResult> steps = new ArrayList<>();

        if ("onboard_user".equals(task)) {
            // Step 1: query existing user data
            steps.add(registry.dispatch("query_database",
                Map.of("query", "SELECT * FROM users WHERE status='pending'")));

            // Step 2: send welcome email — THIS IS WHERE THE BUG MANIFESTS
            steps.add(registry.dispatch("send_email", Map.of(
                "to",      "alice@example.com",
                "subject", "Welcome to our platform",
                "body",    "Hi Alice, your account is ready."
            )));

            // Step 3: schedule onboarding call
            steps.add(registry.dispatch("schedule_meeting", Map.of(
                "title",     "Onboarding call with Alice",
                "attendees", "alice@example.com, support@example.com"
            )));
        } else {
            steps.add(registry.dispatch("query_database",
                Map.of("query", "SELECT 1")));
        }

        boolean hasMockTool = steps.stream()
            .anyMatch(r -> r.implClass() != null && r.implClass().contains("Mock"));

        return new AgentTaskResult(task, steps, hasMockTool);
    }

    public record AgentTaskResult(
        String task,
        List<ToolRegistry.DispatchResult> steps,
        boolean mockToolActivated
    ) {}
}
