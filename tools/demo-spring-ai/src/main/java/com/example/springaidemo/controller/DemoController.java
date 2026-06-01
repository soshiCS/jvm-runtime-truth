package com.example.springaidemo.controller;

import com.example.springaidemo.service.AgentService;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

import java.util.Map;

@RestController
public class DemoController {

    private final AgentService agentService;

    public DemoController(AgentService agentService) {
        this.agentService = agentService;
    }

    @GetMapping("/run/production")
    public Map<String, Object> runProduction() {
        return agentService.runWithProductionTools();
    }

    @GetMapping("/run/mock")
    public Map<String, Object> runMock() {
        return agentService.runWithMockTools();
    }

    @GetMapping("/run/duplicate")
    public Map<String, Object> runDuplicate() {
        return agentService.runWithDuplicateTools();
    }
}
