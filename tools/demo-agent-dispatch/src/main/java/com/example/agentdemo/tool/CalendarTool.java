package com.example.agentdemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

import java.util.Map;

/** Unambiguous tool: only one implementation of "schedule_meeting". */
@Component
public class CalendarTool implements AgentTool {

    private static final Logger log = LoggerFactory.getLogger(CalendarTool.class);

    @Override
    public String toolName() { return "schedule_meeting"; }

    @Override
    public String description() { return "Schedule a calendar meeting and send invites."; }

    @Override
    public String execute(Map<String, Object> input) {
        String title = String.valueOf(input.getOrDefault("title", "Meeting"));
        String attendees = String.valueOf(input.getOrDefault("attendees", ""));
        log.info("[CalendarTool] scheduling: {} with {}", title, attendees);
        return "CALENDAR_OK: meeting '" + title + "' scheduled for " + attendees;
    }
}
