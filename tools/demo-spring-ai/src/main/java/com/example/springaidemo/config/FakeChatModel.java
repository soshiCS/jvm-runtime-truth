package com.example.springaidemo.config;

import org.springframework.ai.chat.messages.ToolResponseMessage;
import org.springframework.ai.chat.messages.UserMessage;
import org.springframework.ai.chat.model.ChatModel;
import org.springframework.ai.chat.model.ChatResponse;
import org.springframework.ai.chat.model.Generation;
import org.springframework.ai.chat.messages.AssistantMessage;
import org.springframework.ai.chat.prompt.Prompt;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

import java.util.List;
import java.util.UUID;

/**
 * A fake ChatModel that simulates an LLM returning tool calls.
 *
 * First call: returns tool calls for sendEmail + queryDatabase + scheduleMeeting.
 * Subsequent calls (after tool results fed back): returns a final text response.
 *
 * This exercises the full Spring AI tool dispatch path without needing an API key.
 */
@Configuration
public class FakeChatModel {

    @Bean
    public ChatModel chatModel() {
        return new StatefulFakeModel();
    }

    public static class StatefulFakeModel implements ChatModel {

        @Override
        public ChatResponse call(Prompt prompt) {
            boolean hasToolResults = prompt.getInstructions().stream()
                    .anyMatch(m -> m instanceof ToolResponseMessage);

            if (hasToolResults) {
                // Final response after tool execution
                return new ChatResponse(List.of(
                        new Generation(new AssistantMessage(
                                "Task completed. Email sent, database queried, and meeting scheduled."))
                ));
            } else {
                // First call: request tool invocations
                return new ChatResponse(List.of(
                        new Generation(new AssistantMessage(
                                "",
                                java.util.Map.of(),
                                List.of(
                                        new AssistantMessage.ToolCall(
                                                UUID.randomUUID().toString(), "function",
                                                "sendEmail",
                                                "{\"to\":\"alice@example.com\",\"subject\":\"Welcome\",\"body\":\"Hi Alice\"}"),
                                        new AssistantMessage.ToolCall(
                                                UUID.randomUUID().toString(), "function",
                                                "queryDatabase",
                                                "{\"query\":\"SELECT * FROM users WHERE id=1\"}"),
                                        new AssistantMessage.ToolCall(
                                                UUID.randomUUID().toString(), "function",
                                                "scheduleMeeting",
                                                "{\"title\":\"Onboarding\",\"attendees\":\"alice@example.com\"}")
                                )
                        ))
                ));
            }
        }
    }
}
