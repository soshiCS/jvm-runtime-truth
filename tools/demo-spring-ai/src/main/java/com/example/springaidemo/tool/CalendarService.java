package com.example.springaidemo.tool;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.ai.tool.annotation.Tool;
import org.springframework.stereotype.Component;

@Component
public class CalendarService {

    private static final Logger log = LoggerFactory.getLogger(CalendarService.class);

    @Tool(description = "Schedule a calendar meeting and send invites to attendees.")
    public String scheduleMeeting(String title, String attendees) {
        log.info("[CalendarService] scheduling: {} with {}", title, attendees);
        return "CALENDAR_OK: meeting '" + title + "' scheduled for " + attendees;
    }
}
