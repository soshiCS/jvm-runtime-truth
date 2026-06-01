package com.example.agentdemo.tool;

import java.util.Map;

/**
 * Contract implemented by every agent tool.
 *
 * Frameworks like Spring AI and LangChain4j use a similar interface —
 * a named callable that accepts a generic input map and returns a result.
 * Invocation is typically via reflection so the framework can discover and
 * call implementations without compile-time knowledge of the class.
 */
public interface AgentTool {
    /** Tool name as it appears in the LLM's tool-call JSON. */
    String toolName();
    /** Execute the tool. Invoked via reflection by the ToolRegistry. */
    String execute(Map<String, Object> input);
    /** Human-readable description (used for tool-selection prompt). */
    String description();
}
