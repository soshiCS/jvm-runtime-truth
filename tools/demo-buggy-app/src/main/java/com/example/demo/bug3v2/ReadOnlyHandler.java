package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class ReadOnlyHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "READ_ONLY"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=read-only,permissions=[read]}";
    }
}
