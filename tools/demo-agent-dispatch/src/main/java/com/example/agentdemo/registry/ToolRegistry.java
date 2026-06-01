package com.example.agentdemo.registry;

import com.example.agentdemo.tool.AgentTool;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

import java.lang.reflect.Method;
import java.util.*;

/**
 * Central tool registry used by the agent to look up and invoke tools.
 *
 * THE BUG IS HERE:
 *
 * The registry accepts a Spring-ordered List<AgentTool>. Spring respects
 * @Order annotations and delivers higher-priority tools first (Order=1 before
 * Order=MAX_VALUE). The developer assumed this meant "first in list = winner."
 *
 * But HashMap.put() is last-write-wins. Iterating in priority order and
 * calling put() means the LOWEST-priority tool (MockEmailTool, Order=MAX_VALUE)
 * is put last and overwrites all previous entries for "send_email".
 *
 * Result: MockEmailTool is active in the registry. Emails are silently dropped.
 *
 * The fix is trivial: putIfAbsent() instead of put(). But without knowing which
 * class is actually in the registry, the bug is invisible from source code alone
 * (three classes all declare toolName()="send_email"; which one won?).
 */
@Component
public class ToolRegistry {

    private static final Logger log = LoggerFactory.getLogger(ToolRegistry.class);

    private final Map<String, AgentTool> registry = new HashMap<>();
    private final List<RegistrationRecord> registrationLog = new ArrayList<>();

    public ToolRegistry(List<AgentTool> allTools) {
        log.info("[ToolRegistry] registering {} tools", allTools.size());
        for (AgentTool tool : allTools) {
            AgentTool previous = registry.get(tool.toolName());
            // BUG: put() overwrites — last registration wins, not first (highest-priority)
            registry.put(tool.toolName(), tool);
            registrationLog.add(new RegistrationRecord(
                tool.toolName(),
                tool.getClass().getSimpleName(),
                previous == null ? null : previous.getClass().getSimpleName()
            ));
            if (previous != null) {
                log.warn("[ToolRegistry] OVERWRITE: '{}' replaced {} with {}",
                    tool.toolName(), previous.getClass().getSimpleName(),
                    tool.getClass().getSimpleName());
            } else {
                log.info("[ToolRegistry] registered '{}' -> {}", tool.toolName(),
                    tool.getClass().getSimpleName());
            }
        }
    }

    /**
     * Dispatch a tool call. Uses reflection to invoke the execute() method —
     * same pattern as Spring AI FunctionCallback and LangChain4j ToolExecutor.
     * This gives RT a reflection_method_invoke callsite record pointing to the
     * actual implementation class.
     */
    public DispatchResult dispatch(String toolName, Map<String, Object> input) {
        AgentTool tool = registry.get(toolName);
        if (tool == null) {
            return new DispatchResult(toolName, null, "ERROR: unknown tool '" + toolName + "'", null);
        }

        String implClass = tool.getClass().getName();
        try {
            // Reflection-based invocation — mirrors how LangChain4j/Spring AI dispatch
            Method method = tool.getClass().getMethod("execute", Map.class);
            String result = (String) method.invoke(tool, input);
            log.info("[ToolRegistry] dispatched '{}' -> {} -> result={}", toolName, implClass, result);
            return new DispatchResult(toolName, implClass, result, null);
        } catch (Exception e) {
            log.error("[ToolRegistry] dispatch error for '{}': {}", toolName, e.getMessage());
            return new DispatchResult(toolName, implClass, null, e.getMessage());
        }
    }

    public Map<String, String> getRegistryState() {
        Map<String, String> state = new LinkedHashMap<>();
        registry.forEach((name, tool) -> state.put(name, tool.getClass().getSimpleName()));
        return state;
    }

    public List<RegistrationRecord> getRegistrationLog() {
        return Collections.unmodifiableList(registrationLog);
    }

    public record RegistrationRecord(String toolName, String registeredClass, String overwrittenClass) {}
    public record DispatchResult(String toolName, String implClass, String result, String error) {}
}
