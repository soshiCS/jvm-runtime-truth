package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class SupportHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "SUPPORT"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=support,permissions=[read,write:support,ticket]}";
    }
}
