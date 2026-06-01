package com.example.springaidemo.service;

import com.example.springaidemo.tool.CalendarService;
import com.example.springaidemo.tool.DatabaseService;
import com.example.springaidemo.tool.MockEmailService;
import com.example.springaidemo.tool.ProductionEmailService;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.chat.messages.AssistantMessage;
import org.springframework.ai.chat.messages.UserMessage;
import org.springframework.ai.chat.model.ChatResponse;
import org.springframework.ai.chat.model.Generation;
import org.springframework.ai.chat.prompt.Prompt;
import org.springframework.ai.model.tool.DefaultToolCallingChatOptions;
import org.springframework.ai.model.tool.DefaultToolCallingManager;
import org.springframework.ai.model.tool.ToolCallingManager;
import org.springframework.ai.tool.ToolCallback;
import org.springframework.ai.tool.method.MethodToolCallbackProvider;
import org.springframework.stereotype.Service;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * Exercises the Spring AI tool dispatch path directly via DefaultToolCallingManager.
 *
 * This bypasses the ChatModel loop (which lives inside each model implementation)
 * and instead directly invokes the same code path that OpenAiChatModel / OllamaChatModel
 * call after receiving tool call responses from the LLM.
 *
 * This is the exact code path that logs during tool dispatch.
 */
@Service
public class AgentService {

    private static final Logger log = LoggerFactory.getLogger(AgentService.class);

    private final ToolCallingManager toolCallingManager;
    private final ProductionEmailService productionEmailService;
    private final MockEmailService mockEmailService;
    private final DatabaseService databaseService;
    private final CalendarService calendarService;

    public AgentService(ToolCallingManager toolCallingManager,
                        ProductionEmailService productionEmailService,
                        MockEmailService mockEmailService,
                        DatabaseService databaseService,
                        CalendarService calendarService) {
        this.toolCallingManager = toolCallingManager;
        this.productionEmailService = productionEmailService;
        this.mockEmailService = mockEmailService;
        this.databaseService = databaseService;
        this.calendarService = calendarService;
    }

    /**
     * Execute tools via DefaultToolCallingManager with PRODUCTION email service.
     * sendEmail -> ProductionEmailService.sendEmail
     */
    public Map<String, Object> runWithProductionTools() {
        log.info("[AgentService] runWithProductionTools — dispatching via DefaultToolCallingManager");
        return executeWithTools(productionEmailService, databaseService, calendarService);
    }

    /**
     * Execute tools via DefaultToolCallingManager with MOCK email service.
     * sendEmail -> MockEmailService.sendEmail
     */
    public Map<String, Object> runWithMockTools() {
        log.info("[AgentService] runWithMockTools — dispatching via DefaultToolCallingManager");
        return executeWithTools(mockEmailService, databaseService, calendarService);
    }

    /**
     * Try to register BOTH email implementations — Spring AI throws at construction.
     */
    public Map<String, Object> runWithDuplicateTools() {
        log.info("[AgentService] runWithDuplicateTools — EXPECT EXCEPTION");
        try {
            return executeWithTools(productionEmailService, mockEmailService, databaseService, calendarService);
        } catch (Exception e) {
            Map<String, Object> result = new LinkedHashMap<>();
            result.put("exception_type", e.getClass().getName());
            result.put("exception_message", e.getMessage());
            return result;
        }
    }

    private Map<String, Object> executeWithTools(Object... toolObjects) {
        // Build tool callbacks from annotated method objects
        ToolCallback[] callbacks = MethodToolCallbackProvider.builder()
                .toolObjects(toolObjects)
                .build()
                .getToolCallbacks();

        // Log which callbacks were registered — do class names appear here?
        log.info("[AgentService] registered {} tool callbacks", callbacks.length);
        for (ToolCallback cb : callbacks) {
            log.info("[AgentService]   callback: name={} definition.name={}",
                    cb.getClass().getSimpleName(), cb.getToolDefinition().name());
        }

        // Build a Prompt with ToolCallingChatOptions that carries the callbacks
        var options = DefaultToolCallingChatOptions.builder()
                .toolCallbacks(List.of(callbacks))
                .build();

        Prompt prompt = new Prompt(
                List.of(new UserMessage("Send email, query db, schedule meeting.")),
                options
        );

        // Fabricate a ChatResponse containing tool calls — exactly what an LLM would return
        List<AssistantMessage.ToolCall> toolCalls = new ArrayList<>();
        toolCalls.add(new AssistantMessage.ToolCall(
                UUID.randomUUID().toString(), "function",
                "sendEmail",
                "{\"to\":\"alice@example.com\",\"subject\":\"Welcome\",\"body\":\"Hi Alice\"}"));
        toolCalls.add(new AssistantMessage.ToolCall(
                UUID.randomUUID().toString(), "function",
                "queryDatabase",
                "{\"query\":\"SELECT * FROM users WHERE id=1\"}"));
        toolCalls.add(new AssistantMessage.ToolCall(
                UUID.randomUUID().toString(), "function",
                "scheduleMeeting",
                "{\"title\":\"Onboarding\",\"attendees\":\"alice@example.com\"}"));

        AssistantMessage assistantMessage = new AssistantMessage("", Map.of(), toolCalls);
        ChatResponse fakeResponse = new ChatResponse(List.of(new Generation(assistantMessage)));

        // THIS IS THE CRITICAL CALL — same as OpenAiChatModel / OllamaChatModel call it
        // DefaultToolCallingManager.executeToolCalls() is where Spring AI logs tool execution
        var executionResult = this.toolCallingManager.executeToolCalls(prompt, fakeResponse);

        Map<String, Object> result = new LinkedHashMap<>();
        result.put("callbacks_registered", callbacks.length);
        result.put("tool_names", List.of(callbacks).stream()
                .map(cb -> cb.getToolDefinition().name()).toList());
        result.put("conversation_history_size", executionResult.conversationHistory().size());
        result.put("return_direct", executionResult.returnDirect());

        // Extract tool results from conversation history
        var toolMessages = executionResult.conversationHistory().stream()
                .filter(m -> m.getClass().getSimpleName().contains("ToolResponse"))
                .toList();
        result.put("tool_results", toolMessages.stream().map(Object::toString).toList());

        return result;
    }
}
