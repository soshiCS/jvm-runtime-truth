package com.example.demo.bug3v2;

import com.example.demo.bug3.Request;
import org.springframework.stereotype.Component;

@Component
public class ApiClientHandler implements RequestHandlerV2 {
    @Override
    public String type() { return "API_CLIENT"; }

    @Override
    public String handle(Request request) {
        return "PROFILE:{userId=" + request.userId() + ",role=api-client,permissions=[read,write:api]}";
    }
}
