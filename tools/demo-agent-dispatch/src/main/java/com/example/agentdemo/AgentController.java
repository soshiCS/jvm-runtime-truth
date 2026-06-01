package com.example.agentdemo;

import com.example.agentdemo.agent.AgentService;
import com.example.agentdemo.registry.ToolRegistry;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.Map;

@RestController
public class AgentController {

    private final AgentService agentService;
    private final ToolRegistry toolRegistry;

    public AgentController(AgentService agentService, ToolRegistry toolRegistry) {
        this.agentService = agentService;
        this.toolRegistry = toolRegistry;
    }

    /**
     * Run an agent task.
     * GET /run?task=onboard_user
     * Shows which tool implementations were actually invoked.
     */
    @GetMapping("/run")
    public Map<String, Object> run(
            @RequestParam(defaultValue = "onboard_user") String task) {

        AgentService.AgentTaskResult result = agentService.runTask(task);

        Map<String, Object> response = new LinkedHashMap<>();
        response.put("task", result.task());
        response.put("mock_tool_activated", result.mockToolActivated());

        if (result.mockToolActivated()) {
            response.put("WARNING",
                "MockEmailTool is active — emails are being silently discarded. " +
                "This tool should only be present in test environments.");
        }

        var steps = result.steps().stream().map(s -> {
            Map<String, Object> m = new LinkedHashMap<>();
            m.put("tool_name",   s.toolName());
            m.put("impl_class",  s.implClass());
            m.put("result",      s.result());
            if (s.error() != null) m.put("error", s.error());
            return m;
        }).toList();
        response.put("steps", steps);

        return response;
    }

    /**
     * Show the current tool registry state.
     * GET /registry
     * Reveals which implementation is registered for each tool name.
     */
    @GetMapping("/registry")
    public Map<String, Object> registry() {
        Map<String, Object> response = new LinkedHashMap<>();
        response.put("current_registry", toolRegistry.getRegistryState());
        response.put("registration_log",
            toolRegistry.getRegistrationLog().stream().map(r -> {
                Map<String, Object> m = new LinkedHashMap<>();
                m.put("tool_name",       r.toolName());
                m.put("registered",      r.registeredClass());
                m.put("overwrote",       r.overwrittenClass());
                return m;
            }).toList()
        );
        response.put("bug_explanation",
            "ToolRegistry uses HashMap.put() which is last-write-wins. " +
            "Spring delivers tools in @Order sequence (1, 2, MAX_VALUE). " +
            "MockEmailTool (@Order=MAX_VALUE) is last in the iteration, " +
            "so it overwrites SendgridEmailTool in the 'send_email' slot. " +
            "The fix is putIfAbsent() to make first-write-wins.");
        return response;
    }
}
