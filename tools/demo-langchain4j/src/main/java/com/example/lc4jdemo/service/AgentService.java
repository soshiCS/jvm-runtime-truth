package com.example.lc4jdemo.service;

import com.example.lc4jdemo.tool.CalendarService;
import com.example.lc4jdemo.tool.DatabaseService;
import com.example.lc4jdemo.tool.MockEmailService;
import com.example.lc4jdemo.tool.ProductionEmailService;
import dev.langchain4j.agent.tool.ToolExecutionRequest;
import dev.langchain4j.service.tool.DefaultToolExecutor;
import dev.langchain4j.service.tool.ToolExecutor;
import dev.langchain4j.service.tool.ToolService;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Exercises LangChain4j's DefaultToolExecutor and ToolService dispatch path directly.
 * No real LLM needed — we fabricate ToolExecutionRequests as an LLM would produce them.
 */
@Service
public class AgentService {

    private static final Logger log = LoggerFactory.getLogger(AgentService.class);

    private final ProductionEmailService productionEmailService;
    private final MockEmailService mockEmailService;
    private final DatabaseService databaseService;
    private final CalendarService calendarService;

    public AgentService(ProductionEmailService productionEmailService,
                        MockEmailService mockEmailService,
                        DatabaseService databaseService,
                        CalendarService calendarService) {
        this.productionEmailService = productionEmailService;
        this.mockEmailService = mockEmailService;
        this.databaseService = databaseService;
        this.calendarService = calendarService;
    }

    /**
     * Dispatch sendEmail -> ProductionEmailService via DefaultToolExecutor.
     */
    public Map<String, Object> runWithProductionTools() {
        log.info("[AgentService] runWithProductionTools");
        return execute(productionEmailService);
    }

    /**
     * Dispatch sendEmail -> MockEmailService via DefaultToolExecutor.
     */
    public Map<String, Object> runWithMockTools() {
        log.info("[AgentService] runWithMockTools");
        return execute(mockEmailService);
    }

    private Map<String, Object> execute(Object emailImpl) {
        List<Map<String, Object>> results = new ArrayList<>();

        // Build tool requests as an LLM would produce
        ToolExecutionRequest emailRequest = ToolExecutionRequest.builder()
                .id("call_1")
                .name("sendEmail")
                .arguments("{\"to\":\"alice@example.com\",\"subject\":\"Welcome\",\"body\":\"Hi Alice\"}")
                .build();

        ToolExecutionRequest dbRequest = ToolExecutionRequest.builder()
                .id("call_2")
                .name("queryDatabase")
                .arguments("{\"query\":\"SELECT * FROM users WHERE id=1\"}")
                .build();

        ToolExecutionRequest calRequest = ToolExecutionRequest.builder()
                .id("call_3")
                .name("scheduleMeeting")
                .arguments("{\"title\":\"Onboarding\",\"attendees\":\"alice@example.com\"}")
                .build();

        // Execute each tool via DefaultToolExecutor — the actual LC4J dispatch path
        results.add(dispatchViaDefaultToolExecutor(emailRequest, emailImpl));
        results.add(dispatchViaDefaultToolExecutor(dbRequest, databaseService));
        results.add(dispatchViaDefaultToolExecutor(calRequest, calendarService));

        Map<String, Object> result = new LinkedHashMap<>();
        result.put("email_impl_class", emailImpl.getClass().getSimpleName());
        result.put("tool_results", results);
        return result;
    }

    private Map<String, Object> dispatchViaDefaultToolExecutor(ToolExecutionRequest request, Object toolObject) {
        // DefaultToolExecutor — the actual LangChain4j reflection-based dispatch
        // This is exactly what ToolService calls internally
        DefaultToolExecutor executor = new DefaultToolExecutor(toolObject, request);
        String result = executor.execute(request, "memoryId");

        Map<String, Object> r = new LinkedHashMap<>();
        r.put("tool_name", request.name());
        r.put("result", result);
        return r;
    }

    /**
     * Also test via ToolService.executeWithErrorHandling() — same path that AiServices uses.
     */
    public Map<String, Object> runViaToolService(boolean useMock) {
        log.info("[AgentService] runViaToolService useMock={}", useMock);

        Object emailImpl = useMock ? mockEmailService : productionEmailService;

        // Build executor map as ToolService would
        Map<String, ToolExecutor> executors = new LinkedHashMap<>();
        executors.put("sendEmail", new DefaultToolExecutor(emailImpl,
                ToolExecutionRequest.builder().id("x").name("sendEmail").arguments("{}").build()));
        executors.put("queryDatabase", new DefaultToolExecutor(databaseService,
                ToolExecutionRequest.builder().id("x").name("queryDatabase").arguments("{}").build()));

        ToolExecutionRequest req = ToolExecutionRequest.builder()
                .id("call_1")
                .name("sendEmail")
                .arguments("{\"to\":\"alice@example.com\",\"subject\":\"Welcome\",\"body\":\"Hi Alice\"}")
                .build();

        var result = ToolService.executeWithErrorHandling(req, executors.get("sendEmail"),
                null, (e, ctx) -> {
                    throw new RuntimeException(e);
                }, (e, ctx) -> {
                    throw new RuntimeException(e);
                });

        Map<String, Object> r = new LinkedHashMap<>();
        r.put("email_impl", emailImpl.getClass().getSimpleName());
        r.put("tool_name", req.name());
        r.put("result", result.resultText());
        return r;
    }
}
