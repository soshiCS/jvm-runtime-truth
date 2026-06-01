package com.example.lc4jdemo.tool;

import dev.langchain4j.agent.tool.Tool;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

@Component
public class CalendarService {

    private static final Logger log = LoggerFactory.getLogger(CalendarService.class);

    @Tool("Schedule a calendar meeting and send invites.")
    public String scheduleMeeting(String title, String attendees) {
        log.info("[CalendarService] scheduling: {} with {}", title, attendees);
        return "CALENDAR_OK: meeting '" + title + "' scheduled for " + attendees;
    }
}
